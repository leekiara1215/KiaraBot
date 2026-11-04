#include "kiarabot/core/application/target/scheduler_service.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kiarabot::core::target {
namespace {

Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose,
                Acceptance acceptance = Acceptance::NotAccepted) {
  return {code, acceptance, recovery, std::nullopt, std::nullopt};
}
Failure internal_failure() {
  return failure(ErrorCode::Internal, Recovery::QuerySameIdentity, Acceptance::Unknown);
}
Failure unavailable() {
  return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
Failure not_found() {
  return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
}
Failure conflict() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}
Failure limited() {
  return failure(ErrorCode::RateLimited, Recovery::Backoff);
}

bool uuid(std::string_view value) {
  return canonical_id(value);
}
bool valid_context(const SchedulerCommandContext& context, bool mutating) {
  if (!uuid(context.owner.value) || !uuid(context.installation.value) ||
      !uuid(context.request_id) || !uuid(context.trace_id) ||
      context.deadline.time_since_epoch().count() <= 0) {
    return false;
  }
  if (mutating && (context.idempotency_key.size() < 16 || context.idempotency_key.size() > 128)) {
    return false;
  }
  return true;
}
ResourceRef task_resource(std::string_view task_id) {
  return ResourceRef{ResourceKind::Task, ResourceId{std::string(task_id)}};
}
ReceiptKey receipt_key(const SchedulerCommandContext& context, std::string_view method,
                       std::string_view task_id) {
  return ReceiptKey{SubjectId(context.owner.value), std::string(method), task_resource(task_id),
                    context.idempotency_key};
}
Failure commit_failure(CommitResult result) {
  if (result == CommitResult::Unknown) {
    return failure(ErrorCode::UpstreamUnavailable, Recovery::QuerySameIdentity,
                   Acceptance::Unknown);
  }
  return unavailable();
}
const Failure* status_failure(const Status& status) {
  return std::get_if<Failure>(&status);
}
template <typename Value> const Failure* outcome_failure(const Value& outcome) {
  return std::get_if<Failure>(&outcome);
}
// Safe scheduler task projection for the durable v1.2 task feed: metadata only, no task payload or
// target package. Staged in the same UoW as the state mutation so rollback publishes nothing.
const Failure* stage_task_feed(UnitOfWork& uow, const ScheduledTask& task) {
  ObservationFeedAppend append;
  append.resource = ResourceRef{ResourceKind::Task, ResourceId{task.key.task_id}};
  TaskObservation observation;
  observation.owner_instance_id = task.key.owner.value;
  observation.task_id = task.key.task_id;
  observation.state = static_cast<TaskFeedState>(static_cast<int>(task.state));
  observation.version = task.version.value;
  observation.updated_at = task.updated_at;
  observation.cancel_requested = task.cancel_requested;
  if (!task.occurrence.value.empty()) {
    observation.occurrence_id = task.occurrence.value;
  }
  observation.reason_code = task.reason;
  append.payload = std::move(observation);
  append.observed_at = task.updated_at;
  const auto appended = uow.append_observation_changes(ObservationFamily::Tasks, {append});
  if (const auto* failure = status_failure(appended)) {
    // A UoW adapter that does not implement the feed journal explicitly reports
    // CapabilityNotSupported (the frozen default is not a silent empty journal). Treat that as
    // "feed not enabled for this adapter" so legacy/fake adapters keep their pre-v1.2 behavior; the
    // production PostgreSQL adapter implements the port, so business mutations there fail closed
    // if a real journal append cannot be staged.
    if (failure->code == ErrorCode::CapabilityNotSupported) {
      return nullptr;
    }
    return failure;
  }
  return nullptr;
}

} // namespace

