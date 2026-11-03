// P13h-2 unit matrix for the production Scheduler lifecycle participant. The Scheduler transport
// is injected as a fake port; wire/mTLS behavior is covered separately by SchedulerTaskClient
// tests, and the real mTLS/PG lifecycle chain stays test-only until P13h-1 is merged.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kiarabot/core/application/target/runtime_lifecycle_participants.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/scheduler_task.hpp"

namespace {
namespace t = kiarabot::core::target;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}

class Ids final : public t::RuntimeIdPort {
public:
  t::Outcome<t::ResourceId> account_id() override {
    return t::ResourceId{id()};
  }
  t::Outcome<t::InstallationId> installation_id() override {
    return t::InstallationId{id()};
  }
  t::Outcome<t::InstanceId> instance_id() override {
    return t::InstanceId{id()};
  }
  t::Outcome<t::InstanceId> adapter_instance_id() override {
    return t::InstanceId{id()};
  }
  t::Outcome<t::ConnectionId> connection_id() override {
    return t::ConnectionId{id()};
  }
  t::Outcome<t::OperationId> operation_id() override {
    return t::OperationId{id()};
  }
  t::Outcome<t::RuntimeEpoch> runtime_epoch() override {
    return t::RuntimeEpoch{id()};
  }
  t::Outcome<std::string> snapshot_id() override {
    return id();
  }

private:
  std::string id() {
    char buffer[40] = {};
    std::snprintf(buffer, sizeof(buffer), "00000000-0000-4000-8000-%012u", ++counter_);
    return buffer;
  }
  unsigned counter_ = 0;
};

t::ScheduledTask task(std::string owner, std::string id, t::TaskState state) {
  t::ScheduledTask value;
  value.key = {t::InstanceId{std::move(owner)}, std::move(id)};
  value.state = state;
  value.run_at = t::TimePoint{std::chrono::seconds{100}};
  return value;
}

class FakeScheduler final : public t::SchedulerTaskPort {
public:
  bool available() const override {
    return true;
  }

  t::Outcome<t::ScheduledTask> create_task(const t::SchedulerTaskContext&,
                                           const t::TaskCreationInput&) override {
    return t::Failure{t::ErrorCode::CapabilityNotSupported, t::Acceptance::NotAccepted,
                      t::Recovery::Diagnose, std::nullopt, std::nullopt};
  }
  t::Outcome<t::ScheduledTask> get_task(const t::SchedulerTaskContext& context,
                                        std::string_view task_id) override {
    ++gets;
    last_context = context;
    last_task_id = task_id;
    return t::Failure{t::ErrorCode::NotFound, t::Acceptance::NotAccepted, t::Recovery::CorrectInput,
                      std::nullopt, std::nullopt};
  }
  t::Outcome<std::vector<t::ScheduledTask>> list_tasks(const t::SchedulerTaskContext& context,
                                                       std::optional<t::TaskState> state,
                                                       std::size_t page_size) override {
    ++lists;
    last_context = context;
    last_state = state;
    last_page_size = page_size;
    if (list_failure) {
      return *list_failure;
    }
    return listed;
  }
  t::Outcome<t::ScheduledTask> cancel_task(const t::SchedulerTaskContext& context,
                                           std::string_view task_id) override {
    ++cancels;
    last_context = context;
    last_task_id = task_id;
    canceled.push_back(std::string{task_id});
    if (cancel_failure) {
      return *cancel_failure;
    }
    return task(context.owner.value, std::string{task_id}, t::TaskState::Cancelled);
  }

  std::vector<t::ScheduledTask> listed;
  std::optional<t::Failure> list_failure;
  std::optional<t::Failure> cancel_failure;
  int lists = 0;
  int gets = 0;
  int cancels = 0;
  std::optional<t::TaskState> last_state;
  std::size_t last_page_size = 0;
  t::SchedulerTaskContext last_context;
  std::string last_task_id;
  std::vector<std::string> canceled;
};

