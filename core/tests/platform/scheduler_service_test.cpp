#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/application/target/scheduler_service.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("scheduler assertion failed at line " +
                             std::to_string(location.line()));
  }
}
t::TimePoint at(std::int64_t seconds) {
  return t::TimePoint{std::chrono::seconds{seconds}};
}
std::string uuid(std::uint8_t first) {
  std::string result(36, '0');
  for (const int index : {8, 13, 18, 23}) {
    result[static_cast<std::size_t>(index)] = '-';
  }
  result[0] = static_cast<char>('a' + (first % 6));
  result[14] = '4';
  result[19] = '8';
  return result;
}
t::Failure failure(t::ErrorCode code) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, std::nullopt, std::nullopt};
}

class Store {
public:
  std::map<std::string, t::ScheduledTask> tasks;
  std::map<std::string, t::Occurrence> occurrences;
  std::map<std::string, t::Operation> operations;
  std::vector<t::CommandReceipt> receipts;
  std::vector<t::OutboxEntry> outbox;
  t::TimePoint now = at(1000);
};
std::string task_key(const t::TaskKey& key) {
  return key.owner.value + "#" + key.task_id;
}
std::string occurrence_key(const t::MessageId& id) {
  return id.value;
}

template <typename Value> class MapRepository final : public t::Repository<Value, Value> {
public:
  using Key = Value;
  // Not used directly; concrete repositories below adapt typed keys.
};

template <typename Key, typename Value>
class TypedRepository final : public t::Repository<Key, Value> {
public:
  TypedRepository(std::map<std::string, Value>& map, std::string (*key)(const Key&),
                  Key (*from_value)(const Value&))
      : map_(map), key_(key), from_value_(from_value) {}
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    const auto found = map_.find(key_(key));
    if (found == map_.end()) {
      return std::optional<Value>{};
    }
    return std::optional<Value>{found->second};
  }
  t::Status save(const Value& value, std::optional<t::ResourceVersion> expected) override {
    const Key key = from_value_(value);
    const auto found = map_.find(key_(key));
    if (expected) {
      if (found == map_.end() || found->second.version != *expected) {
        return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                          t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
      }
    } else if (found != map_.end()) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    map_[key_(key)] = value;
    return std::monostate{};
  }

private:
  std::map<std::string, Value>& map_;
  std::string (*key_)(const Key&);
  Key (*from_value_)(const Value&);
};

std::string task_key_text(const t::TaskKey& key) {
  return task_key(key);
}
t::TaskKey task_from_value(const t::ScheduledTask& task) {
  return task.key;
}
std::string occurrence_key_text(const t::MessageId& id) {
  return id.value;
}
t::MessageId occurrence_from_value(const t::Occurrence& occurrence) {
  return occurrence.occurrence_id;
}

class FakeUow final : public t::SchedulerUnitOfWork {
public:
  FakeUow(const Store& store, std::shared_ptr<Store> live)
      : store_(store), live_(std::move(live)), tasks_(store_.tasks, task_key_text, task_from_value),
        occurrences_(store_.occurrences, occurrence_key_text, occurrence_from_value) {}

  t::Repository<t::TaskKey, t::ScheduledTask>& tasks() override {
    return tasks_;
  }
  t::Repository<t::MessageId, t::Occurrence>& occurrences() override {
    return occurrences_;
  }
  t::Outcome<std::vector<t::ScheduledTask>>
  list_tasks(const t::InstanceId& owner, std::size_t limit, const std::string& after) override {
    std::vector<t::ScheduledTask> result;
    for (const auto& [key, task] : store_.tasks) {
      if (task.key.owner != owner || (!after.empty() && task.key.task_id <= after)) {
        continue;
      }
      result.push_back(task);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
      return left.key.task_id < right.key.task_id;
    });
    if (result.size() > limit) {
      result.resize(limit);
    }
    return result;
  }
  t::Outcome<std::pair<std::size_t, std::size_t>>
  count_tasks(const t::InstanceId& owner, const t::InstallationId& installation) override {
    std::size_t instance = 0;
    std::size_t install = 0;
    for (const auto& [key, task] : store_.tasks) {
      if (t::task_terminal(task.state)) {
        continue;
      }
      if (task.key.owner == owner) {
        ++instance;
      }
      if (task.installation == installation) {
        ++install;
      }
    }
    return std::pair<std::size_t, std::size_t>{instance, install};
  }
  t::Outcome<std::vector<t::ScheduledTask>> due_tasks(t::TimePoint run_at,
                                                      std::size_t limit) override {
    std::vector<t::ScheduledTask> result;
    for (const auto& [key, task] : store_.tasks) {
      if (task.state == t::TaskState::Pending && task.run_at <= run_at) {
        result.push_back(task);
      }
    }
    std::sort(result.begin(), result.end(),
              [](const auto& left, const auto& right) { return left.run_at < right.run_at; });
    if (result.size() > limit) {
      result.resize(limit);
    }
    return result;
  }
  t::Outcome<std::vector<t::Occurrence>> inflight_occurrences(std::size_t limit) override {
    std::vector<t::Occurrence> result;
    for (const auto& [key, occurrence] : store_.occurrences) {
      if (occurrence.state == t::OperationState::Accepted) {
        result.push_back(occurrence);
      }
    }
    if (result.size() > limit) {
      result.resize(limit);
    }
    return result;
  }
  t::Outcome<t::TimePoint> current_time() override {
    return store_.now;
  }
  t::Service owner() const noexcept override {
    return t::Service::Scheduler;
  }
  t::ReceiptRepository& receipts() override {
    return receipts_;
  }
  t::JournalRepository& journal() override {
    return journal_;
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    return operations_;
  }
  t::CommitResult commit() override {
    *live_ = store_;
    return t::CommitResult::Committed;
  }
  void rollback() noexcept override {}