SchedulerService::SchedulerService(SchedulerServiceDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

Service SchedulerService::service() const noexcept {
  return Service::Scheduler;
}

ServiceReadiness SchedulerService::readiness() const {
  const bool zones = dependencies_.timezone.available();
  const bool runtime = dependencies_.delivery != nullptr && dependencies_.delivery->available();
  return {
      zones && runtime,
      {{Dependency::BusinessImplementation, DependencyState::Available},
       {Dependency::DatabaseSchema, DependencyState::Unverified},
       {Dependency::Runtime, runtime ? DependencyState::Available : DependencyState::Unavailable}}};
}

Outcome<ScheduledTask> SchedulerService::create_task(const SchedulerCommandContext& context,
                                                     const TaskCreationInput& input) {
  if (!valid_context(context, true)) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto now_result = uow->current_time();
  if (const auto* failed = std::get_if<Failure>(&now_result)) {
    uow->rollback();
    return *failed;
  }
  const auto now = std::get<TimePoint>(now_result);
  const auto hashed = creation_fingerprint(input, dependencies_.digests);
  if (const auto* failed = outcome_failure(hashed)) {
    uow->rollback();
    return *failed;
  }
  const auto fingerprint = std::get<Sha256Digest>(hashed);
  const auto receipt = receipt_key(context, "SchedulerCreateTask", input.task_id);
  const auto existing = uow->receipts().find(receipt);
  if (const auto* failed = outcome_failure(existing)) {
    uow->rollback();
    return *failed;
  }
  if (const auto& stored = std::get<std::optional<CommandReceipt>>(existing)) {
    if (stored->fingerprint != fingerprint) {
      uow->rollback();
      return conflict();
    }
    if (const auto* task = std::get_if<ScheduledTask>(&stored->result)) {
      uow->rollback();
      return *task;
    }
    uow->rollback();
    return internal_failure();
  }
  const auto counts = uow->count_tasks(context.owner, context.installation);
  if (const auto* failed = outcome_failure(counts)) {
    uow->rollback();
    return *failed;
  }
  const auto [instance_tasks, installation_tasks] =
      std::get<std::pair<std::size_t, std::size_t>>(counts);
  if (instance_tasks >= dependencies_.instance_capacity ||
      installation_tasks >= dependencies_.installation_capacity) {
    uow->rollback();
    return limited();
  }
  const auto run_at = resolve_run_at(input, dependencies_.timezone, now, dependencies_.horizon);
  if (const auto* failed = std::get_if<Failure>(&run_at)) {
    uow->rollback();
    return *failed;
  }
  const auto occurrence = dependencies_.ids.occurrence_id();
  if (const auto* failed = std::get_if<Failure>(&occurrence)) {
    uow->rollback();
    return *failed;
  }
  ScheduledTask task;
  task.key = TaskKey{context.owner, input.task_id};
  task.installation = context.installation;
  task.target = input.target;
  task.payload = input.payload;
  task.run_at = std::get<TimePoint>(run_at);
  task.state = TaskState::Pending;
  task.version = ResourceVersion{1};
  task.occurrence = std::get<MessageId>(occurrence);
  task.created_at = now;
  task.updated_at = now;
  const auto saved = uow->tasks().save(task, std::nullopt);
  if (const auto* failed = outcome_failure(saved)) {
    uow->rollback();
    return *failed;
  }
  CommandReceipt written{receipt, fingerprint, task, now, now + kTriggerValidFor};
  const auto stored = uow->receipts().insert(written, ReceiptCapacity{});
  if (const auto* failed = outcome_failure(stored)) {
    uow->rollback();
    return *failed;
  }
  if (const auto* failed = stage_task_feed(*uow, task)) {
    uow->rollback();
    return *failed;
  }
  const auto committed = uow->commit();
  if (committed != CommitResult::Committed) {
    return commit_failure(committed);
  }
  return task;
}

Outcome<ScheduledTask> SchedulerService::cancel_task(const SchedulerCommandContext& context,
                                                     std::string_view task_id) {
  if (!valid_context(context, true) || !task_id_text(task_id)) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto now_result = uow->current_time();
  if (const auto* failed = std::get_if<Failure>(&now_result)) {
    uow->rollback();
    return *failed;
  }
  const auto now = std::get<TimePoint>(now_result);
  const auto receipt = receipt_key(context, "SchedulerCancelTask", task_id);
  const auto existing = uow->receipts().find(receipt);
  if (const auto* failed = outcome_failure(existing)) {
    uow->rollback();
    return *failed;
  }
  if (const auto& stored = std::get<std::optional<CommandReceipt>>(existing)) {
    if (const auto* task = std::get_if<ScheduledTask>(&stored->result)) {
      uow->rollback();
      return *task;
    }
    uow->rollback();
    return internal_failure();
  }
  const auto found = uow->tasks().find(TaskKey{context.owner, std::string(task_id)});
  if (const auto* failed = outcome_failure(found)) {
    uow->rollback();
    return *failed;
  }
  const auto& stored_task = std::get<std::optional<ScheduledTask>>(found);
  if (!stored_task) {
    uow->rollback();
    return not_found();
  }
  auto task = *stored_task;
  if (task.key.owner != context.owner ||
      (context.platform_peer && (task.installation != context.installation ||
                                 (context.account && !task.target.account.value.empty() &&
                                  task.target.account != *context.account)))) {
    uow->rollback();
    return not_found();
  }
  if (task.state != TaskState::Pending || task.cancel_requested) {
    // Handoff has started or the task is already terminal: never pretend to retract it.
    uow->rollback();
    return conflict();
  }
  const auto occurrence = uow->occurrences().find(task.occurrence);
  if (const auto* failed = outcome_failure(occurrence)) {
    uow->rollback();
    return *failed;
  }
  if (std::get<std::optional<Occurrence>>(occurrence)) {
    uow->rollback();
    return conflict();
  }
  const auto expected = task.version;
  task.state = TaskState::Cancelled;
  task.cancel_requested = true;
  task.version = ResourceVersion{task.version.value + 1};
  task.updated_at = now;
  const auto saved = uow->tasks().save(task, expected);
  if (const auto* failed = outcome_failure(saved)) {
    uow->rollback();
    return *failed;
  }
  const auto hashed = creation_fingerprint(
      TaskCreationInput{std::string(task_id), {}, {}, std::nullopt, LocalCivilTime{}},
      dependencies_.digests);
  if (const auto* failed = outcome_failure(hashed)) {
    uow->rollback();
    return *failed;
  }
  CommandReceipt written{receipt, std::get<Sha256Digest>(hashed), task, now,
                         now + kTriggerValidFor};
  const auto stored = uow->receipts().insert(written, ReceiptCapacity{});
  if (const auto* failed = outcome_failure(stored)) {
    uow->rollback();
    return *failed;
  }
  if (const auto* failed = stage_task_feed(*uow, task)) {
    uow->rollback();
    return *failed;
  }
  const auto committed = uow->commit();
  if (committed != CommitResult::Committed) {
    return commit_failure(committed);
  }
  return task;
}

Outcome<ScheduledTask> SchedulerService::get_task(const SchedulerCommandContext& context,
                                                  std::string_view task_id) {
  if (!valid_context(context, false) || !task_id_text(task_id)) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto found = uow->tasks().find(TaskKey{context.owner, std::string(task_id)});
  uow->rollback();
  if (const auto* failed = outcome_failure(found)) {
    return *failed;
  }
  const auto& task = std::get<std::optional<ScheduledTask>>(found);
  if (!task || task->key.owner != context.owner) {
    return not_found();
  }
  return *task;
}

Outcome<SchedulerListPage> SchedulerService::list_tasks(const SchedulerCommandContext& context,
                                                        std::optional<TaskState> state,
                                                        std::uint32_t page_size,
                                                        std::string_view page_token) {
  if (!valid_context(context, false) || page_size == 0 || page_size > 100) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto listed = uow->list_tasks(context.owner, page_size + 1, std::string(page_token));
  uow->rollback();
  if (const auto* failed = outcome_failure(listed)) {
    return *failed;
  }
  auto tasks = std::get<std::vector<ScheduledTask>>(listed);
  SchedulerListPage page;
  for (auto& task : tasks) {
    if (context.platform_peer && (task.installation != context.installation ||
                                  (context.account && !task.target.account.value.empty() &&
                                   task.target.account != *context.account))) {
      // A Runtime platform peer may only observe its deployment-authorized instance/installation/
      // account; any mismatched row is withheld rather than exposed or turned into an error.
      continue;
    }
    if (state && task.state != *state) {
      continue;
    }
    page.tasks.push_back(std::move(task));
  }
  if (page.tasks.size() > page_size) {
    page.tasks.resize(page_size);
    page.next_page_token = page.tasks.back().key.task_id;
  }
  return page;
}

Outcome<Occurrence> SchedulerService::get_occurrence(const SchedulerCommandContext& context,
                                                     std::string_view occurrence_id) {
  if (!valid_context(context, false) || !uuid(occurrence_id)) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto found = uow->occurrences().find(MessageId{std::string(occurrence_id)});
  uow->rollback();
  if (const auto* failed = outcome_failure(found)) {
    return *failed;
  }
  const auto& occurrence = std::get<std::optional<Occurrence>>(found);
  if (!occurrence || occurrence->owner != context.owner) {
    return not_found();
  }
  return *occurrence;
}

Outcome<Operation> SchedulerService::get_operation(const SchedulerCommandContext& context,
                                                   const OperationRef& operation) {
  if (!valid_context(context, false) || operation.owner != Service::Scheduler ||
      !uuid(operation.id.value) || operation.resource.kind != ResourceKind::Task) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto found = uow->operations().find(operation.id);
  uow->rollback();
  if (const auto* failed = outcome_failure(found)) {
    return *failed;
  }
  const auto& stored = std::get<std::optional<Operation>>(found);
  if (!stored || stored->ref.owner != Service::Scheduler ||
      stored->ref.resource.id != operation.resource.id) {
    return not_found();
  }
  return *stored;
}

Outcome<std::size_t> SchedulerService::due_sweep(TimePoint now, std::size_t limit) {
  if (limit == 0 || limit > 100) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto listed = uow->due_tasks(now, limit);
  uow->rollback();
  if (const auto* failed = outcome_failure(listed)) {
    return *failed;
  }
  std::size_t activated = 0;
  for (const auto& candidate : std::get<std::vector<ScheduledTask>>(listed)) {
    auto task_work = dependencies_.transactions.begin();
    if (const auto* failed = std::get_if<Failure>(&task_work)) {
      return *failed;
    }
    auto task_uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(task_work));
    const auto stamp_result = task_uow->current_time();
    if (const auto* failed = std::get_if<Failure>(&stamp_result)) {
      task_uow->rollback();
      return *failed;
    }
    const auto stamp = std::get<TimePoint>(stamp_result);
    const auto found = task_uow->tasks().find(candidate.key);
    if (const auto* failed = outcome_failure(found)) {
      task_uow->rollback();
      return *failed;
    }
    const auto& current = std::get<std::optional<ScheduledTask>>(found);
    if (!current || current->state != TaskState::Pending || current->run_at > now) {
      task_uow->rollback();
      continue;
    }
    const auto existing_occurrence = task_uow->occurrences().find(current->occurrence);
    if (const auto* failed = outcome_failure(existing_occurrence)) {
      task_uow->rollback();
      return *failed;
    }
    if (std::get<std::optional<Occurrence>>(existing_occurrence)) {
      task_uow->rollback();
      continue;
    }
    const auto operation_id = dependencies_.ids.operation_id();
    if (const auto* failed = std::get_if<Failure>(&operation_id)) {
      task_uow->rollback();
      return *failed;
    }
    const auto resource = task_resource(current->key.task_id);
    Operation operation;
    operation.ref = OperationRef{Service::Scheduler, std::get<OperationId>(operation_id), resource};
    operation.version = ResourceVersion{1};
    operation.state = OperationState::Accepted;
    operation.execution_outcome = ExecutionOutcome::NotStarted;
    const auto saved_operation = task_uow->operations().save(operation, std::nullopt);
    if (const auto* failed = outcome_failure(saved_operation)) {
      task_uow->rollback();
      return *failed;
    }
    HandoffContext handoff;
    handoff.source = Service::Scheduler;
    handoff.message = current->occurrence;
    handoff.operation = operation.ref;
    handoff.resource = resource;
    handoff.version = current->version;
    handoff.actor = SubjectId{current->key.owner.value};
    handoff.authorization_version = ResourceVersion{0};
    const auto hashed = dependencies_.digests.sha256(std::string_view(
        reinterpret_cast<const char*>(current->payload.data()), current->payload.size()));
    if (const auto* failed = outcome_failure(hashed)) {
      task_uow->rollback();
      return *failed;
    }
    handoff.payload_hash = std::get<Sha256Digest>(hashed);
    handoff.created_at = stamp;
    handoff.valid_until = stamp + kTriggerValidFor;
    Occurrence occurrence;
    occurrence.owner = current->key.owner;
    occurrence.installation = current->installation;
    occurrence.task_id = current->key.task_id;
    occurrence.task_version = current->version;
    occurrence.occurrence_id = current->occurrence;
    occurrence.due_at = current->run_at;
    occurrence.handoff = handoff;
    occurrence.version = ResourceVersion{1};
    occurrence.state = OperationState::Accepted;
    occurrence.created_at = stamp;
    occurrence.updated_at = stamp;
    const auto saved_occurrence = task_uow->occurrences().save(occurrence, std::nullopt);
    if (const auto* failed = outcome_failure(saved_occurrence)) {
      task_uow->rollback();
      return *failed;
    }
    OutboxEntry entry;
    entry.context = handoff;
    entry.payload_reference = ImmutablePayloadRef{resource, current->version};
    entry.lane = OutboxLane::Ordinary;
    entry.state = OutboxState::Pending;
    entry.next_attempt_at = stamp;
    const auto enqueued = task_uow->journal().enqueue(entry, JournalCapacity{}, now);
    if (const auto* failed = status_failure(enqueued)) {
      task_uow->rollback();
      return *failed;
    }
    const auto expected = current->version;
    auto updated = *current;
    updated.state = TaskState::Running;
    updated.version = ResourceVersion{current->version.value + 1};
    updated.updated_at = stamp;
    const auto saved_task = task_uow->tasks().save(updated, expected);
    if (const auto* failed = outcome_failure(saved_task)) {
      task_uow->rollback();
      return *failed;
    }
    if (const auto* failed = stage_task_feed(*task_uow, updated)) {
      task_uow->rollback();
      return *failed;
    }
    const auto committed = task_uow->commit();
    if (committed != CommitResult::Committed) {
      return commit_failure(committed);
    }
    ++activated;
  }
  return activated;
}

