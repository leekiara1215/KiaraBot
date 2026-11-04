#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

#include <pqxx/pqxx>

#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"
#include "typed_work_checks.hpp"

namespace kiarabot::infrastructure::postgres::test {
namespace {
namespace t = core::target;
void check(bool value, const char* reason) {
  if (!value)
    throw std::runtime_error(reason);
}
template <typename Value> Value take(t::Outcome<Value> result) {
  check(std::holds_alternative<Value>(result), "P05 management receipt storage failure");
  return std::move(std::get<Value>(result));
}
std::string quote(std::string_view value) {
  std::string result = "'";
  for (const char ch : value) {
    if (ch == '\'' || ch == '\\')
      result += '\\';
    result += ch;
  }
  return result + '\'';
}
std::string dsn(const DatabaseConfig& c) {
  return "host=" + quote(c.host) + " port=" + std::to_string(c.port) +
         " dbname=" + quote(c.database) + " user=" + quote(c.user) +
         " password=" + quote(c.password) +
         " sslmode=verify-full channel_binding=require sslrootcert=" + quote(c.root_certificate);
}
void commit(t::RuntimeUnitOfWork& work) {
  check(work.commit() == t::CommitResult::Committed, "P05 receipt transaction failed");
}
void expire(const DatabaseConfig& config, const std::string& key) {
  pqxx::connection connection(dsn(config));
  pqxx::work tx(connection);
  tx.exec("UPDATE kiarabot.command_receipts SET created_at=created_at-691200000000000,"
          "retain_until=retain_until-691200000000000 WHERE subject='management-actor' AND "
          "idempotency_key=$1",
          pqxx::params{key});
  tx.commit();
}
void assert_no_orphans(const DatabaseConfig& config) {
  pqxx::connection connection(dsn(config));
  pqxx::read_transaction tx(connection);
  check(tx.exec("SELECT count(*) FROM kiarabot.runtime_management_receipts m LEFT JOIN "
                "kiarabot.command_receipts c USING(result_id) WHERE c.result_id IS NULL")
                .one_row()[0]
                .as<int>() == 0,
        "P05 orphan supplement survived");
}
// Only the pure dependency is injected: business writes and audit use the production PG UoW.
class FailingSecrets final : public t::RuntimeSecretPort {
public:
  bool fail_credential = true;
  t::Outcome<std::string> bootstrap_credential() override {
    if (fail_credential)
      return t::rejected(t::ErrorCode::UpstreamUnavailable);
    return std::string(64, 'a');
  }
  t::Outcome<std::array<std::byte, 32>> opaque_secret() override {
    return t::rejected(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<t::Sha256Digest> sha256(std::string_view) override {
    return t::rejected(t::ErrorCode::UpstreamUnavailable);
  }
};
class UnusedIds final : public t::RuntimeIdPort {
public:
  t::Outcome<t::ResourceId> account_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<t::InstallationId> installation_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<t::InstanceId> instance_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<t::InstanceId> adapter_instance_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<t::ConnectionId> connection_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<t::OperationId> operation_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<t::RuntimeEpoch> runtime_epoch() override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<std::string> snapshot_id() override {
    return t::rejected(t::ErrorCode::Internal);
  }
};
class OnlineAllow final : public t::RuntimeAuthorizationPort {
public:
  bool available() const noexcept override {
    return true;
  }
  t::Outcome<t::RuntimeAuthorizationDecision>
  check(const t::RuntimeAuthorizationRequest& request) override {
    return t::RuntimeAuthorizationDecision{true,
                                           request.authentication.subject,
                                           request.permission,
                                           request.scope,
                                           {1},
                                           std::chrono::system_clock::now(),
                                           {}};
  }
};
void check_pure_port_failure(t::RuntimeUnitOfWorkFactory& factory,
                             const DatabaseConfig& migration) {
  const t::ResourceId account_id{"95000000-0000-4000-8000-000000000001"};
  const t::InstallationId installation_id{"95000000-0000-4000-8000-000000000002"};
  const t::InstanceId instance_id{"95000000-0000-4000-8000-000000000003"};
  auto work = take(factory.begin());
  take(work->accounts().save(
      {account_id, "pure-fault", "remote", "original", {1}, t::LifecycleState::Active}, {}));
  take(work->installations().save({installation_id, {"pure-type"}, {1}, t::LifecycleState::Active},
                                  {}));
  take(work->instances().save({instance_id,
                               installation_id,
                               {"pure-type"},
                               {1},
                               {0},
                               {},
                               {1},
                               t::LifecycleState::Active,
                               t::ResourceId{"pure-host"},
                               t::DesiredState::Stopped},
                              {}));
  commit(*work);
  work.reset();
  UnusedIds ids;
  FailingSecrets secrets;
  OnlineAllow authorization;
  auto application =
      take(t::make_runtime_resource_application({factory, ids, secrets, authorization, {}}));
  t::CommandContext context;
  const t::SubjectId subject{"95000000-0000-4000-8000-000000000004"};
  const auto now = std::chrono::system_clock::now();
  context.authentication = {subject,
                            t::PlatformPeer{t::Service::Control, subject},
                            t::Service::Runtime,
                            {},
                            {},
                            {1},
                            now - std::chrono::seconds{1},
                            now + std::chrono::seconds{60}};
  context.trace_id = "95000000-0000-4000-8000-000000000005";
  context.deadline = now + std::chrono::seconds{30};
  context.idempotency_key = "pure-port-failure-key";
  auto require_rejection = [&](const auto& result) {
    check(std::holds_alternative<t::Failure>(result) &&
              std::get<t::Failure>(result).code == t::ErrorCode::UpstreamUnavailable,
          "P05 pure-port rejection changed by rejection audit failure");
  };
  for (const bool fail_credential : {true, false}) {
    secrets.fail_credential = fail_credential;
    context.request_id = fail_credential ? "95000000-0000-4000-8000-000000000006"
                                         : "95000000-0000-4000-8000-000000000007";
    context.method = "kiarabot.v1.RuntimeService/BindAdapter";
    require_rejection(
        application->bind_adapter({context, account_id, {"95000000-0000-4000-8000-000000000008"}}));
    work = take(factory.begin());
    check(!take(work->adapter_bindings().find(account_id)) &&
              !take(work->operations().find({context.request_id})),
          "P05 pure bootstrap/hash failure committed binding or operation");
    work->rollback();
  }
  // Deny only the rejection-audit INSERT after startup, then restore the exact runtime grant.
  {
    pqxx::connection connection(dsn(migration));
    pqxx::work tx(connection);
    tx.exec("REVOKE INSERT ON kiarabot.audit_entries FROM " +
            tx.quote_name(migration.runtime_role));
    tx.commit();
  }
  context.request_id = "95000000-0000-4000-8000-000000000009";
  context.method = "kiarabot.v1.RuntimeService/SetDesiredState";
  context.expected_version = t::ResourceVersion{1};
  require_rejection(
      application->apply_desired_state({context, instance_id, t::DesiredState::Running}));
  {
    pqxx::connection connection(dsn(migration));
    pqxx::work tx(connection);
    tx.exec("GRANT INSERT ON kiarabot.audit_entries TO " + tx.quote_name(migration.runtime_role));
    tx.commit();
  }
  work = take(factory.begin());
  const auto instance = take(work->instances().find(instance_id));
  const t::ReceiptKey key{subject,
                          context.method,
                          {t::ResourceKind::Instance, {instance_id.value}},
                          context.idempotency_key};
  check(instance && instance->version.value == 1 && instance->desired == t::DesiredState::Stopped &&
            !take(work->operations().find({context.request_id})) &&
            !take(work->receipts().find(key)) && !take(work->management_receipt(key)),
        "P05 pure host hash failure committed partial state");
  work->rollback();
}
} // namespace

void check_runtime_management_receipts(const DatabaseConfig& runtime,
                                       const DatabaseConfig& migration,
                                       const std::filesystem::path& root,
                                       std::uint16_t proxy_port) {
  auto factory = take(open_runtime(runtime));
  check_pure_port_failure(*factory, migration);
  auto work = take(factory->begin());
  const auto now = take(work->current_time());
  const t::ResourceRef resource{t::ResourceKind::Account, {"management-account"}};
  t::BotAccount account{resource.id, "receipt-test",           "receipt-remote", "original",
                        {1},         t::LifecycleState::Active};
  const t::ReceiptKey key{
      {"management-actor"}, "kiarabot.v1.RuntimeService/DisableAccount", resource, "original"};
  const t::RuntimeManagementReceipt supplement{
      1, {t::Service::Runtime, {"immutable-original-operation"}, resource}, {}};
  auto receipt = t::CommandReceipt{key, {}, account, now, now + std::chrono::hours{168}};
  take(work->accounts().save(account, {}));
  take(work->receipts().insert(receipt, {1000}));
  take(work->save_management_receipt(key, supplement));
  commit(*work);
  work.reset();
  factory.reset();
  factory = take(open_runtime(runtime));
  work = take(factory->begin());
  const auto loaded = take(work->management_receipt(key));
  check(loaded && loaded->operation == supplement.operation && !loaded->committed_state,
        "P05 restart lost immutable original operation");
  auto another_subject = key;
  another_subject.subject = {"management-other-actor"};
  check(!take(work->receipts().find(another_subject)) &&
            !take(work->management_receipt(another_subject)),
        "P05 another subject read original receipt");
  work->rollback();

  // Same parent key cannot replace its immutable supplement; conflicting arguments never write.
  work = take(factory->begin());
  check(take(work->receipts().insert(receipt, {1000})).duplicate, "P05 duplicate parent changed");
  check(std::holds_alternative<t::Failure>(work->save_management_receipt(key, supplement)),
        "P05 duplicate supplement overwritten");
  check(work->commit() == t::CommitResult::RolledBack, "P05 duplicate write partially committed");
  work = take(factory->begin());
  auto conflict = receipt;
  conflict.fingerprint.bytes[0] = std::byte{1};
  check(std::holds_alternative<t::Failure>(work->receipts().insert(conflict, {1000})),
        "P05 same key changed fingerprint accepted");
  work->rollback();

  // A failure after the parent, and another after both receipt relations, roll back business CAS.
  for (const bool after_supplement : {false, true}) {
    work = take(factory->begin());
    auto failed = receipt;
    failed.key.idempotency_key = after_supplement ? "fail-after-supplement" : "fail-after-parent";
    auto changed = account;
    changed.version = {2};
    changed.display_name = "must roll back";
    take(work->accounts().save(changed, t::ResourceVersion{1}));
    take(work->receipts().insert(failed, {1000}));
    if (after_supplement) {
      take(work->save_management_receipt(failed.key, supplement));
      check(std::holds_alternative<t::Failure>(
                work->accounts().save(changed, t::ResourceVersion{999})),
            "P05 fault after supplement not injected");
    } else {
      auto wrong = supplement;
      wrong.operation.owner = t::Service::Model;
      check(std::holds_alternative<t::Failure>(work->save_management_receipt(failed.key, wrong)),
            "P05 wrong operation owner accepted");
    }
    check(work->commit() == t::CommitResult::RolledBack,
          "P05 receipt fault committed business write");
    work = take(factory->begin());
    check(!take(work->receipts().find(failed.key)) && !take(work->management_receipt(failed.key)) &&
              take(work->accounts().find(account.id))->version.value == 1,
          "P05 receipt fault left partial durable state");
    work->rollback();
  }

  // Codec covers full uint64 fence/state and nanosecond observed time, including STOPPED intent.
  const t::ResourceRef instance{t::ResourceKind::Instance, {"management-instance"}};
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  t::ResourceState state{
      instance,
      {maximum},
      t::LifecycleState::Disabled,
      t::DesiredState::Stopped,
      t::Fence{{instance.id.value}, {"epoch"}, {"execution"}, {maximum}, {"connection"}, {maximum}},
      {},
      true,
      {maximum},
      {maximum - 1},
      t::ExecutionOutcome::Unknown,
      now,
      t::Service::Runtime};
  auto config_receipt = receipt;
  config_receipt.key = {
      {"management-actor"}, "kiarabot.v1.RuntimeService/UpdateConfig", instance, "state"};
  config_receipt.result = t::InstanceConfiguration{
      {instance.id.value}, {2}, {1}, t::ConfigApplyState::Pending,   "delivery",
      state.fence,         {},  now, {std::byte{0}, std::byte{255}}, {3}};
  const t::RuntimeManagementReceipt config_supplement{
      1, {t::Service::Runtime, {"config-operation"}, instance}, state};
  work = take(factory->begin());
  take(work->receipts().insert(config_receipt, {1000}));
  take(work->save_management_receipt(config_receipt.key, config_supplement));
  commit(*work);
  work = take(factory->begin());
  const auto recovered = take(work->management_receipt(config_receipt.key));
  check(recovered && recovered->committed_state &&
            recovered->committed_state->version == state.version &&
            recovered->committed_state->desired == state.desired &&
            recovered->committed_state->lifecycle == state.lifecycle &&
            recovered->committed_state->fence == state.fence &&
            recovered->committed_state->registered &&
            recovered->committed_state->desired_config_version == state.desired_config_version &&
            recovered->committed_state->applied_config_version == state.applied_config_version &&
            recovered->committed_state->process_outcome == state.process_outcome &&
            recovered->committed_state->observed_at == now,
        "P05 full state codec changed snapshot");
  work->rollback();

  expire(migration, key.idempotency_key);
  work = take(factory->begin());
  check(!take(work->receipts().find(key)) && !take(work->management_receipt(key)),
        "P05 logically expired supplement replayable before physical pruning");
  auto replacement = supplement;
  replacement.operation.id = {"replacement-operation"};
  take(work->receipts().insert(receipt, {1000}));
  take(work->save_management_receipt(key, replacement));
  commit(*work);
  work = take(factory->begin());
  check(take(work->management_receipt(key))->operation == replacement.operation,
        "P05 expired-key replacement retained stale supplement");
  work->rollback();
  expire(migration, key.idempotency_key);
  work = take(factory->begin());
  check(take(work->receipts().prune_expired(now, 100)) >= 1, "P05 expired parent not pruned");
  commit(*work);
  assert_no_orphans(migration);
  {
    pqxx::connection connection(dsn(migration));
    pqxx::read_transaction tx(connection);
    check(tx.exec("SELECT count(*) FROM kiarabot.runtime_management_receipts WHERE operation_id IN "
                  "('immutable-original-operation','replacement-operation')")
                  .one_row()[0]
                  .as<int>() == 0,
          "P05 parent expiry did not cascade supplement");
  }
  work.reset();
  factory.reset();

  // TLS stays end-to-end at PG. The owned relay discards the actual successful COMMIT response.
  for (const char* name : {"drop-commit-arm", "drop-commit-ready", "commit-receipt-dropped"}) {
    std::filesystem::remove(root / name);
  }
  auto proxy = runtime;
  proxy.port = proxy_port;
  factory = take(open_runtime(proxy));
  work = take(factory->begin());
  receipt.key.idempotency_key = "unknown-commit";
  take(work->receipts().insert(receipt, {1000}));
  take(work->save_management_receipt(receipt.key, supplement));
  std::ofstream(root / "drop-commit-arm") << "arm";
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (!std::filesystem::exists(root / "drop-commit-ready")) {
    check(std::chrono::steady_clock::now() < deadline, "P05 relay did not arm");
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  check(work->commit() == t::CommitResult::Unknown,
        "P05 real lost commit response was not UNKNOWN");
  work.reset();
  factory.reset();
  factory = take(open_runtime(runtime));
  work = take(factory->begin());
  check(take(work->receipts().find(receipt.key)).has_value() &&
            take(work->management_receipt(receipt.key))->operation == supplement.operation &&
            std::filesystem::exists(root / "commit-receipt-dropped"),
        "P05 unknown commit could not recover original complete receipt");
  work->rollback();
}
} // namespace kiarabot::infrastructure::postgres::test