private:
  class Receipts final : public t::ReceiptRepository {
  public:
    explicit Receipts(Store& store) : store_(store) {}
    t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey& key) override {
      for (const auto& receipt : store_.receipts) {
        if (receipt.key == key) {
          return std::optional<t::CommandReceipt>{receipt};
        }
      }
      return std::optional<t::CommandReceipt>{};
    }
    t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt& receipt,
                                       t::ReceiptCapacity) override {
      store_.receipts.push_back(receipt);
      return t::ReceiptWrite{receipt, false};
    }
    t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
      return std::size_t{0};
    }

  private:
    Store& store_;
  };
  class Journal final : public t::JournalRepository {
  public:
    explicit Journal(Store& store) : store_(store) {}
    t::Status append_audit(const t::AuditEntry&) override {
      return std::monostate{};
    }
    t::Status enqueue(const t::OutboxEntry& entry, t::JournalCapacity, t::TimePoint) override {
      store_.outbox.push_back(entry);
      return std::monostate{};
    }
    t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service source, std::size_t limit,
                                                  t::TimePoint now, std::chrono::seconds) override {
      std::vector<t::OutboxClaim> claims;
      for (auto& entry : store_.outbox) {
        if (entry.context.source == source && entry.state != t::OutboxState::Confirmed &&
            entry.attempts < t::kMaxTriggerAttempts) {
          entry.state = t::OutboxState::Claimed;
          entry.attempts += 1;
          t::ClaimToken token;
          token.value = "claim-token-0123456789abcdef";
          entry.claim_token = token;
          claims.push_back(t::OutboxClaim{entry, token, now + 30s});
          if (claims.size() >= limit) {
            break;
          }
        }
      }
      return claims;
    }
    t::Status confirm(const t::HandoffKey& key, const t::ClaimToken& token, t::TimePoint) override {
      for (auto& entry : store_.outbox) {
        if (entry.context.source == key.source && entry.context.message == key.message) {
          if (entry.state == t::OutboxState::Confirmed && entry.claim_token &&
              entry.claim_token->value == token.value) {
            return std::monostate{};
          }
          if (!entry.claim_token || entry.claim_token->value != token.value) {
            return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                              t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
          }
          entry.state = t::OutboxState::Confirmed;
          return std::monostate{};
        }
      }
      return t::Failure{t::ErrorCode::NotFound, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
      return std::optional<t::InboxRecord>{};
    }
    t::Outcome<t::InboxWrite> record_received(const t::InboxRecord& receipt, t::JournalCapacity,
                                              t::TimePoint) override {
      return t::InboxWrite{receipt, false};
    }
    t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
      return std::size_t{0};
    }

  private:
    Store& store_;
  };
  class Operations final : public t::Repository<t::OperationId, t::Operation> {
  public:
    explicit Operations(Store& store) : store_(store) {}
    t::Outcome<std::optional<t::Operation>> find(const t::OperationId& id) override {
      const auto found = store_.operations.find(id.value);
      if (found == store_.operations.end()) {
        return std::optional<t::Operation>{};
      }
      return std::optional<t::Operation>{found->second};
    }
    t::Status save(const t::Operation& operation,
                   std::optional<t::ResourceVersion> expected) override {
      const auto found = store_.operations.find(operation.ref.id.value);
      if (expected) {
        if (found == store_.operations.end() || found->second.version != *expected) {
          return failure(t::ErrorCode::Conflict);
        }
      } else if (found != store_.operations.end()) {
        return failure(t::ErrorCode::Conflict);
      }
      store_.operations[operation.ref.id.value] = operation;
      return std::monostate{};
    }

  private:
    Store& store_;
  };

  Store store_;
  std::shared_ptr<Store> live_;
  TypedRepository<t::TaskKey, t::ScheduledTask> tasks_;
  TypedRepository<t::MessageId, t::Occurrence> occurrences_;
  Receipts receipts_{store_};
  Journal journal_{store_};
  Operations operations_{store_};
};