Outcome<std::vector<SchedulerTriggerClaim>>
SchedulerService::claim_triggers(TimePoint now, std::size_t limit, std::chrono::seconds lease) {
  if (limit == 0 || limit > 100 || lease <= std::chrono::seconds{0} ||
      lease > std::chrono::seconds{30}) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto stamp_result = uow->current_time();
  if (const auto* failed = std::get_if<Failure>(&stamp_result)) {
    uow->rollback();
    return *failed;
  }
  const auto stamp = std::get<TimePoint>(stamp_result);
  const auto claimed = uow->journal().claim(Service::Scheduler, limit, now, lease);
  if (const auto* failed = outcome_failure(claimed)) {
    uow->rollback();
    return *failed;
  }
  std::vector<SchedulerTriggerClaim> result;
  for (const auto& claim : std::get<std::vector<OutboxClaim>>(claimed)) {
    const auto found = uow->occurrences().find(claim.entry.context.message);
    if (const auto* failed = outcome_failure(found)) {
      uow->rollback();
      return *failed;
    }
    const auto& occurrence = std::get<std::optional<Occurrence>>(found);
    if (!occurrence) {
      continue;
    }
    auto updated = *occurrence;
    const auto expected = updated.version;
    updated.attempts += 1;
    updated.updated_at = stamp;
    updated.version = ResourceVersion{updated.version.value + 1};
    const auto saved = uow->occurrences().save(updated, expected);
    if (const auto* failed = outcome_failure(saved)) {
      uow->rollback();
      return *failed;
    }
    result.push_back({claim, updated});
  }
  const auto committed = uow->commit();
  if (committed != CommitResult::Committed) {
    return commit_failure(committed);
  }
  return result;
}

