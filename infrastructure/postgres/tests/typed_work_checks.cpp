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
  expect(open_runtime(mismatch), target::ErrorCode::InvalidArgument);
  expect(open_scheduler(mismatch), target::ErrorCode::InvalidArgument);
  expect(open_model(mismatch), target::ErrorCode::InvalidArgument);
  switch (config.service) {
  case target::Service::Identity:
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
