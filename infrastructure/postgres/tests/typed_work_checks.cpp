#include "typed_work_checks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>
#include <variant>

#include "kiarabot/infrastructure/postgres/factories.hpp"
#include "kiarabot/infrastructure/postgres/security_factory.hpp"

namespace kiarabot::infrastructure::postgres::test {
namespace {
namespace target = core::target;
constexpr auto kHigh = std::uint64_t{1} << 63;
constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
void check(bool value, const char* reason) {
  if (!value) {
    std::cerr << "P03 assertion: " << reason << '\n';
    throw std::runtime_error("typed storage test rejected");
  }
}
template <typename Value> Value take(target::Outcome<Value> result) {
  if (const auto* error = std::get_if<target::Failure>(&result)) {
    std::cerr << "P03 assertion: typed failure code " << static_cast<int>(error->code) << '\n';
  }
  check(std::holds_alternative<Value>(result), "unexpected typed failure");
  return std::move(std::get<Value>(result));
}
template <typename Value>
void expect(const target::Outcome<Value>& result, target::ErrorCode code) {
  check(std::holds_alternative<target::Failure>(result) &&
            std::get<target::Failure>(result).code == code,
        "wrong typed failure");
}
void commit(target::UnitOfWork& work) {
  check(work.commit() == target::CommitResult::Committed, "typed commit failed");
}
void rollback_rejected(target::UnitOfWork& work) {
  check(work.commit() == target::CommitResult::RolledBack, "typed partial write committed");
}

template <typename Factory> void concurrent_probe(Factory& factory) {
  auto active = take(factory.begin());
  std::atomic<bool> busy = false;
  std::thread other([&] {
    const auto result = factory.begin();
    busy = std::holds_alternative<target::Failure>(result) &&
           std::get<target::Failure>(result).code == target::ErrorCode::Conflict;
  });
  other.join();
  check(busy, "concurrent typed begin did not serialize");
  active->rollback();
}
void security(const DatabaseConfig& config) {
  auto factory = take(open_identity_security(config));
  concurrent_probe(*factory);
  for (int pass = 0; pass < 2; ++pass) {
    const std::string id = pass == 0 ? "factory-rollback" : "factory-commit";
    auto work = take(factory->begin());
    check(work->owner() == target::Service::Identity, "security owner not Identity");
    const auto now = take(work->current_time());
    const target::SubjectId subject{id};
    const target::CredentialId credential{id + "-credential"};
    const target::SessionId session{id + "-session"};
    const target::AssignmentId grant{id + "-grant"};
    take(work->principals().save({subject, target::PrincipalKind::Human, {}, {1}}, {}));
    take(work->credentials().save(
        {{credential, {1}},
         subject,
         target::PendingCredentialMaterial{target::CredentialKind::Password, {}},
         target::CredentialState::Pending,
         now,
         {},
         {target::ResourceKind::Catalog, {"catalog"}},
         {}},
        {}));
    take(work->credentials().save({{credential, {2}},
                                   subject,
                                   target::PasswordVerifier{"encoded-verifier-only", {1}},
                                   target::CredentialState::Active,
                                   now,
                                   {},
                                   {target::ResourceKind::Catalog, {"catalog"}},
                                   now + std::chrono::hours{24}},
                                  target::ResourceVersion{1}));
    take(work->credentials().save_login({id, subject, credential, {1}}, {}));
    const target::SubjectId control{id + "-control"};
    const target::CredentialId control_credential{id + "-control-credential"};
    take(work->principals().save(
        {control,
         target::PrincipalKind::PlatformService,
         target::ResourceRef{target::ResourceKind::Principal, {control.value}},
         {1}},
        {}));
    target::Sha256Digest control_fingerprint{};
    control_fingerprint.bytes[0] = static_cast<std::byte>(0x92 + pass);
    take(work->credentials().save(
        {{control_credential, {1}},
         control,
         target::CertificateBinding{control, control_fingerprint, "registered-control-san",
                                    target::Service::Control, now, now + std::chrono::hours{24}},
         target::CredentialState::Active,
         now,
         {},
         {target::ResourceKind::Principal, {control.value}},
         now + std::chrono::hours{24}},
        {}));
    const target::SessionSecurity secured{session,
                                          subject,
                                          {credential, {2}},
                                          {target::TokenPurpose::Access, {id + "-access"}, {}},
                                          {target::TokenPurpose::Refresh, {id + "-refresh"}, {}},
                                          now,
                                          now + std::chrono::minutes{15},
                                          now + std::chrono::hours{24},
                                          1,
                                          {1},
                                          false,
                                          target::SessionControlPeer{control, control_credential}};
    take(work->session_security().save(secured, {}, {}));
    take(work->exact_assignments().save(
        {grant,
         subject,
         {{target::FixedRole::Admin, {target::ResourceKind::Catalog, {id}}}},
         {},
         {1}},
        {}));
    take(work->authorizations().save({subject, {1}, true}, {}));
    const target::ReceiptKey ordinary{
        subject, "factory-success", {target::ResourceKind::Session, {session.value}}, "original"};
    take(work->receipts().insert(
        {ordinary,
         {},
         target::SessionMetadata{session, subject, now, secured.refresh_expires_at, 1, {1}, false},
         now,
         now + std::chrono::hours{168}},
        {100}));
    const target::SecretReceiptScope scope =
        target::LoginReceiptScope{{"control-peer"}, id, "original"};
    take(work->secret_receipts().insert(
        {{scope,
          subject,
          session,
          {credential, {2}},
          target::LoginRequestParameters{},
          1,
          {{"request-key"}, {}},
          now,
          now + std::chrono::hours{23}},
         {{"sealed-key"}, 1, {std::byte{0}}, {std::byte{0}, std::byte{0xff}}}},
        {100}));
    take(work->journal().append_audit({subject,
                                       {target::ResourceKind::Session, {session.value}},
                                       id,
                                       "trace",
                                       "factory-write",
                                       {},
                                       {1},
                                       now}));
    take(work->operations().save(
        {{target::Service::Identity, {id}, {target::ResourceKind::Session, {session.value}}},
         {1},
         target::OperationState::Blocked,
         target::ExecutionOutcome::NotStarted,
         {},
         {}},
        {}));
    check(take(work->session_security().count_unexpired()) > 0,
          "security factory lost current count");
    check(take(work->sessions().find(session))->version.value == 1 &&
              take(work->grants().find(grant))->version.value == 1 &&
              take(work->exact_assignments().for_subject(subject)).size() == 1,
          "security projection not shared with inherited metadata");
    if (pass == 0) {
      take(work->bootstrap().consume({target::BootstrapState::Consumed, {2}, subject, now}, {1}));
      expect(work->credentials().login(std::string("invalid\0login", 13)),
             target::ErrorCode::InvalidArgument);
      rollback_rejected(*work);
    } else {
      commit(*work);
    }
    work = take(factory->begin());
    const bool exists = pass == 1;
    check(take(work->principals().find(subject)).has_value() == exists &&
              take(work->credentials().current(credential)).has_value() == exists &&
              take(work->credentials().login(id)).has_value() == exists &&
              take(work->session_security().find(session)).has_value() == exists &&
              take(work->sessions().find(session)).has_value() == exists &&
              take(work->grants().find(grant)).has_value() == exists &&
              take(work->authorizations().find(subject)).has_value() == exists &&
              take(work->secret_receipts().find(scope)).has_value() == exists &&
              take(work->receipts().find(ordinary)).has_value() == exists &&
              take(work->operations().find({id})).has_value() == exists &&
              take(work->exact_assignments().for_subject(subject)).size() == (exists ? 1U : 0U),
          "security/inherited repositories did not commit or roll back together");
    check(take(work->bootstrap().state()).state == target::BootstrapState::Required,
          "failed security UoW consumed bootstrap");
    work->rollback();
  }
  // Reproduce the application's old double-write sequence without weakening either CAS.
  for (int kind = 0; kind < 3; ++kind) {
    auto work = take(factory->begin());
    const auto stored = take(work->session_security().find({"factory-commit-session"}));
    check(stored.has_value(), "duplicate-write fixture session missing");
    auto changed = *stored;
    std::optional<target::ResourceVersion> expected;
    std::optional<std::uint64_t> expected_rotation;
    if (kind == 0) {
      changed.id = {"duplicate-login-session"};
      changed.access.key.value += "-duplicate-login";
      changed.refresh.key.value += "-duplicate-login";
    } else {
      expected = stored->version;
      expected_rotation = stored->rotation;
      changed.version = {stored->version.value + 1};
      if (kind == 1) {
        ++changed.rotation;
        changed.access.key.value += "-refreshed";
        changed.refresh.key.value += "-refreshed";
      } else {
        changed.revoked = true;
      }
    }
    take(work->session_security().save(changed, expected, expected_rotation));
    const target::LoginSession duplicate{changed.id,
                                         changed.subject,
                                         changed.version,
                                         changed.rotation,
                                         changed.refresh_expires_at,
                                         changed.revoked};
    expect(work->sessions().save(duplicate, expected), target::ErrorCode::Conflict);
    rollback_rejected(*work);
    work = take(factory->begin());
    const auto security = take(work->session_security().find(changed.id));
    const auto base = take(work->sessions().find(changed.id));
    if (kind == 0) {
      check(!security && !base, "duplicate login conflict left either session projection");
    } else {
      check(security && base && security->version == stored->version &&
                security->rotation == stored->rotation && !security->revoked &&
                base->version == stored->version && base->rotation == stored->rotation &&
                !base->revoked,
            "duplicate refresh/revoke conflict partially committed session state");
    }
    work->rollback();
  }
  auto surviving = take(factory->begin());
  factory.reset();
  check(take(surviving->credentials().current({"factory-commit-credential"})).has_value(),
        "security UoW lost connection when factory was destroyed");
  expect(open_identity_security(config), target::ErrorCode::Conflict);
  surviving->rollback();
  factory = take(open_identity_security(config));
  auto probe = take(factory->begin());
  take(probe->current_time());
  probe->rollback();
}

void identity(const DatabaseConfig& config) {
  auto factory = take(open_identity(config));
  concurrent_probe(*factory);
  auto work = take(factory->begin());
  target::LoginSession orphan{{"orphan"}, {"absent"}, {1}, 0, target::TimePoint::max(), false};
  expect(work->sessions().save(orphan, {}), target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  target::Principal principal{
      {"live-principal"}, target::PrincipalKind::Human, {}, {1}, target::LifecycleState::Active};
  take(work->principals().save(principal, {}));
  target::LoginSession session{{"live-session"},         principal.id, {1}, kMaximum,
                               target::TimePoint::max(), false};
  take(work->sessions().save(session, {}));
  target::ResourceGrant grant{{"live-grant"},
                              principal.id,
                              target::FixedRole::Operator,
                              {{{target::ResourceKind::Instance, {"same-id"}}, "plugin.manage"},
                               {{target::ResourceKind::Catalog, {"same-id"}}, "account.manage"}},
                              {1}};
  take(work->grants().save(grant, {}));
  take(work->authorizations().save({principal.id, {kHigh}, false}, {}));
  const auto now = take(work->current_time());
  target::CommandReceipt receipt{{principal.id,
                                  "grant.create",
                                  {target::ResourceKind::Catalog, {grant.id.value}},
                                  "live-grant-receipt"},
                                 {},
                                 grant,
                                 now,
                                 now + std::chrono::hours(168)};
  take(work->receipts().insert(receipt, {100}));
  target::HandoffContext dirty_context{target::Service::Identity,
                                       {"dirty-capacity"},
                                       {target::Service::Identity,
                                        {"dirty-operation"},
                                        {target::ResourceKind::Principal, {principal.id.value}}},
                                       {},
                                       {target::ResourceKind::Principal, {principal.id.value}},
                                       {1},
                                       principal.id,
                                       {1},
                                       {},
                                       now,
                                       now + std::chrono::hours(1)};
  take(work->journal().enqueue({dirty_context,
                                {dirty_context.resource, {1}},
                                target::OutboxLane::Ordinary,
                                target::OutboxState::Pending,
                                0,
                                now,
                                {},
                                {}},
                               {1, 1, 1}, now));
  commit(*work);
  work = take(factory->begin());
  const auto loaded = take(work->sessions().find(session.id));
  check(loaded && loaded->rotation == kMaximum && loaded->expires_at == target::TimePoint::max(),
        "typed session lost uint64/time endpoint");
  check(take(work->grants().find(grant.id))->scopes.size() == 2, "typed grant lost scopes");
  grant.version = {2};
  grant.role.reset();
  grant.scopes.erase(grant.scopes.begin());
  take(work->grants().save(grant, target::ResourceVersion{1}));
  take(work->authorizations().save({principal.id, {kHigh + 1}, true},
                                   target::ResourceVersion{kHigh}));
  commit(*work);
  work = take(factory->begin());
  const auto updated = take(work->grants().find(grant.id));
  check(updated && !updated->role && updated->scopes.size() == 1 &&
            updated->scopes[0].resource.kind == target::ResourceKind::Catalog,
        "grant CAS did not replace child scopes");
  const auto original = take(work->receipts().find(receipt.key));
  check(original && std::get<target::ResourceGrant>(original->result).scopes.size() == 2,
        "live update changed immutable receipt");
  check(take(work->authorizations().find(principal.id))->invalidation_dirty,
        "authorization dirty marker lost");
  work->rollback();
  // The dirty/version update above committed while the ordinary outbox was at capacity.
  work = take(factory->begin());
  const auto claimed =
      take(work->journal().claim(target::Service::Identity, 1, now, std::chrono::seconds(30)));
  check(claimed.size() == 1, "identity capacity fixture missing");
  commit(*work);
  work = take(factory->begin());
  take(work->journal().confirm({target::Service::Identity, dirty_context.message},
                               claimed[0].claim_token, now));
  commit(*work);
  factory.reset();
  factory = take(open_identity(config));
  work = take(factory->begin());
  check(take(work->principals().find(principal.id))->version.value == 1,
        "identity restart lost state");
  work->rollback();
  work = take(factory->begin());
  expect(work->authorizations().find({principal.id.value + '\0' + "suffix"}),
         target::ErrorCode::InvalidArgument);
  rollback_rejected(*work);
}
void runtime(const DatabaseConfig& config) {
  auto factory = take(open_runtime(config));
  concurrent_probe(*factory);
  auto work = take(factory->begin());
  target::BotAccount account{{"live-account"}, "platform", "00042",
                             "机器人🙂",       {1},        target::LifecycleState::Active};
  take(work->accounts().save(account, {}));
  target::PluginInstallation installation{
      {"live-installation"}, {"live-type"}, {1}, target::LifecycleState::Active};
  take(work->installations().save(installation, {}));
  target::PluginInstance instance{{"live-instance"},
                                  installation.id,
                                  installation.plugin_type,
                                  {kHigh},
                                  {kHigh},
                                  target::Fence{{"live-instance"},
                                                {"live-epoch"},
                                                {"live-execution"},
                                                {kMaximum},
                                                {"live-connection"},
                                                {kHigh}},
                                  {1},
                                  target::LifecycleState::Active,
                                  target::ResourceId{"live-host"}};
  take(work->instances().save(instance, {}));
  commit(*work);
  work = take(factory->begin());
  auto duplicate = account;
  duplicate.id = {"duplicate-account"};
  expect(work->accounts().save(duplicate, {}), target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  auto bad = instance;
  bad.id = {"bad-instance"};
  bad.current.reset();
  bad.plugin_type = {"wrong-type"};
  expect(work->instances().save(bad, {}), target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  const auto loaded = take(work->instances().find(instance.id));
  check(loaded && loaded->current->execution_generation.value == kMaximum &&
            loaded->current->connection_generation.value == kHigh &&
            loaded->host->value == "live-host",
        "typed instance fence lost precision");
  work->rollback();

  work = take(factory->begin());
  const auto nul_lookup = work->accounts().find({account.id.value + '\0' + "suffix"});
  check(std::holds_alternative<target::Failure>(nul_lookup), "NUL key aliased an existing account");
  expect(nul_lookup, target::ErrorCode::InvalidArgument);
  rollback_rejected(*work);
  work = take(factory->begin());
  auto nul_body = account;
  nul_body.id = {"nul-body"};
  nul_body.display_name = "prefix" + std::string(1, '\0') + "suffix";
  expect(work->accounts().save(nul_body, {}), target::ErrorCode::InvalidArgument);
  rollback_rejected(*work);

  // Two concurrent requests carry the same expected version; exactly one SQL CAS wins.
  std::latch start{1};
  std::atomic<int> successes = 0, conflicts = 0;
  auto competitor = [&] {
    start.wait();
    auto competing = take(factory->begin());
    auto changed = account;
    changed.version = {2};
    const auto saved = competing->accounts().save(changed, target::ResourceVersion{1});
    if (std::holds_alternative<std::monostate>(saved)) {
      commit(*competing);
      ++successes;
    } else {
      expect(saved, target::ErrorCode::Conflict);
      rollback_rejected(*competing);
      ++conflicts;
    }
  };
  std::thread first(competitor), second(competitor);
  start.count_down();
  first.join();
  second.join();
  check(successes == 1 && conflicts == 1, "concurrent account CAS did not choose one winner");
  work = take(factory->begin());
  check(take(work->accounts().find(account.id))->version.value == 2, "CAS winner not durable");
  instance.version = {2};
  instance.current.reset();
  instance.host.reset();
  instance.desired_version = {kMaximum};
  take(work->instances().save(instance, target::ResourceVersion{1}));
  commit(*work);
  work = take(factory->begin());
  check(!take(work->instances().find(instance.id))->current &&
            !take(work->instances().find(instance.id))->host,
        "nullable fence/host update lost");
  work->rollback();
  factory.reset();
  factory = take(open_runtime(config));
  work = take(factory->begin());
  check(take(work->accounts().find(account.id))->display_name == "机器人🙂",
        "runtime restart lost UTF8 state");
  work->rollback();

  // P05 adapter registry: stable adapter id plus assigned instance lookup, connection-only fence,
  // declared/effective capability text round-trip and bounded list ordering. The earlier factory is
  // still valid; opening a second writer session while this UoW holds its lease would conflict.
  work = take(factory->begin());
  target::AdapterRecord adapter{
      {"live-adapter"},
      {"nonebot",
       "1.0.0",
       "kiarabot.v1",
       {{"action.send_message.text.v1", "1"}, {"action.react_message.v1", "1"}},
       {{"action.send_message.text.v1", "1"}}},
      {{"live-adapter-instance"}, {"live-epoch"}, {"live-connection"}, {1}},
      {1},
      target::LifecycleState::Active,
      take(work->current_time()),
      take(work->current_time()) + std::chrono::seconds{1},
      take(work->current_time()) + std::chrono::seconds{91}};
  take(work->adapters().save(adapter, {}));
  target::AdapterBindingRecord binding{
      {"live-account"}, {"live-adapter-instance"}, {1}, target::LifecycleState::Active};
  take(work->adapter_bindings().save(binding, {}));
  target::InstanceConfiguration instance_config{{"live-instance"},
                                                {1},
                                                {0},
                                                target::ConfigApplyState::Pending,
                                                "delivery-1",
                                                std::nullopt,
                                                std::nullopt,
                                                take(work->current_time()),
                                                {std::byte{1}, std::byte{2}},
                                                {1}};
  take(work->instance_configs().save(instance_config, {}));
  commit(*work);
  work = take(factory->begin());
  const auto stored_adapter = take(work->adapters().find({"live-adapter"}));
  check(stored_adapter && stored_adapter->current.instance.value == "live-adapter-instance" &&
            stored_adapter->current.connection_generation.value == 1 &&
            stored_adapter->registration.declared_capabilities.size() == 2 &&
            stored_adapter->registration.effective_capabilities.size() == 1,
        "adapter relation lost connection fence or capability negotiation");
  const auto by_instance = take(work->adapters().by_instance({"live-adapter-instance"}));
  check(by_instance && by_instance->adapter.value == "live-adapter",
        "adapter instance lookup lost stable adapter id");
  check(!take(work->adapters().by_instance({"unknown-instance"})),
        "unknown adapter instance resolved to a record");
  const auto stored_binding = take(work->adapter_bindings().find({"live-account"}));
  check(stored_binding && stored_binding->adapter_instance.value == "live-adapter-instance",
        "adapter binding lost assigned instance");
  const auto stored_config = take(work->instance_configs().find({"live-instance"}));
  check(stored_config && stored_config->values.size() == 2 && stored_config->version.value == 1,
        "instance config lost values/version");
  const auto adapters = take(work->list_adapters(2, {}));
  check(adapters.size() == 1 && adapters.front().adapter.value == "live-adapter",
        "adapter list lost owner ordering");
  const auto installations = take(work->list_installations(2, {}));
  check(!installations.empty(), "installation list lost current rows");
  const auto configs_missing = work->instance_configs().find({"unknown-instance"});
  check(!take(configs_missing), "unknown instance config found");
  work->rollback();

  // One-time external-bridge bootstrap credentials: only the SHA-256 digest is stored, an active
  // credential blocks a second plaintext, and unbind-driven revocation flips the flag.
  work = take(factory->begin());
  check(!take(work->bridge_credentials().active("live-adapter-instance")),
        "unknown resource unexpectedly had an active bootstrap credential");
  const target::BridgeCredentialRecord credential{{"live-credential-id"},
                                                  "live-adapter-instance",
                                                  {std::byte{1}, std::byte{2}},
                                                  take(work->current_time()),
                                                  false};
  take(work->bridge_credentials().insert(credential));
  commit(*work);
  work = take(factory->begin());
  check(take(work->bridge_credentials().active("live-adapter-instance")),
        "active bootstrap credential not visible");
  const target::BridgeCredentialRecord credential_two{{"live-credential-id-2"},
                                                      "live-adapter-instance",
                                                      {std::byte{3}, std::byte{4}},
                                                      take(work->current_time()),
                                                      false};
  expect(work->bridge_credentials().insert(credential_two), target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  take(work->bridge_credentials().revoke("live-adapter-instance", take(work->current_time())));
  commit(*work);
  work = take(factory->begin());
  check(!take(work->bridge_credentials().active("live-adapter-instance")),
        "revoked bootstrap credential still active");
  work->rollback();
  work = take(factory->begin());
  // Conditional version still rejects a stale expected version for the new records.
  auto changed_adapter = *stored_adapter;
  changed_adapter.version = {2};
  changed_adapter.last_heartbeat_at += std::chrono::seconds{30};
  take(work->adapters().save(changed_adapter, target::ResourceVersion{1}));
  commit(*work);
  work = take(factory->begin());
  auto duplicate_adapter = adapter;
  duplicate_adapter.adapter = {"duplicate-adapter"};
  expect(work->adapters().save(duplicate_adapter, {}), target::ErrorCode::Conflict);
  rollback_rejected(*work);
  // P08b-3 Host command outbox and observation inbox.
  work = take(factory->begin());
  const auto now = take(work->current_time());
  target::HostCommandOutboxRecord command;
  command.command_id = {"live-host-command"};
  command.host_id = "live-host";
  command.instance_id = "live-instance";
  command.installation_id = "live-installation";
  command.kind = target::HostCommandKind::ApplyDesiredProcess;
  command.execution_generation = 7;
  command.desired_version = 3;
  command.desired_state = target::DesiredState::Running;
  command.package_sha256 = std::string(64, 'a');
  command.payload_sha256 = std::string(64, 'b');
  command.state = target::HostCommandDeliveryState::Pending;
  command.created_at = now;
  command.updated_at = now;
  const auto inserted = take(work->host_commands().insert(command));
  check(!inserted.duplicate, "new host command marked duplicate");
  const auto repeated = take(work->host_commands().insert(command));
  check(repeated.duplicate, "same host command payload was not idempotent");
  auto pending = take(work->host_commands().pending("live-host", 1, 10));
  check(pending.size() == 1 && pending[0].command_id == "live-host-command",
        "pending host command not listed");
  take(work->host_commands().mark_dispatched(command.command_id, 1, "live-epoch", now));
  take(work->host_commands().record_receipt(command.command_id, true,
                                            target::ExecutionOutcome::KnownSucceeded,
                                            std::string{"live-marker"}, now));
  check(take(work->host_commands().pending("live-host", 2, 10)).empty(),
        "receipted host command replayed");
  // P08d Delete participant snapshot: the same schema accepts a StopExecution row with the full
  // installation/package/desired-state snapshot; an UNKNOWN receipt stays Unknown, never stopped.
  target::HostCommandOutboxRecord stop = command;
  stop.command_id = {"live-host-stop"};
  stop.kind = target::HostCommandKind::StopExecution;
  stop.desired_state = target::DesiredState::Stopped;
  stop.payload_sha256 = std::string(64, 'c');
  stop.created_at = now;
  stop.updated_at = now;
  take(work->host_commands().insert(stop));
  take(work->host_commands().record_receipt(stop.command_id, true,
                                            target::ExecutionOutcome::Unknown, std::string{}, now));
  const auto stopped_receipt = take(work->host_commands().find(stop.command_id));
  check(stopped_receipt && stopped_receipt->kind == target::HostCommandKind::StopExecution &&
            stopped_receipt->installation_id == "live-installation" &&
            stopped_receipt->desired_state == target::DesiredState::Stopped &&
            stopped_receipt->receipt_persisted.value_or(false) &&
            stopped_receipt->receipt_outcome == target::ExecutionOutcome::Unknown,
        "Delete stop snapshot/UNKNOWN receipt projection incorrect");
  const auto receipted = take(work->host_commands().find(command.command_id));
  check(receipted && receipted->state == target::HostCommandDeliveryState::Receipted &&
            receipted->receipt_persisted.value_or(false) &&
            receipted->receipt_outcome == target::ExecutionOutcome::KnownSucceeded,
        "host receipt not persisted");
  target::HostObservationRecord observation;
  observation.host_id = "live-host";
  observation.connection_generation = 1;
  observation.sequence = 42;
  observation.instance_id = "live-instance";
  observation.execution_generation = 7;
  observation.outcome = target::ExecutionOutcome::KnownSucceeded;
  observation.process_marker = "live-marker";
  observation.command_id = command.command_id;
  observation.observed_at = now;
  check(!take(work->host_commands().record_observation(observation)),
        "first observation marked duplicate");
  check(take(work->host_commands().record_observation(observation)),
        "observation dedup key not enforced");
  auto later = observation;
  later.sequence = 44;
  later.command_id = "live-host-command-later";
  auto earlier = observation;
  earlier.sequence = 43;
  check(!take(work->host_commands().record_observation(later)), "later observation rejected");
  check(!take(work->host_commands().record_observation(earlier)), "earlier observation rejected");
  const auto observations = take(work->host_commands().observations("live-host", 1, 10));
  check(observations.size() == 3 && observations[0].sequence == 42 &&
            observations[1].sequence == 43 && observations[2].sequence == 44,
        "observation projection not durable/deduplicated/ordered");
  commit(*work);
  work = take(factory->begin());
  check(take(work->host_commands().observations("live-host", 1, 10)).size() == 3,
        "observation lost after restart");
  work->rollback();

  // Maintenance: dispatched-without-receipt becomes Unknown after the timeout; pruned only after
  // the unresolved retention window, never fabricated as a stopped process.
  work = take(factory->begin());
  auto timeout_command = command;
  timeout_command.command_id = "live-host-timeout";
  timeout_command.payload_sha256 = std::string(64, 'd');
  timeout_command.state = target::HostCommandDeliveryState::Pending;
  take(work->host_commands().insert(timeout_command));
  take(work->host_commands().mark_dispatched(timeout_command.command_id, 1, "live-epoch", now));
  check(take(work->host_commands().expire_dispatched(now + std::chrono::seconds{31},
                                                     std::chrono::seconds{30})) == 1,
        "dispatched timeout command not expired");
  const auto expired = take(work->host_commands().find(timeout_command.command_id));
  check(expired && expired->state == target::HostCommandDeliveryState::Unknown,
        "timed out command not Unknown");
  const auto pruned = take(work->host_commands().prune(now + std::chrono::hours{24 * 40},
                                                       std::chrono::hours{24 * 7},
                                                       std::chrono::hours{24 * 30}, 100));
  check(pruned == 3, "expired/receipted/Delete commands not pruned by retention");
  check(!take(work->host_commands().find(timeout_command.command_id)),
        "pruned command still present");
  commit(*work);

  // Same command_id with a different payload is a conflict, and that failure poisons its UoW.
  work = take(factory->begin());
  auto conflict_seed = command;
  conflict_seed.command_id = "live-host-conflict";
  conflict_seed.payload_sha256 = std::string(64, 'e');
  take(work->host_commands().insert(conflict_seed));
  commit(*work);
  work = take(factory->begin());
  auto conflicting = conflict_seed;
  conflicting.payload_sha256 = std::string(64, 'c');
  expect(work->host_commands().insert(conflicting), target::ErrorCode::Conflict);
  rollback_rejected(*work);
}
void scheduler(const DatabaseConfig& config) {
  auto factory = take(open_scheduler(config));
  concurrent_probe(*factory);
  auto work = take(factory->begin());
  const auto now = take(work->current_time());
  target::ScheduledTask task{
      {{"task-owner"}, "same-task"},
      {"task-installation"},
      {{"adapter"}, {"account"}, "0001", target::ConversationKey::Kind::Group},
      {std::byte{0}, std::byte{0xff}},
      now + std::chrono::nanoseconds(137),
      target::TaskState::Pending,
      {1},
      {"live-occurrence"}};
  take(work->tasks().save(task, {}));
  auto other = task;
  other.key.owner = {"other-owner"};
  other.occurrence = {"other-occurrence"};
  take(work->tasks().save(other, {}));
  target::HandoffContext handoff{target::Service::Scheduler,
                                 task.occurrence,
                                 {target::Service::Scheduler,
                                  {"task-operation"},
                                  {target::ResourceKind::Task, {"task-resource"}}},
                                 {},
                                 {target::ResourceKind::Instance, {task.key.owner.value}},
                                 {kMaximum},
                                 {"actor"},
                                 {kHigh},
                                 {},
                                 now,
                                 now + std::chrono::hours(1)};
  target::Occurrence occurrence{task.key.owner,
                                task.installation,
                                task.key.task_id,
                                {1},
                                task.occurrence,
                                task.run_at,
                                handoff,
                                {1},
                                target::OperationState::Accepted,
                                {},
                                {}};
  take(work->occurrences().save(occurrence, {}));
  commit(*work);
  work = take(factory->begin());
  const auto loaded = take(work->tasks().find(task.key));
  check(loaded && loaded->payload == task.payload && loaded->run_at == task.run_at &&
            loaded->target.conversation_id == "0001",
        "typed task payload/time/target lost");
  check(take(work->tasks().find(other.key)).has_value(), "task owner key collided");
  work->rollback();
  work = take(factory->begin());
  auto changed_occurrence = task;
  changed_occurrence.version = {2};
  changed_occurrence.occurrence = {"changed-occurrence"};
  expect(work->tasks().save(changed_occurrence, target::ResourceVersion{1}),
         target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  occurrence.version = {2};
  occurrence.state = target::OperationState::Succeeded;
  occurrence.accepted_event_id = "accepted-event";
  occurrence.acknowledged_at = now + std::chrono::nanoseconds(731);
  take(work->occurrences().save(occurrence, target::ResourceVersion{1}));
  commit(*work);
  work = take(factory->begin());
  const auto accepted = take(work->occurrences().find(occurrence.occurrence_id));
  check(accepted && accepted->accepted_event_id == occurrence.accepted_event_id &&
            accepted->acknowledged_at == occurrence.acknowledged_at &&
            accepted->handoff.operation == handoff.operation,
        "occurrence acceptance evidence lost");
  work->rollback();
  factory.reset();
  factory = take(open_scheduler(config));
  work = take(factory->begin());
  check(take(work->occurrences().find(occurrence.occurrence_id))->version.value == 2,
        "scheduler restart lost acceptance");
  work->rollback();
}
void model(const DatabaseConfig& config) {
  auto factory = take(open_model(config));
  concurrent_probe(*factory);
  auto work = take(factory->begin());
  const auto now = take(work->current_time());
  target::ModelIntent intent{{"model-owner"},
                             "same-call",
                             {"model"},
                             {kMaximum},
                             {},
                             target::ModelCallState::Started,
                             {1},
                             now,
                             now + std::chrono::nanoseconds(137)};
  intent.request_hash.bytes[31] = std::byte{0xff};
  take(work->model_intents().save(intent, {}));
  auto other = intent;
  other.owner = {"other-owner"};
  take(work->model_intents().save(other, {}));
  commit(*work);
  work = take(factory->begin());
  const target::ModelCallKey key{intent.owner, intent.logical_call_id};
  const auto loaded = take(work->model_intents().find(key));
  check(loaded && loaded->config_version.value == kMaximum &&
            loaded->request_hash == intent.request_hash &&
            loaded->observed_at == intent.observed_at,
        "typed model intent lost hash/version/time");
  intent.version = {2};
  intent.state = target::ModelCallState::Unknown;
  take(work->model_intents().save(intent, target::ResourceVersion{1}));
  commit(*work);
  work = take(factory->begin());
  expect(work->model_intents().save(intent, {}), target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  factory.reset(); // Active UoW owns the session lease independently of factory lifetime.
  intent.version = {3};
  take(work->model_intents().save(intent, target::ResourceVersion{2}));
  commit(*work);
  check(work->owner() == target::Service::Model, "terminal UoW lost owner identity");
  factory = take(open_model(config));
  work = take(factory->begin());
  check(take(work->model_intents().find(key))->state == target::ModelCallState::Unknown,
        "model restart lost Unknown");
  work->rollback();

  // P11 dedicated Model repositories: configuration CAS/alias, sealed secret version pinning and
  // the durable call/UNKNOWN projection on the same physical writer transaction.
  target::ModelConfiguration model_config;
  model_config.model_id = {"11111111-1111-4111-8111-111111111111"};
  model_config.alias = "default";
  model_config.provider_kind = target::ModelProviderKind::OpenAiChatCompletions;
  model_config.endpoint = "https://api.example.com/v1/chat/completions";
  model_config.provider_model = "gpt-test";
  model_config.timeout_ms = 5000;
  model_config.max_concurrency = 2;
  model_config.state = target::LifecycleState::Disabled;
  model_config.version = {1};
  work = take(factory->begin());
  const auto config_now = take(work->current_time());
  model_config.created_at = config_now;
  model_config.observed_at = config_now;
  take(work->model_configs().save(model_config, {}));
  commit(*work);
  work = take(factory->begin());
  expect(work->model_configs().save(model_config, {}), target::ErrorCode::Conflict);
  rollback_rejected(*work);

  work = take(factory->begin());
  const auto stored_config = take(work->model_configs().find(model_config.model_id));
  check(stored_config && stored_config->alias == "default" &&
            stored_config->provider_kind == target::ModelProviderKind::OpenAiChatCompletions &&
            stored_config->timeout_ms == 5000 && stored_config->version.value == 1,
        "P11 model configuration round trip failed");
  const auto by_alias = take(work->model_configs().find_by_alias("default"));
  check(by_alias && by_alias->model_id.value == model_config.model_id.value,
        "P11 model alias lookup failed");
  const auto listed = take(work->model_configs().list(10, ""));
  check(listed.size() == 1 && listed.front().model_id.value == model_config.model_id.value,
        "P11 model configuration page failed");
  auto updated_config = *stored_config;
  updated_config.timeout_ms = 6000;
  updated_config.version = {2};
  take(work->model_configs().save(updated_config, target::ResourceVersion{1}));
  commit(*work);
  work = take(factory->begin());
  expect(work->model_configs().save(updated_config, target::ResourceVersion{1}),
         target::ErrorCode::Conflict);
  rollback_rejected(*work);

  target::ProviderSecretRecord provider_secret;
  provider_secret.model_id = model_config.model_id;
  provider_secret.secret_version = {1};
  provider_secret.root_key_version = 7;
  provider_secret.aead = 1;
  provider_secret.nonce.fill(std::byte{0x11});
  provider_secret.sealed_secret.assign(20, std::byte{0x22});
  work = take(factory->begin());
  const auto secret_now = take(work->current_time());
  provider_secret.created_at = secret_now;
  take(work->provider_secrets().insert(provider_secret));
  commit(*work);
  work = take(factory->begin());
  const auto current_secret = take(work->provider_secrets().current(model_config.model_id));
  check(current_secret && current_secret->secret_version.value == 1 &&
            current_secret->root_key_version == 7 &&
            current_secret->sealed_secret == provider_secret.sealed_secret,
        "P11 provider secret round trip failed");
  const auto pinned =
      take(work->provider_secrets().find(model_config.model_id, target::ResourceVersion{1}));
  check(pinned && pinned->sealed_secret.size() == 20, "P11 pinned provider secret lost");
  work->rollback();

  target::ModelIntent call;
  call.owner = {"p11-call-owner"};
  call.logical_call_id = "p11-logical-call";
  call.model = model_config.model_id;
  call.config_version = {2};
  call.request_hash.bytes[0] = std::byte{0x9a};
  call.state = target::ModelCallState::Started;
  call.version = {1};
  work = take(factory->begin());
  const auto call_now = take(work->current_time());
  call.created_at = call_now;
  call.observed_at = call_now;
  call.secret_version = {1};
  call.usage_known = true;
  call.may_have_billed = false;
  call.provider_request_id = "req-1";
  call.latency_ms = 12;
  take(work->model_calls().insert(call));
  commit(*work);
  work = take(factory->begin());
  expect(work->model_calls().insert(call), target::ErrorCode::Conflict);
  rollback_rejected(*work);

  work = take(factory->begin());
  const auto loaded_call = take(work->model_calls().find({call.owner, call.logical_call_id}));
  check(loaded_call && loaded_call->secret_version.value == 1 && loaded_call->usage_known &&
            !loaded_call->may_have_billed && loaded_call->provider_request_id == "req-1" &&
            loaded_call->latency_ms == 12,
        "P11 durable call facts lost");
  // v1.2 regression: a definite provider rejection maps to the domain ErrorCode::InvalidArgument
  // (0) and must be durably storable as a FAILED terminal fact on the dedicated call repository.
  // The shared model_intents snapshot CHECK previously rejected 0, so txn2 failed and the call
  // stayed Started/UNKNOWN instead. A dedicated call row keeps the in-flight retention case intact.
  auto invalid_argument_call = *loaded_call;
  invalid_argument_call.logical_call_id = "p11-invalid-argument-call";
  invalid_argument_call.version = {1};
  invalid_argument_call.state = target::ModelCallState::Started;
  invalid_argument_call.error.reset();
  take(work->model_calls().insert(invalid_argument_call));
  commit(*work);
  work = take(factory->begin());
  auto rejected_fact = take(
      work->model_calls().find({invalid_argument_call.owner, invalid_argument_call.logical_call_id}));
  rejected_fact->state = target::ModelCallState::Failed;
  rejected_fact->error = target::ErrorCode::InvalidArgument;
  rejected_fact->usage_known = true;
  rejected_fact->may_have_billed = false;
  ++rejected_fact->version.value;
  take(work->model_calls().save(*rejected_fact, invalid_argument_call.version));
  commit(*work);
  work = take(factory->begin());
  const auto stored_rejection = take(
      work->model_calls().find({invalid_argument_call.owner, invalid_argument_call.logical_call_id}));
  check(stored_rejection && stored_rejection->state == target::ModelCallState::Failed &&
            stored_rejection->error == target::ErrorCode::InvalidArgument,
        "definite InvalidArgument rejection was not durably stored");
  work->rollback();

  work = take(factory->begin());
  // Query cutoff uses the caller DB sample exactly, not wall-clock time in the adapter.
  auto expired_started = *loaded_call;
  expired_started.logical_call_id = "p11-started-35s";
  expired_started.created_at = call_now - std::chrono::seconds{35};
  expired_started.observed_at = expired_started.created_at;
  expired_started.may_have_billed = true;
  take(work->model_calls().insert(expired_started));
  auto recent_started = expired_started;
  recent_started.logical_call_id = "p11-started-34s";
  recent_started.created_at = call_now - std::chrono::seconds{34};
  recent_started.observed_at = recent_started.created_at;
  take(work->model_calls().insert(recent_started));
  auto unknown_29 = *loaded_call;
  unknown_29.logical_call_id = "p11-unknown-29d";
  unknown_29.state = target::ModelCallState::Unknown;
  unknown_29.created_at = call_now - std::chrono::hours{24 * 29};
  unknown_29.observed_at = unknown_29.created_at;
  unknown_29.may_have_billed = true;
  take(work->model_calls().insert(unknown_29));
  auto unknown_30 = unknown_29;
  unknown_30.logical_call_id = "p11-unknown-30d";
  unknown_30.created_at = call_now - target::kModelUnknownResolutionWindow;
  unknown_30.observed_at = unknown_30.created_at;
  take(work->model_calls().insert(unknown_30));
  auto unknown_call = *loaded_call;
  unknown_call.logical_call_id = "p11-unknown-call";
  unknown_call.state = target::ModelCallState::Unknown;
  unknown_call.may_have_billed = true;
  unknown_call.created_at = call_now - std::chrono::hours{24 * 31};
  unknown_call.observed_at = unknown_call.created_at;
  take(work->model_calls().insert(unknown_call));
  commit(*work);

  work = take(factory->begin());
  const auto unresolved = take(work->model_calls().list_unresolved_unknown(call_now, 10));
  check(std::any_of(unresolved.begin(), unresolved.end(),
                    [](const target::ModelIntent& value) {
                      return value.logical_call_id == "p11-unknown-call";
                    }),
        "P11 stale UNKNOWN projection failed");
  const auto contains_call = [](const auto& values, std::string_view id) {
    return std::any_of(values.begin(), values.end(), [&](const target::ModelIntent& value) {
      return value.logical_call_id == id;
    });
  };
  check(!contains_call(unresolved, "p11-unknown-29d") &&
            contains_call(unresolved, "p11-unknown-30d"),
        "P11 real UNKNOWN query violated the 29/30-day boundary");
  const auto expired =
      take(work->model_calls().list_expired_started(call_now - std::chrono::seconds{35}, 10));
  check(contains_call(expired, "p11-started-35s") && !contains_call(expired, "p11-started-34s") &&
            !contains_call(expired, "p11-unknown-30d"),
        "P11 expired STARTED query violated 35-second/state boundary");
  auto recovered = expired_started;
  recovered.state = target::ModelCallState::Unknown;
  recovered.may_have_billed = true;
  recovered.usage_known = false;
  recovered.observed_at = call_now;
  recovered.error = target::ErrorCode::Timeout;
  ++recovered.version.value;
  take(work->model_calls().save(recovered, expired_started.version));
  check(target::model_unknown_resolution_due(unknown_30, call_now) &&
            !target::model_unknown_resolution_due(unknown_29, call_now),
        "P11 Core UNKNOWN window disagrees with real query");
  take(work->model_calls().save(
      target::model_abandon_unknown(unknown_30, call_now, "p11-maintenance"), unknown_30.version));
  const auto stale_call =
      take(work->model_calls().find({unknown_call.owner, unknown_call.logical_call_id}));
  const auto abandoned = target::model_abandon_unknown(*stale_call, call_now, "p11-maintenance");
  take(work->model_calls().save(abandoned, stale_call->version));
  commit(*work);

  // A late Provider still owns version1; recovery's version2 wins, including under a new UoW.
  work = take(factory->begin());
  auto late_success = expired_started;
  late_success.state = target::ModelCallState::Succeeded;
  ++late_success.version.value;
  expect(work->model_calls().save(late_success, expired_started.version),
         target::ErrorCode::Conflict);
  rollback_rejected(*work);
  work = take(factory->begin());
  const auto recovered_call =
      take(work->model_calls().find({expired_started.owner, expired_started.logical_call_id}));
  check(recovered_call && recovered_call->state == target::ModelCallState::Unknown &&
            recovered_call->version.value == 2 && recovered_call->may_have_billed,
        "P11 stale Provider CAS overwrote STARTED recovery");
  const auto kept_29 =
      take(work->model_calls().find({unknown_29.owner, unknown_29.logical_call_id}));
  const auto disposed_30 =
      take(work->model_calls().find({unknown_30.owner, unknown_30.logical_call_id}));
  check(kept_29 && kept_29->state == target::ModelCallState::Unknown &&
            kept_29->disposition == target::ModelCallDisposition::Unspecified && disposed_30 &&
            disposed_30->state == target::ModelCallState::Failed &&
            disposed_30->disposition == target::ModelCallDisposition::Abandoned,
        "P11 real 29-day retention/30-day disposition did not persist");
  work->rollback();

  work = take(factory->begin());
  const auto resolved_call =
      take(work->model_calls().find({unknown_call.owner, unknown_call.logical_call_id}));
  check(resolved_call && resolved_call->state == target::ModelCallState::Failed &&
            resolved_call->disposition == target::ModelCallDisposition::Abandoned &&
            resolved_call->resolved_by == "p11-maintenance",
        "P11 UNKNOWN resolution did not persist");
  const auto pruned = take(work->model_calls().prune_terminal(
      call_now + target::kModelCallRetention + std::chrono::hours{1}, 10));
  check(pruned >= 1, "P11 terminal call retention sweep did nothing");
  commit(*work);

  work = take(factory->begin());
  check(take(work->model_calls().find({call.owner, call.logical_call_id}))->state ==
            target::ModelCallState::Started,
        "P11 retention sweep deleted an in-flight call");
  work->rollback();
  // A control-side lookup by call id alone must reject cross-owner ambiguity instead of returning
  // an arbitrary instance's call.
  work = take(factory->begin());
  auto ambiguous = call;
  ambiguous.owner = {"p11-other-owner"};
  take(work->model_calls().insert(ambiguous));
  commit(*work);
  work = take(factory->begin());
  expect(work->model_calls().find_by_call_id(call.logical_call_id), target::ErrorCode::Conflict);
  rollback_rejected(*work);
}
} // namespace

void check_typed(const DatabaseConfig& config) {
  auto runtime_config = config;
  runtime_config.user = config.runtime_role;
  auto mismatch = runtime_config;
  mismatch.service = target::Service::Control;
  expect(open_identity(mismatch), target::ErrorCode::InvalidArgument);
  expect(open_identity_security(mismatch), target::ErrorCode::InvalidArgument);
  expect(open_runtime(mismatch), target::ErrorCode::InvalidArgument);
  expect(open_scheduler(mismatch), target::ErrorCode::InvalidArgument);
  expect(open_model(mismatch), target::ErrorCode::InvalidArgument);
  switch (config.service) {
  case target::Service::Identity:
    security(runtime_config);
    identity(runtime_config);
    break;
  case target::Service::Runtime:
    runtime(runtime_config);
    break;
  case target::Service::Scheduler:
    scheduler(runtime_config);
    break;
  case target::Service::Model:
    model(runtime_config);
    break;
  default:
    check(false, "invalid fixture owner");
  }
}
} // namespace kiarabot::infrastructure::postgres::test