Outcome<Occurrence> SchedulerService::confirm_trigger_accepted(const OutboxClaim& claim,
                                                               std::string_view accepted_event_id,
                                                               TimePoint) {
  if (!uuid(claim.entry.context.message.value) ||
      (accepted_event_id.size() > 256 || accepted_event_id.empty())) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto stamp_result = uow->current_time();
  if (const auto* failed = std::get_if<Failure>(&stamp_result)) {
    uow->rollback();
    return *failed;
  }
  const auto stamp = std::get<TimePoint>(stamp_result);
  const auto found = uow->occurrences().find(claim.entry.context.message);
  if (const auto* failed = outcome_failure(found)) {
    uow->rollback();
    return *failed;
  }
  const auto& stored = std::get<std::optional<Occurrence>>(found);
  if (!stored) {
    uow->rollback();
    return not_found();
  }
  if (stored->state == OperationState::Succeeded) {
    uow->rollback();
    return *stored; // Confirmation lost and retried: same occurrence, no duplicate effect.
  }
  if (stored->state != OperationState::Accepted) {
    uow->rollback();
    return conflict();
  }
  const auto confirmed = uow->journal().confirm(
      HandoffKey{Service::Scheduler, claim.entry.context.message}, claim.claim_token, stamp);
  if (const auto* failed = status_failure(confirmed)) {
    uow->rollback();
    return *failed;
  }
  auto occurrence = *stored;
  const auto expected_occurrence = occurrence.version;
  occurrence.state = OperationState::Succeeded;
  occurrence.accepted_event_id = std::string(accepted_event_id);
  occurrence.acknowledged_at = stamp;
  occurrence.updated_at = stamp;
  occurrence.version = ResourceVersion{occurrence.version.value + 1};
  const auto saved_occurrence = uow->occurrences().save(occurrence, expected_occurrence);
  if (const auto* failed = outcome_failure(saved_occurrence)) {
    uow->rollback();
    return *failed;
  }
  const auto task_found = uow->tasks().find(TaskKey{occurrence.owner, occurrence.task_id});
  if (const auto* failed = outcome_failure(task_found)) {
    uow->rollback();
    return *failed;
  }
  if (const auto& task = std::get<std::optional<ScheduledTask>>(task_found)) {
    auto updated = *task;
    const auto expected_task = updated.version;
    updated.state = TaskState::Completed;
    updated.updated_at = stamp;
    updated.version = ResourceVersion{updated.version.value + 1};
    const auto saved_task = uow->tasks().save(updated, expected_task);
    if (const auto* failed = outcome_failure(saved_task)) {
      uow->rollback();
      return *failed;
    }
    if (const auto* failed = stage_task_feed(*uow, updated)) {
      uow->rollback();
      return *failed;
    }
  }
  const auto committed = uow->commit();
  if (committed != CommitResult::Committed) {
    return commit_failure(committed);
  }
  return occurrence;
}