class Factory final : public t::SchedulerUnitOfWorkFactory {
public:
  explicit Factory(std::shared_ptr<Store> store) : store_(std::move(store)) {}
  t::Outcome<std::unique_ptr<t::SchedulerUnitOfWork>> begin() override {
    return std::unique_ptr<t::SchedulerUnitOfWork>(std::make_unique<FakeUow>(*store_, store_));
  }

private:
  std::shared_ptr<Store> store_;
};

class FakeZones final : public t::SchedulerTimeZonePort {
public:
  bool zone_data = true;
  std::optional<t::TimePoint> resolved;
  bool available() const override {
    return zone_data;
  }
  t::Outcome<t::TimePoint> resolve(const t::LocalCivilTime&) override {
    if (!zone_data) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (!resolved) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    return *resolved;
  }
};

class FakeIds final : public t::SchedulerIdPort {
public:
  t::Outcome<t::MessageId> occurrence_id() override {
    return t::MessageId{uuid(static_cast<std::uint8_t>(++counter))};
  }
  t::Outcome<t::OperationId> operation_id() override {
    return t::OperationId{uuid(static_cast<std::uint8_t>(++counter))};
  }
  int counter = 0;
};

class FakeDelivery final : public t::TimerDeliveryPort {
public:
  bool ok = true;
  bool available() const override {
    return ok;
  }
  t::Outcome<t::TimerDeliveryReceipt> accept(const t::TimerDeliveryRequest& request) override {
    accepted += 1;
    last_request = request;
    if (scripted) {
      return *scripted;
    }
    return t::TimerDeliveryReceipt{request.occurrence_id, duplicate, at(1300)};
  }
  int accepted = 0;
  bool duplicate = false;
  std::optional<t::Outcome<t::TimerDeliveryReceipt>> scripted;
  t::TimerDeliveryRequest last_request;
};

struct Harness {
  std::shared_ptr<Store> store = std::make_shared<Store>();
  Factory factory{store};
  FakeZones zones;
  FakeIds ids;
  FakeDelivery delivery;
  t::SchedulerService service{t::SchedulerServiceDependencies{
      factory, ids, zones, t::kTaskHorizon, t::kInstanceTaskCapacity, t::kInstallationTaskCapacity,
      &delivery}};

  t::SchedulerCommandContext context() const {
    return t::SchedulerCommandContext{
        t::InstanceId{uuid(1)}, t::InstallationId{uuid(2)}, uuid(3), uuid(4), at(2000),
        std::string(16, 'k')};
  }
  t::TaskCreationInput input(std::string task_id, t::TimePoint run_at) {
    t::TaskCreationInput value;
    value.task_id = std::move(task_id);
    value.target = t::ConversationKey{t::ResourceId{uuid(5)}, t::ResourceId{uuid(6)},
                                      "conversation", t::ConversationKey::Kind::Private};
    value.payload = {std::byte{1}, std::byte{2}};
    value.run_at = run_at;
    return value;
  }
};

