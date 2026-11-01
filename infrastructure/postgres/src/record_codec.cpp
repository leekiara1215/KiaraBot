#include "record_codec.hpp"

#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace kiarabot::infrastructure::postgres::detail {
namespace target = core::target;
static_assert(std::is_same_v<target::TimePoint::duration, std::chrono::nanoseconds>);
namespace {

template <typename Enum> Enum enum_field(const pqxx::field& field, int maximum) {
  const auto value = field.as<int>();
  if (value < 0 || value > maximum) {
    throw std::runtime_error("invalid stored enumeration");
  }
  return static_cast<Enum>(value);
}
std::optional<int> optional_enum(const auto& value) {
  return value ? std::optional<int>(static_cast<int>(*value)) : std::nullopt;
}
std::optional<std::string> optional_timestamp(const std::optional<target::TimePoint>& value) {
  return value ? std::optional<std::string>(timestamp(*value)) : std::nullopt;
}
std::string marks(std::size_t count, std::size_t offset = 1) {
  std::string result;
  for (std::size_t index = 0; index < count; ++index) {
    result += (index ? ",$" : "$") + std::to_string(offset + index);
  }
  return result;
}
void put(pqxx::transaction_base& transaction, std::string_view table, std::string_view columns,
         std::int64_t id, pqxx::params values) {
  pqxx::params parameters{id};
  parameters.append(std::move(values));
  transaction.exec("INSERT INTO kiarabot.result_" + std::string(table) + "(result_id," +
                       std::string(columns) + ") VALUES(" + marks(parameters.size()) + ")",
                   parameters);
}
pqxx::row get(pqxx::transaction_base& transaction, std::string_view table, std::int64_t id,
              std::string_view projection = "*") {
  return transaction
      .exec("SELECT " + std::string(projection) + " FROM kiarabot.result_" + std::string(table) +
                " WHERE result_id=$1",
            pqxx::params{id})
      .one_row();
}
} // namespace

std::string timestamp(target::TimePoint value) {
  return std::to_string(
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}
std::string time_projection(std::string_view column) {
  return std::string(column);
}
target::TimePoint time_field(const pqxx::row& row, const char* column) {
  return target::TimePoint(std::chrono::duration_cast<target::TimePoint::duration>(
      std::chrono::nanoseconds(row[column].as<std::int64_t>())));
}
std::optional<target::TimePoint> optional_time(const pqxx::row& row, const char* column) {
  return row[column].is_null() ? std::nullopt
                               : std::optional<target::TimePoint>(time_field(row, column));
}
std::string digest(const target::Sha256Digest& value) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string result = "\\x";
  for (const auto byte : value.bytes) {
    const auto number = std::to_integer<unsigned>(byte);
    result += kDigits[number >> 4];
    result += kDigits[number & 15];
  }
  return result;
}
target::Sha256Digest read_digest(const pqxx::field& field) {
  const auto value = field.as<pqxx::bytes>();
  if (value.size() != 32) {
    throw std::runtime_error("invalid stored digest");
  }
  target::Sha256Digest result;
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    result.bytes[index] = static_cast<std::byte>(value[index]);
  }
  return result;
}
target::Operation read_operation(const pqxx::row& row) {
  target::Operation value;
  value.ref = {enum_field<target::Service>(row["owner"], 5),
               {row["id"].as<std::string>()},
               {enum_field<target::ResourceKind>(row["resource_kind"], 14),
                {row["resource_id"].as<std::string>()}}};
  value.version = {row["version"].as<std::uint64_t>()};
  value.state = enum_field<target::OperationState>(row["state"], 6);
  value.execution_outcome = enum_field<target::ExecutionOutcome>(row["execution_outcome"], 3);
  if (!row["blocked_reason"].is_null()) {
    value.blocked_reason = enum_field<target::ErrorCode>(row["blocked_reason"], 10);
  }
  value.next_attempt = optional_time(row, "next_attempt");
  return value;
}
pqxx::params handoff_params(const target::HandoffContext& value) {
  return {static_cast<int>(value.source),
          value.message.value,
          value.operation.id.value,
          static_cast<int>(value.operation.owner),
          static_cast<int>(value.operation.resource.kind),
          value.operation.resource.id.value,
          value.causation ? std::optional<std::string>(value.causation->value) : std::nullopt,
          static_cast<int>(value.resource.kind),
          value.resource.id.value,
          value.version.value,
          value.actor.value,
          value.authorization_version.value,
          digest(value.payload_hash),
          timestamp(value.created_at),
          timestamp(value.valid_until)};
}
std::string handoff_projection() {
  return "source,message,operation_id,operation_owner,operation_resource_kind,operation_resource_"
         "id,causation,resource_kind,resource_id,resource_version,actor,"
         "authorization_version,payload_hash," +
         time_projection("created_at") + "," + time_projection("valid_until");
}
target::HandoffContext read_handoff(const pqxx::row& row) {
  target::HandoffContext value;
  value.source = enum_field<target::Service>(row["source"], 5);
  value.message = {row["message"].as<std::string>()};
  value.resource = {enum_field<target::ResourceKind>(row["resource_kind"], 14),
                    {row["resource_id"].as<std::string>()}};
  value.operation = {enum_field<target::Service>(row["operation_owner"], 5),
                     {row["operation_id"].as<std::string>()},
                     {enum_field<target::ResourceKind>(row["operation_resource_kind"], 14),
                      {row["operation_resource_id"].as<std::string>()}}};
  if (!row["causation"].is_null()) {
    value.causation = target::MessageId{row["causation"].as<std::string>()};
  }
  value.version = {row["resource_version"].as<std::uint64_t>()};
  value.actor = {row["actor"].as<std::string>()};
  value.authorization_version = {row["authorization_version"].as<std::uint64_t>()};
  value.payload_hash = read_digest(row["payload_hash"]);
  value.created_at = time_field(row, "created_at");
  value.valid_until = time_field(row, "valid_until");
  return value;
}

