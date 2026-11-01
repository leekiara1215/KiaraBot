#include "typed_work_checks.hpp"

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
         target::PendingCredentialMaterial{target::CredentialKind::Password},
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
                                          false};
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