void test_create_replay_capacity_and_invalid_time() {
  Harness harness;
  auto created = harness.service.create_task(harness.context(), harness.input("task-1", at(1500)));
  require(std::holds_alternative<t::ScheduledTask>(created));
  const auto task = std::get<t::ScheduledTask>(created);
  require(task.state == t::TaskState::Pending && task.version.value == 1);
  require(task.run_at == at(1500));
  auto replay = harness.service.create_task(harness.context(), harness.input("task-1", at(1500)));
  require(std::holds_alternative<t::ScheduledTask>(replay));
  require(harness.store->tasks.size() == 1);
  auto past = harness.service.create_task(harness.context(), harness.input("task-2", at(100)));
  require(std::holds_alternative<t::Failure>(past));
  require(std::get<t::Failure>(past).code == t::ErrorCode::InvalidArgument);
  auto beyond = harness.service.create_task(harness.context(),
                                            harness.input("task-3", at(1000 + 400LL * 24 * 3600)));
  require(std::holds_alternative<t::Failure>(beyond));
  // Capacity: preload an owner to the instance limit.
  const auto context = harness.context();
  for (std::size_t index = 0; index < t::kInstanceTaskCapacity; ++index) {
    t::ScheduledTask filler;
    filler.key = t::TaskKey{context.owner, "filler-" + std::to_string(index)};
    filler.installation = context.installation;
    filler.state = t::TaskState::Pending;
    filler.version = t::ResourceVersion{1};
    harness.store->tasks[task_key(filler.key)] = filler;
  }
  auto limited = harness.service.create_task(context, harness.input("task-capacity", at(1500)));
  require(std::holds_alternative<t::Failure>(limited));
  require(std::get<t::Failure>(limited).code == t::ErrorCode::RateLimited);
}

void test_local_time_gap_fold_and_missing_zone_data() {
  Harness harness;
  auto local = harness.input("task-local", at(0));
  local.run_at.reset();
  local.local_time = t::LocalCivilTime{2026, 3, 8, 2, 30, 0, "America/New_York"};
  auto rejected = harness.service.create_task(harness.context(), local);
  require(std::holds_alternative<t::Failure>(rejected));
  require(std::get<t::Failure>(rejected).code == t::ErrorCode::InvalidArgument);
  harness.zones.resolved = at(1600);
  auto accepted = harness.service.create_task(harness.context(), local);
  require(std::holds_alternative<t::ScheduledTask>(accepted));
  harness.zones.zone_data = false;
  auto unavailable =
      harness.service.create_task(harness.context(), harness.input("task-missing-zone", at(1700)));
  require(std::holds_alternative<t::ScheduledTask>(unavailable));
  auto missing_local = harness.input("task-missing-zone-2", at(0));
  missing_local.run_at.reset();
  missing_local.local_time = t::LocalCivilTime{2026, 1, 1, 0, 0, 0, "UTC"};
  auto zones_missing = harness.service.create_task(harness.context(), missing_local);
  require(std::holds_alternative<t::Failure>(zones_missing));
  require(std::get<t::Failure>(zones_missing).code == t::ErrorCode::UpstreamUnavailable);
}

void test_cancel_window_and_handoff_lifecycle() {
  Harness harness;
  const auto context = harness.context();
  require(std::holds_alternative<t::ScheduledTask>(
      harness.service.create_task(context, harness.input("task-cancel", at(1500)))));
  auto cancelled = harness.service.cancel_task(context, "task-cancel");
  require(std::holds_alternative<t::ScheduledTask>(cancelled));
  require(std::get<t::ScheduledTask>(cancelled).state == t::TaskState::Cancelled);
  auto replay = harness.service.cancel_task(context, "task-cancel");
  require(std::holds_alternative<t::ScheduledTask>(replay));
  // A due task cannot be cancelled once the trigger intent exists.
  require(std::holds_alternative<t::ScheduledTask>(
      harness.service.create_task(context, harness.input("task-due", at(1200)))));
  auto activated = harness.service.due_sweep(at(1300), 10);
  require(std::holds_alternative<std::size_t>(activated));
  require(std::get<std::size_t>(activated) == 1);
  auto late_cancel = harness.service.cancel_task(context, "task-due");
  require(std::holds_alternative<t::Failure>(late_cancel));
  require(std::get<t::Failure>(late_cancel).code == t::ErrorCode::Conflict);
  // Second sweep is idempotent: the occurrence and outbox already exist.
  require(std::get<std::size_t>(harness.service.due_sweep(at(1300), 10)) == 0);
  require(harness.store->occurrences.size() == 1);
  require(harness.store->outbox.size() == 1);
  const auto& running = harness.store->tasks[task_key(t::TaskKey{context.owner, "task-due"})];
  require(running.state == t::TaskState::Running);
}

