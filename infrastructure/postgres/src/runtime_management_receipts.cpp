#include "runtime_management_receipts.hpp"

#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace t = core::target;
bool valid_key(const t::ReceiptKey& key) {
  return valid_sql_text_fields(
      {key.subject.value, key.method, key.resource.id.value, key.idempotency_key});
}
bool supported(const t::ReceiptKey& key, int kind) {
  return (key.method == "kiarabot.v1.RuntimeService/DisableAccount" && kind == 6 &&
          key.resource.kind == t::ResourceKind::Account) ||
         ((key.method == "kiarabot.v1.RuntimeService/BindAdapter" ||
           key.method == "kiarabot.v1.RuntimeService/UnbindAdapter") &&
          kind == 16 && key.resource.kind == t::ResourceKind::Account) ||
         (key.method == "kiarabot.v1.RuntimeService/SetDesiredState" && kind == 8 &&
          key.resource.kind == t::ResourceKind::Instance) ||
         (key.method == "kiarabot.v1.RuntimeService/UpdateConfig" && kind == 18 &&
          key.resource.kind == t::ResourceKind::Instance);
}
constexpr auto kKey = "c.subject=$1 AND c.method=$2 AND c.resource_kind=$3 AND c.resource_id=$4 "
                      "AND c.idempotency_key=$5";
pqxx::params parameters(const t::ReceiptKey& key) {
  return {key.subject.value, key.method, static_cast<int>(key.resource.kind), key.resource.id.value,
          key.idempotency_key};
}
t::Failure invalid() {
  return t::rejected(t::ErrorCode::Internal, t::Recovery::Diagnose);
}
} // namespace
t::Outcome<std::optional<t::RuntimeManagementReceipt>>
find_management_receipt(SqlWork& work, const t::ReceiptKey& key) {
  std::optional<t::RuntimeManagementReceipt> result;
  auto status = work.run([&](pqxx::transaction_base& tx) -> t::Status {
    if (!valid_key(key))
      return t::rejected(t::ErrorCode::InvalidArgument);
    const auto rows = tx.exec(
        "SELECT m.*,r.kind AS parent_kind FROM kiarabot.runtime_management_receipts m JOIN "
        "kiarabot.command_receipts c USING(result_id) JOIN kiarabot.replay_results r ON "
        "r.id=c.result_id WHERE " +
            std::string(kKey) +
            " AND c.retain_until>(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
        parameters(key));
    if (rows.empty())
      return std::monostate{};
    const auto row = rows.one_row();
    t::RuntimeManagementReceipt value;
    value.format_version = row["format_version"].as<std::uint32_t>();
    value.operation = {t::Service::Runtime,
                       {row["operation_id"].as<std::string>()},
                       {static_cast<t::ResourceKind>(row["resource_kind"].as<int>()),
                        {row["resource_id"].as<std::string>()}}};
    if (value.format_version != 1 || value.operation.resource != key.resource ||
        row["operation_owner"].as<int>() != static_cast<int>(t::Service::Runtime) ||
        row["method"].as<std::string>() != key.method ||
        row["result_kind"].as<int>() != row["parent_kind"].as<int>() ||
        !supported(key, row["parent_kind"].as<int>()))
      return invalid();
    if (!row["state_version"].is_null()) {
      t::ResourceState state;
      state.resource = key.resource;
      state.version = {row["state_version"].as<std::uint64_t>()};
      state.lifecycle = static_cast<t::LifecycleState>(row["lifecycle"].as<int>());
      state.desired = static_cast<t::DesiredState>(row["desired"].as<int>());
      state.registered = row["registered"].as<bool>();
      state.desired_config_version = {row["desired_config_version"].as<std::uint64_t>()};
      state.applied_config_version = {row["applied_config_version"].as<std::uint64_t>()};
      state.process_outcome = static_cast<t::ExecutionOutcome>(row["process_outcome"].as<int>());
      state.observed_at =
          t::TimePoint{std::chrono::nanoseconds{row["observed_at"].as<std::int64_t>()}};
      if (!row["fence_instance"].is_null()) {
        state.fence = t::Fence{{row["fence_instance"].as<std::string>()},
                               {row["fence_epoch"].as<std::string>()},
                               {row["fence_execution"].as<std::string>()},
                               {row["fence_execution_generation"].as<std::uint64_t>()},
                               {row["fence_connection"].as<std::string>()},
                               {row["fence_connection_generation"].as<std::uint64_t>()}};
      }
      value.committed_state = state;
    }
    result = value;
    return std::monostate{};
  });
  if (auto* failure = std::get_if<t::Failure>(&status))
    return *failure;
  return result;
}
t::Status save_management_receipt(SqlWork& work, const t::ReceiptKey& key,
                                  const t::RuntimeManagementReceipt& receipt) {
  return work.run([&](pqxx::transaction_base& tx) -> t::Status {
    if (!valid_key(key) || !valid_sql_text(receipt.operation.id.value))
      return t::rejected(t::ErrorCode::InvalidArgument);
    if (receipt.format_version != 1 || receipt.operation.owner != t::Service::Runtime ||
        receipt.operation.resource != key.resource || receipt.operation.id.value.empty())
      return invalid();
    const auto& state = receipt.committed_state;
    if ((key.resource.kind == t::ResourceKind::Instance) != state.has_value() ||
        (key.resource.kind != t::ResourceKind::Account &&
         key.resource.kind != t::ResourceKind::Instance) ||
        (state && (state->resource != key.resource ||
                   state->source_service != t::Service::Runtime || state->adapter_fence)))
      return invalid();
    auto args = parameters(key);
    args.append(pqxx::params{receipt.format_version, receipt.operation.id.value});
    if (state) {
      args.append(pqxx::params{
          state->version.value, static_cast<int>(state->lifecycle),
          static_cast<int>(state->desired), state->registered, state->desired_config_version.value,
          state->applied_config_version.value, static_cast<int>(state->process_outcome),
          state->observed_at.time_since_epoch().count()});
    } else {
      for (int field = 0; field < 8; ++field)
        args.append(pqxx::params{nullptr});
    }
    if (state && state->fence) {
      const auto& f = *state->fence;
      args.append(pqxx::params{f.instance.value, f.runtime_epoch.value, f.execution.value,
                               f.execution_generation.value, f.connection.value,
                               f.connection_generation.value});
    } else {
      for (int field = 0; field < 6; ++field)
        args.append(pqxx::params{nullptr});
    }
    auto inserted =
        tx.exec("INSERT INTO kiarabot.runtime_management_receipts SELECT "
                "c.result_id,$6,c.method,r.kind,1,$7,c.resource_kind,c.resource_id,$8,$9,$10,$11,$"
                "12,$13,$14,$15,$16,$17,$18,$19,$20,$21 FROM kiarabot.command_receipts c JOIN "
                "kiarabot.replay_results r ON r.id=c.result_id WHERE " +
                    std::string(kKey) + " RETURNING result_id",
                args);
    if (inserted.size() != 1)
      return invalid();
    return std::monostate{};
  });
}
} // namespace kiarabot::infrastructure::postgres::detail