constexpr auto kTarget = "11111111-1111-4111-8111-111111111111";
constexpr auto kOther = "22222222-2222-4222-8222-222222222222";

t::LifecycleParticipantResult run(FakeScheduler& scheduler, t::ResourceKind kind,
                                  const std::string& id) {
  Ids ids;
  auto participant = t::make_runtime_scheduler_lifecycle_participant(scheduler, ids);
  check(!std::holds_alternative<t::Failure>(participant), "participant factory");
  return std::get<std::unique_ptr<t::SchedulerLifecycleParticipantPort>>(participant)
      ->cancel_tasks(t::ResourceRef{kind, {id}});
}

void target_only_cancellation() {
  FakeScheduler scheduler;
  scheduler.listed = {task(kTarget, "pending", t::TaskState::Pending),
                      task(kOther, "foreign", t::TaskState::Pending),
                      task(kTarget, "running", t::TaskState::Running)};
  const auto result = run(scheduler, t::ResourceKind::Instance, kTarget);
  check(result.disposition == t::LifecycleParticipantDisposition::Done, "target task Done");
  check(scheduler.lists == 1 && scheduler.last_context.owner.value == kTarget,
        "list scoped to target instance");
  check(scheduler.last_state && *scheduler.last_state == t::TaskState::Pending,
        "list asks for pending tasks");
  check(scheduler.last_page_size == 100, "list bounded page size");
  check(scheduler.canceled == std::vector<std::string>{"pending"},
        "only target pending task canceled; foreign/running untouched");
  check(scheduler.last_context.idempotency_key.size() >= 16, "cancel carries idempotency key");
}

void empty_pending_is_done() {
  FakeScheduler scheduler;
  const auto result = run(scheduler, t::ResourceKind::Instance, kTarget);
  check(result.disposition == t::LifecycleParticipantDisposition::Done, "empty pending Done");
  check(scheduler.cancels == 0, "no cancel call for empty list");
}

void list_failure_blocks() {
  FakeScheduler scheduler;
  scheduler.list_failure = t::Failure{t::ErrorCode::UpstreamUnavailable, t::Acceptance::Unknown,
                                      t::Recovery::Backoff, std::nullopt, std::nullopt};
  const auto result = run(scheduler, t::ResourceKind::Instance, kTarget);
  check(result.disposition == t::LifecycleParticipantDisposition::Blocked,
        "offline ListTasks stays Blocked");
  check(result.reason && *result.reason == t::ErrorCode::UpstreamUnavailable,
        "offline reason preserved");
  check(scheduler.cancels == 0, "offline never cancels");
}

void cancel_failure_blocks() {
  FakeScheduler scheduler;
  scheduler.listed = {task(kTarget, "pending", t::TaskState::Pending)};
  scheduler.cancel_failure = t::Failure{t::ErrorCode::Timeout, t::Acceptance::Unknown,
                                        t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  const auto result = run(scheduler, t::ResourceKind::Instance, kTarget);
  check(result.disposition == t::LifecycleParticipantDisposition::Blocked,
        "UNKNOWN/timeout CancelTask stays Blocked");
  check(result.reason && *result.reason == t::ErrorCode::Timeout, "timeout reason preserved");
}

void installation_scope_fail_closed() {
  FakeScheduler scheduler;
  const auto result =
      run(scheduler, t::ResourceKind::Installation, "33333333-3333-4333-8333-333333333333");
  check(result.disposition == t::LifecycleParticipantDisposition::Blocked,
        "installation-wide cancellation blocked");
  check(result.reason && *result.reason == t::ErrorCode::CapabilityNotSupported,
        "installation scope stays unsupported");
  check(scheduler.lists == 0 && scheduler.cancels == 0,
        "installation never lists/cancels cross-instance tasks");
}
} // namespace

int main() {
  target_only_cancellation();
  empty_pending_is_done();
  list_failure_blocks();
  cancel_failure_blocks();
  installation_scope_fail_closed();
  std::cout << "P13h-2 scheduler lifecycle participant unit matrix passed\n";
  return 0;
}