void test_claim_confirm_and_confirmation_loss() {
  Harness harness;
  const auto context = harness.context();
  require(std::holds_alternative<t::ScheduledTask>(
      harness.service.create_task(context, harness.input("task-confirm", at(1200)))));
  require(std::get<std::size_t>(harness.service.due_sweep(at(1300), 10)) == 1);
  auto claimed = harness.service.claim_triggers(at(1300), 10, 30s);
  require(std::holds_alternative<std::vector<t::SchedulerTriggerClaim>>(claimed));
  const auto claims = std::get<std::vector<t::SchedulerTriggerClaim>>(claimed);
  require(claims.size() == 1 && claims.front().occurrence.attempts == 1);
  auto occurrence =
      harness.service.confirm_trigger_accepted(claims.front().claim, "event-1", at(1301));
  require(std::holds_alternative<t::Occurrence>(occurrence));
  require(std::get<t::Occurrence>(occurrence).state == t::OperationState::Succeeded);
  const auto& task = harness.store->tasks[task_key(t::TaskKey{context.owner, "task-confirm"})];
  require(task.state == t::TaskState::Completed);
  // Confirmation loss: retry with the same occurrence/token is idempotent.
  auto retried =
      harness.service.confirm_trigger_accepted(claims.front().claim, "event-1", at(1302));
  require(std::holds_alternative<t::Occurrence>(retried));
  require(harness.store->outbox.front().state == t::OutboxState::Confirmed);
}

void test_expiry_marks_task_failed_timeout() {
  Harness harness;
  const auto context = harness.context();
  require(std::holds_alternative<t::ScheduledTask>(
      harness.service.create_task(context, harness.input("task-expire", at(1200)))));
  require(std::get<std::size_t>(harness.service.due_sweep(at(1300), 10)) == 1);
  const auto valid_until = harness.store->occurrences.begin()->second.handoff.valid_until;
  auto expired = harness.service.expire_stale_triggers(valid_until + 1s, 10);
  require(std::holds_alternative<std::size_t>(expired));
  require(std::get<std::size_t>(expired) == 1);
  const auto occurrence = harness.store->occurrences.begin()->second;
  require(occurrence.state == t::OperationState::Expired);
  require(occurrence.last_error == t::ErrorCode::Timeout);
  const auto& task = harness.store->tasks[task_key(t::TaskKey{context.owner, "task-expire"})];
  require(task.state == t::TaskState::Failed && task.reason == t::ErrorCode::Timeout);
}

void test_list_get_and_operation_scope() {
  Harness harness;
  const auto context = harness.context();
  for (int index = 0; index < 4; ++index) {
    require(std::holds_alternative<t::ScheduledTask>(harness.service.create_task(
        context, harness.input("task-list-" + std::to_string(index), at(1500)))));
  }
  auto first = harness.service.list_tasks(context, std::nullopt, 2, "");
  require(std::holds_alternative<t::SchedulerListPage>(first));
  const auto& page = std::get<t::SchedulerListPage>(first);
  require(page.tasks.size() == 2 && !page.next_page_token.empty());
  auto second = harness.service.list_tasks(context, std::nullopt, 2, page.next_page_token);
  require(std::get<t::SchedulerListPage>(second).tasks.size() == 2);
  auto other = harness.context();
  other.owner = t::InstanceId{uuid(9)};
  auto isolated = harness.service.get_task(other, "task-list-0");
  require(std::holds_alternative<t::Failure>(isolated));
  require(std::get<t::Failure>(isolated).code == t::ErrorCode::NotFound);
  require(
      std::holds_alternative<t::ScheduledTask>(harness.service.get_task(context, "task-list-0")));
  require(std::holds_alternative<t::ScheduledTask>(
      harness.service.create_task(context, harness.input("task-op", at(1200)))));
  require(std::get<std::size_t>(harness.service.due_sweep(at(1300), 10)) == 1);
  const auto operation = harness.store->operations.begin()->second;
  auto found = harness.service.get_operation(context, operation.ref);
  require(std::holds_alternative<t::Operation>(found));
  auto wrong_kind = operation.ref;
  wrong_kind.resource.kind = t::ResourceKind::Account;
  require(std::holds_alternative<t::Failure>(harness.service.get_operation(context, wrong_kind)));
}