Outcome<std::size_t> SchedulerService::expire_stale_triggers(TimePoint now, std::size_t limit) {
  if (limit == 0 || limit > 100) {
    return scheduler_invalid();
  }
  auto work = dependencies_.transactions.begin();
  if (const auto* failed = std::get_if<Failure>(&work)) {
    return *failed;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto stamp_result = uow->current_time();
  if (const auto* failed = std::get_if<Failure>(&stamp_result)) {
    uow->rollback();
    return *failed;
  }
  const auto stamp = std::get<TimePoint>(stamp_result);
  const auto listed = uow->inflight_occurrences(limit);
  if (const auto* failed = outcome_failure(listed)) {
    uow->rollback();
    return *failed;
  }
  std::size_t expired = 0;
  for (const auto& candidate : std::get<std::vector<Occurrence>>(listed)) {
    if (candidate.handoff.valid_until > now) {
      continue;
    }
    const auto current_found = uow->occurrences().find(candidate.occurrence_id);
    if (const auto* failed = outcome_failure(current_found)) {
      uow->rollback();
      return *failed;
    }
    const auto& current = std::get<std::optional<Occurrence>>(current_found);
    if (!current || current->state != OperationState::Accepted) {
      continue;
    }
    auto occurrence = *current;
    const auto expected_occurrence = occurrence.version;
    occurrence.state = OperationState::Expired;
    occurrence.last_error = ErrorCode::Timeout;
    occurrence.updated_at = stamp;
    occurrence.version = ResourceVersion{occurrence.version.value + 1};
    const auto saved_occurrence = uow->occurrences().save(occurrence, expected_occurrence);
    if (const auto* failed = outcome_failure(saved_occurrence)) {
      uow->rollback();
      return *failed;
    }
    const auto task_found = uow->tasks().find(TaskKey{occurrence.owner, occurrence.task_id});
    if (const auto* failed = outcome_failure(task_found)) {
      uow->rollback();
      return *failed;
    }
    if (const auto& task = std::get<std::optional<ScheduledTask>>(task_found)) {
      auto updated = *task;
      const auto expected_task = updated.version;
      updated.state = TaskState::Failed;
      updated.reason = ErrorCode::Timeout;
      updated.updated_at = stamp;
      updated.version = ResourceVersion{updated.version.value + 1};
      const auto saved_task = uow->tasks().save(updated, expected_task);
      if (const auto* failed = outcome_failure(saved_task)) {
        uow->rollback();
        return *failed;
      }
      if (const auto* failed = stage_task_feed(*uow, updated)) {
        uow->rollback();
        return *failed;
      }
    }
    ++expired;
  }
  const auto committed = uow->commit();
  if (committed != CommitResult::Committed) {
    return commit_failure(committed);
  }
  return expired;
}

Outcome<SchedulerDeliverySummary> SchedulerService::deliver_triggers(TimePoint now,
                                                                     std::size_t limit) {
  if (dependencies_.delivery == nullptr) {
    return failure(ErrorCode::CapabilityNotSupported, Recovery::Diagnose);
  }
  if (limit == 0 || limit > 20) {
    return scheduler_invalid();
  }
  auto claimed = claim_triggers(now, limit, std::chrono::seconds{20});
  if (const auto* rejected = outcome_failure(claimed)) {
    return *rejected;
  }
  SchedulerDeliverySummary summary;
  for (const auto& trigger : std::get<std::vector<SchedulerTriggerClaim>>(claimed)) {
    TimerDeliveryRequest request;
    request.occurrence_id = trigger.occurrence.occurrence_id;
    request.task_id = trigger.occurrence.task_id;
    request.owner_instance_id = trigger.occurrence.owner;
    request.schedule_version = trigger.occurrence.task_version;
    request.handoff = trigger.occurrence.handoff;
    request.trace_id = trigger.claim.entry.context.actor.value;
    auto work = dependencies_.transactions.begin();
    if (std::holds_alternative<Failure>(work)) {
      ++summary.retried;
      continue;
    }
    auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
    const auto task_found =
        uow->tasks().find(TaskKey{trigger.occurrence.owner, trigger.occurrence.task_id});
    if (const auto* rejected = outcome_failure(task_found)) {
      uow->rollback();
      (void)rejected;
      ++summary.retried;
      continue;
    }
    const auto& task = std::get<std::optional<ScheduledTask>>(task_found);
    if (!task) {
      uow->rollback();
      ++summary.retried;
      continue;
    }
    request.target = task->target;
    request.payload = task->payload;
    uow->rollback();
    const auto delivered = dependencies_.delivery->accept(request);
    if (const auto* rejected = std::get_if<Failure>(&delivered)) {
      const bool permanent = rejected->code == ErrorCode::InvalidArgument ||
                             rejected->code == ErrorCode::PermissionDenied ||
                             rejected->code == ErrorCode::Conflict ||
                             rejected->code == ErrorCode::CapabilityNotSupported;
      if (permanent) {
        const auto failed = fail_trigger(trigger.claim, rejected->code, now);
        if (const auto* failure = outcome_failure(failed)) {
          (void)failure;
          ++summary.retried;
        } else {
          ++summary.rejected;
        }
      } else {
        ++summary.retried; // Leave the claim; Runtime deduplicates the same occurrence/event id.
      }
      continue;
    }
    const auto& receipt = std::get<TimerDeliveryReceipt>(delivered);
    if (receipt.event_id.value.empty()) {
      ++summary.retried;
      continue;
    }
    const auto confirmed = confirm_trigger_accepted(trigger.claim, receipt.event_id.value, now);
    if (const auto* rejected = std::get_if<Failure>(&confirmed)) {
      (void)rejected;
      ++summary.retried;
      continue;
    }
    if (receipt.duplicate) {
      ++summary.duplicates;
    } else {
      ++summary.accepted;
    }
  }
  return summary;
}

Outcome<Occurrence> SchedulerService::fail_trigger(const OutboxClaim& claim, ErrorCode reason,
                                                   TimePoint) {
  auto work = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&work)) {
    return *rejected;
  }
  auto uow = std::move(std::get<std::unique_ptr<SchedulerUnitOfWork>>(work));
  const auto stamp_result = uow->current_time();
  if (const auto* rejected = std::get_if<Failure>(&stamp_result)) {
    uow->rollback();
    return *rejected;
  }
  const auto stamp = std::get<TimePoint>(stamp_result);
  const auto found = uow->occurrences().find(claim.entry.context.message);
  if (const auto* rejected = outcome_failure(found)) {
    uow->rollback();
    return *rejected;
  }
  const auto& stored = std::get<std::optional<Occurrence>>(found);
  if (!stored) {
    uow->rollback();
    return not_found();
  }
  if (stored->state != OperationState::Accepted) {
    uow->rollback();
    return *stored;
  }
  auto occurrence = *stored;
  const auto expected_occurrence = occurrence.version;
  occurrence.state = OperationState::Failed;
  occurrence.last_error = reason;
  occurrence.updated_at = stamp;
  occurrence.version = ResourceVersion{occurrence.version.value + 1};
  const auto saved_occurrence = uow->occurrences().save(occurrence, expected_occurrence);
  if (const auto* rejected = outcome_failure(saved_occurrence)) {
    uow->rollback();
    return *rejected;
  }
  const auto task_found = uow->tasks().find(TaskKey{occurrence.owner, occurrence.task_id});
  if (const auto* rejected = outcome_failure(task_found)) {
    uow->rollback();
    return *rejected;
  }
  if (const auto& task = std::get<std::optional<ScheduledTask>>(task_found)) {
    auto updated = *task;
    const auto expected_task = updated.version;
    updated.state = TaskState::Failed;
    updated.reason = reason;
    updated.updated_at = stamp;
    updated.version = ResourceVersion{updated.version.value + 1};
    const auto saved_task = uow->tasks().save(updated, expected_task);
    if (const auto* rejected = outcome_failure(saved_task)) {
      uow->rollback();
      return *rejected;
    }
    if (const auto* rejected = stage_task_feed(*uow, updated)) {
      uow->rollback();
      return *rejected;
    }
  }
  const auto confirmed = uow->journal().confirm(
      HandoffKey{Service::Scheduler, occurrence.occurrence_id}, claim.claim_token, stamp);
  if (const auto* rejected = status_failure(confirmed)) {
    uow->rollback();
    return *rejected;
  }
  const auto committed = uow->commit();
  if (committed != CommitResult::Committed) {
    return commit_failure(committed);
  }
  return occurrence;
}

} // namespace kiarabot::core::target