std::int64_t store_result(pqxx::transaction_base& transaction, const target::ReplayResult& result) {
  const auto id = transaction
                      .exec("INSERT INTO kiarabot.replay_results(kind) VALUES($1) RETURNING id",
                            pqxx::params{static_cast<int>(result.index())})
                      .one_row()[0]
                      .as<std::int64_t>();
  std::visit(
      [&](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, target::EmptySuccess>) {
          return;
        } else if constexpr (std::is_same_v<Value, target::ResourceMutationResult>) {
          put(transaction, "mutations", "resource_kind,resource_id,version", id,
              {static_cast<int>(value.resource.kind), value.resource.id.value,
               value.version.value});
        } else if constexpr (std::is_same_v<Value, target::Operation>) {
          put(transaction, "operations",
              "id,owner,resource_kind,resource_id,version,state,execution_outcome,blocked_reason,"
              "next_attempt",
              id,
              {value.ref.id.value, static_cast<int>(value.ref.owner),
               static_cast<int>(value.ref.resource.kind), value.ref.resource.id.value,
               value.version.value, static_cast<int>(value.state),
               static_cast<int>(value.execution_outcome), optional_enum(value.blocked_reason),
               optional_timestamp(value.next_attempt)});
        } else if constexpr (std::is_same_v<Value, target::Principal>) {
          put(transaction, "principals", "id,kind,binding_kind,binding_id,version,state", id,
              {value.id.value, static_cast<int>(value.kind),
               value.binding ? std::optional<int>(static_cast<int>(value.binding->kind))
                             : std::nullopt,
               value.binding ? std::optional<std::string>(value.binding->id.value) : std::nullopt,
               value.version.value, static_cast<int>(value.state)});
        } else if constexpr (std::is_same_v<Value, target::LoginSession>) {
          put(transaction, "sessions", "id,subject,version,rotation,expires_at,revoked", id,
              {value.id.value, value.subject.value, value.version.value, value.rotation,
               timestamp(value.expires_at), value.revoked});
        } else if constexpr (std::is_same_v<Value, target::ResourceGrant>) {
          put(transaction, "grants", "id,subject,role,version", id,
              {value.id.value, value.subject.value, optional_enum(value.role),
               value.version.value});
          for (std::size_t index = 0; index < value.scopes.size(); ++index) {
            const auto& scope = value.scopes[index];
            transaction.exec("INSERT INTO kiarabot.result_grant_scopes VALUES($1,$2,$3,$4,$5)",
                             pqxx::params{id, index, static_cast<int>(scope.resource.kind),
                                          scope.resource.id.value, scope.permission});
          }
        } else if constexpr (std::is_same_v<Value, target::BotAccount>) {
          put(transaction, "accounts", "id,platform,platform_account_id,display_name,version,state",
              id,
              {value.id.value, value.platform, value.platform_account_id, value.display_name,
               value.version.value, static_cast<int>(value.state)});
        } else if constexpr (std::is_same_v<Value, target::PluginInstallation>) {
          put(transaction, "installations", "id,plugin_type,version,state", id,
              {value.id.value, value.plugin_type.value, value.version.value,
               static_cast<int>(value.state)});
        } else if constexpr (std::is_same_v<Value, target::PluginInstance>) {
          pqxx::params parameters{value.id.value, value.installation.value, value.plugin_type.value,
                                  value.desired_version.value, value.applied_config_version.value};
          if (value.current) {
            const auto& fence = *value.current;
            parameters.append(pqxx::params{fence.instance.value, fence.runtime_epoch.value,
                                           fence.execution.value, fence.execution_generation.value,
                                           fence.connection.value,
                                           fence.connection_generation.value});
          } else {
            parameters.append(pqxx::params{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
          }
          parameters.append(pqxx::params{value.version.value, static_cast<int>(value.state),
                                         value.host ? std::optional<std::string>(value.host->value)
                                                    : std::nullopt});
          put(transaction, "instances",
              "id,installation,plugin_type,desired_version,applied_config_version,fence_instance,"
              "runtime_epoch,execution_id,execution_generation,connection_id,connection_generation,"
              "version,state,host",
              id, std::move(parameters));
        } else if constexpr (std::is_same_v<Value, target::ScheduledTask>) {
          put(transaction, "tasks",
              "owner,task_id,installation,adapter,account,conversation_id,conversation_kind,"
              "payload,run_at,state,version,occurrence_id",
              id,
              {value.key.owner.value, value.key.task_id, value.installation.value,
               value.target.adapter.value, value.target.account.value, value.target.conversation_id,
               static_cast<int>(value.target.kind),
               pqxx::bytes_view(value.payload.data(), value.payload.size()),
               timestamp(value.run_at), static_cast<int>(value.state), value.version.value,
               value.occurrence.value});
        } else if constexpr (std::is_same_v<Value, target::Occurrence>) {
          pqxx::params parameters{value.owner.value,
                                  value.installation.value,
                                  value.task_id,
                                  value.task_version.value,
                                  value.occurrence_id.value,
                                  timestamp(value.due_at),
                                  value.version.value,
                                  static_cast<int>(value.state),
                                  value.accepted_event_id,
                                  optional_timestamp(value.acknowledged_at)};
          parameters.append(handoff_params(value.handoff));
          put(transaction, "occurrences",
              "owner,installation,task_id,task_version,occurrence_id,due_at,version,state,accepted_"
              "event_id,acknowledged_at," +
                  std::string(kHandoffFields),
              id, std::move(parameters));
        } else if constexpr (std::is_same_v<Value, target::ModelIntent>) {
          put(transaction, "model_intents",
              "owner,logical_call_id,model,config_version,request_hash,state,version,created_at,"
              "observed_at",
              id,
              {value.owner.value, value.logical_call_id, value.model.value,
               value.config_version.value, digest(value.request_hash),
               static_cast<int>(value.state), value.version.value, timestamp(value.created_at),
               timestamp(value.observed_at)});
        }
      },
      result);
  return id;
}

target::ReplayResult load_result(pqxx::transaction_base& transaction, std::int64_t id) {
  const auto kind =
      transaction.exec("SELECT kind FROM kiarabot.replay_results WHERE id=$1", pqxx::params{id})
          .one_row()[0]
          .as<int>();
  switch (kind) {
  case 0:
    return target::EmptySuccess{};
  case 1: {
    const auto row = get(transaction, "mutations", id);
    return target::ResourceMutationResult{
        {enum_field<target::ResourceKind>(row["resource_kind"], 14),
         {row["resource_id"].as<std::string>()}},
        {row["version"].as<std::uint64_t>()}};
  }
  case 2:
    return read_operation(
        get(transaction, "operations", id,
            "id,owner,resource_kind,resource_id,version,state,execution_outcome,blocked_reason," +
                time_projection("next_attempt")));
  case 3: {
    const auto row = get(transaction, "principals", id);
    target::Principal value{{row["id"].as<std::string>()},
                            enum_field<target::PrincipalKind>(row["kind"], 3),
                            {},
                            {row["version"].as<std::uint64_t>()},
                            enum_field<target::LifecycleState>(row["state"], 5)};
    if (!row["binding_kind"].is_null()) {
      value.binding = target::ResourceRef{enum_field<target::ResourceKind>(row["binding_kind"], 14),
                                          {row["binding_id"].as<std::string>()}};
    }
    return value;
  }
  case 4: {
    const auto row = get(transaction, "sessions", id,
                         "id,subject,version,rotation,revoked," + time_projection("expires_at"));
    return target::LoginSession{
        {row["id"].as<std::string>()},        {row["subject"].as<std::string>()},
        {row["version"].as<std::uint64_t>()}, row["rotation"].as<std::uint64_t>(),
        time_field(row, "expires_at"),        row["revoked"].as<bool>()};
  }
  case 5: {
    const auto row = get(transaction, "grants", id);
    target::ResourceGrant value{{row["id"].as<std::string>()},
                                {row["subject"].as<std::string>()},
                                {},
                                {},
                                {row["version"].as<std::uint64_t>()}};
    if (!row["role"].is_null()) {
      value.role = enum_field<target::FixedRole>(row["role"], 2);
    }
    const auto scopes = transaction.exec(
        "SELECT * FROM kiarabot.result_grant_scopes WHERE result_id=$1 ORDER BY position",
        pqxx::params{id});
    for (const auto& scope : scopes) {
      value.scopes.push_back({{enum_field<target::ResourceKind>(scope["resource_kind"], 14),
                               {scope["resource_id"].as<std::string>()}},
                              scope["permission"].as<std::string>()});
    }
    return value;
  }
  case 6: {
    const auto row = get(transaction, "accounts", id);
    return target::BotAccount{{row["id"].as<std::string>()},
                              row["platform"].as<std::string>(),
                              row["platform_account_id"].as<std::string>(),
                              row["display_name"].as<std::string>(),
                              {row["version"].as<std::uint64_t>()},
                              enum_field<target::LifecycleState>(row["state"], 5)};
  }
  case 7: {
    const auto row = get(transaction, "installations", id);
    return target::PluginInstallation{{row["id"].as<std::string>()},
                                      {row["plugin_type"].as<std::string>()},
                                      {row["version"].as<std::uint64_t>()},
                                      enum_field<target::LifecycleState>(row["state"], 5)};
  }
  case 8: {
    const auto row = get(transaction, "instances", id);
    target::PluginInstance value{{row["id"].as<std::string>()},
                                 {row["installation"].as<std::string>()},
                                 {row["plugin_type"].as<std::string>()},
                                 {row["desired_version"].as<std::uint64_t>()},
                                 {row["applied_config_version"].as<std::uint64_t>()},
                                 {},
                                 {row["version"].as<std::uint64_t>()},
                                 enum_field<target::LifecycleState>(row["state"], 5),
                                 {}};
    if (!row["fence_instance"].is_null()) {
      value.current = target::Fence{{row["fence_instance"].as<std::string>()},
                                    {row["runtime_epoch"].as<std::string>()},
                                    {row["execution_id"].as<std::string>()},
                                    {row["execution_generation"].as<std::uint64_t>()},
                                    {row["connection_id"].as<std::string>()},
                                    {row["connection_generation"].as<std::uint64_t>()}};
    }
    if (!row["host"].is_null()) {
      value.host = target::ResourceId{row["host"].as<std::string>()};
    }
    return value;
  }
  case 9: {
    const auto row = get(transaction, "tasks", id,
                         "owner,task_id,installation,adapter,account,conversation_id,conversation_"
                         "kind,payload,state,version,occurrence_id," +
                             time_projection("run_at"));
    const auto payload = row["payload"].as<pqxx::bytes>();
    target::ScheduledTask value{
        {{row["owner"].as<std::string>()}, row["task_id"].as<std::string>()},
        {row["installation"].as<std::string>()},
        {{row["adapter"].as<std::string>()},
         {row["account"].as<std::string>()},
         row["conversation_id"].as<std::string>(),
         enum_field<target::ConversationKey::Kind>(row["conversation_kind"], 2)},
        {},
        time_field(row, "run_at"),
        enum_field<target::TaskState>(row["state"], 5),
        {row["version"].as<std::uint64_t>()},
        {row["occurrence_id"].as<std::string>()}};
    for (std::size_t index = 0; index < payload.size(); ++index) {
      value.payload.push_back(static_cast<std::byte>(payload[index]));
    }
    return value;
  }
  case 10: {
    const auto row = get(
        transaction, "occurrences", id,
        "owner,installation,task_id,task_version,occurrence_id,version,state,accepted_event_id," +
            time_projection("due_at") + "," + time_projection("acknowledged_at") + "," +
            handoff_projection());
    return target::Occurrence{{row["owner"].as<std::string>()},
                              {row["installation"].as<std::string>()},
                              row["task_id"].as<std::string>(),
                              {row["task_version"].as<std::uint64_t>()},
                              {row["occurrence_id"].as<std::string>()},
                              time_field(row, "due_at"),
                              read_handoff(row),
                              {row["version"].as<std::uint64_t>()},
                              enum_field<target::OperationState>(row["state"], 6),
                              row["accepted_event_id"].as<std::optional<std::string>>(),
                              optional_time(row, "acknowledged_at")};
  }
  case 11: {
    const auto row = get(transaction, "model_intents", id,
                         "owner,logical_call_id,model,config_version,request_hash,state,version," +
                             time_projection("created_at") + "," + time_projection("observed_at"));
    return target::ModelIntent{
        {row["owner"].as<std::string>()},     row["logical_call_id"].as<std::string>(),
        {row["model"].as<std::string>()},     {row["config_version"].as<std::uint64_t>()},
        read_digest(row["request_hash"]),     enum_field<target::ModelCallState>(row["state"], 3),
        {row["version"].as<std::uint64_t>()}, time_field(row, "created_at"),
        time_field(row, "observed_at")};
  }
  default:
    throw std::runtime_error("invalid stored result kind");
  }
}
} // namespace kiarabot::infrastructure::postgres::detail