void test_deliver_trigger_success_rejection_and_retry() {
  Harness harness;
  const auto context = harness.context();
  require(std::holds_alternative<t::ScheduledTask>(
      harness.service.create_task(context, harness.input("task-deliver", at(1200)))));
  require(std::get<std::size_t>(harness.service.due_sweep(at(1300), 10)) == 1);
  auto delivered = harness.service.deliver_triggers(at(1300), 5);
  require(std::holds_alternative<t::SchedulerDeliverySummary>(delivered));
  require(std::get<t::SchedulerDeliverySummary>(delivered).accepted == 1);
  require(harness.delivery.accepted == 1);
  require(harness.delivery.last_request.occurrence_id.value ==
          harness.store->occurrences.begin()->first);
  require(harness.delivery.last_request.payload.size() == 2);
  const auto& completed = harness.store->tasks[task_key(t::TaskKey{context.owner, "task-deliver"})];
  require(completed.state == t::TaskState::Completed);
  require(harness.store->occurrences.begin()->second.state == t::OperationState::Succeeded);

  // Duplicate Runtime acceptance (lost Scheduler receipt) confirms exactly once.
  Harness duplicate_harness;
  const auto duplicate_context = duplicate_harness.context();
  require(std::holds_alternative<t::ScheduledTask>(duplicate_harness.service.create_task(
      duplicate_context, duplicate_harness.input("task-dup", at(1200)))));
  require(std::get<std::size_t>(duplicate_harness.service.due_sweep(at(1300), 10)) == 1);
  duplicate_harness.delivery.duplicate = true;
  const auto duplicate_result = duplicate_harness.service.deliver_triggers(at(1300), 5);
  require(std::holds_alternative<t::SchedulerDeliverySummary>(duplicate_result));
  require(std::get<t::SchedulerDeliverySummary>(duplicate_result).duplicates == 1);
  require(duplicate_harness.store->tasks[task_key(t::TaskKey{duplicate_context.owner, "task-dup"})]
              .state == t::TaskState::Completed);

  // Permanent Runtime rejection marks the occurrence/task FAILED and concludes the outbox handoff.
  Harness rejected_harness;
  const auto rejected_context = rejected_harness.context();
  require(std::holds_alternative<t::ScheduledTask>(rejected_harness.service.create_task(
      rejected_context, rejected_harness.input("task-rejected", at(1200)))));
  require(std::get<std::size_t>(rejected_harness.service.due_sweep(at(1300), 10)) == 1);
  rejected_harness.delivery.scripted = failure(t::ErrorCode::InvalidArgument);
  const auto rejected_result = rejected_harness.service.deliver_triggers(at(1300), 5);
  require(std::holds_alternative<t::SchedulerDeliverySummary>(rejected_result));
  require(std::get<t::SchedulerDeliverySummary>(rejected_result).rejected == 1);
  const auto rejected_task =
      rejected_harness.store->tasks[task_key(t::TaskKey{rejected_context.owner, "task-rejected"})];
  require(rejected_task.state == t::TaskState::Failed &&
          rejected_task.reason == t::ErrorCode::InvalidArgument);
  require(rejected_harness.store->occurrences.begin()->second.state == t::OperationState::Failed);

  // Retryable Runtime failure leaves the occurrence ACCEPTED and a later claim retries the same
  // occurrence/event id (never a COMPLETED state and never a duplicated event identity).
  Harness retry_harness;
  const auto retry_context = retry_harness.context();
  require(std::holds_alternative<t::ScheduledTask>(retry_harness.service.create_task(
      retry_context, retry_harness.input("task-retry", at(1200)))));
  require(std::get<std::size_t>(retry_harness.service.due_sweep(at(1300), 10)) == 1);
  retry_harness.delivery.scripted = failure(t::ErrorCode::UpstreamUnavailable);
  const auto retried = retry_harness.service.deliver_triggers(at(1300), 5);
  require(std::holds_alternative<t::SchedulerDeliverySummary>(retried));
  require(std::get<t::SchedulerDeliverySummary>(retried).retried == 1);
  require(retry_harness.store->occurrences.begin()->second.state == t::OperationState::Accepted);
  require(
      retry_harness.store->tasks[task_key(t::TaskKey{retry_context.owner, "task-retry"})].state ==
      t::TaskState::Running);
  retry_harness.delivery.scripted.reset();
  const auto retried_again = retry_harness.service.deliver_triggers(at(1300), 5);
  require(std::holds_alternative<t::SchedulerDeliverySummary>(retried_again));
  require(std::get<t::SchedulerDeliverySummary>(retried_again).accepted == 1);
  require(retry_harness.delivery.last_request.occurrence_id ==
          retry_harness.store->occurrences.begin()->second.occurrence_id);
}

void run() {
  test_create_replay_capacity_and_invalid_time();
  test_local_time_gap_fold_and_missing_zone_data();
  test_cancel_window_and_handoff_lifecycle();
  test_claim_confirm_and_confirmation_loss();
  test_expiry_marks_task_failed_timeout();
  test_list_get_and_operation_scope();
  test_deliver_trigger_success_rejection_and_retry();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "scheduler service fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
