#include "kiarabot/core/application/task_scheduler.hpp"

#include "kiarabot/core/application/host_validation.hpp"
#include "kiarabot/core/domain/runtime_engine.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"
#include "kiarabot/core/runtime/runtime_state.hpp"

namespace kiarabot::core {
namespace {
namespace wire = kiarabot::v1;
constexpr std::size_t kTasksPerPlugin = 1024;
wire::Error error(wire::ErrorCode code, bool retryable = false) {
  wire::Error result;
  result.set_code(code);
  result.set_message(wire::ErrorCode_Name(code));
  result.set_retryable(retryable);
  return result;
}
wire::Error mapped(StorageError failure) {
  return error(failure == StorageError::Corrupt    ? wire::INTERNAL
               : failure == StorageError::Conflict ? wire::CONFLICT
                                                   : wire::UPSTREAM_UNAVAILABLE,
               failure != StorageError::Corrupt);
}
PortResult<std::unique_ptr<RuntimeTransaction>> begin(RuntimeStore& store) {
  auto result = store.begin();
  if (const auto* failure = std::get_if<StorageError>(&result)) {
    return mapped(*failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(result));
  if (!transaction) {
    return error(wire::UPSTREAM_UNAVAILABLE, true);
  }
  return transaction;
}
PortStatus commit(RuntimeTransaction& transaction) {
  const auto result = transaction.commit();
  if (const auto* failure = std::get_if<StorageError>(&result)) {
    return mapped(*failure);
  }
  return std::monostate{};
}
template <typename ResultType, typename Function>
PortResult<ResultType> edit(RuntimeStore& store, Function function) {
  auto begun = begin(store);
  if (const auto* failure = std::get_if<wire::Error>(&begun)) {
    return *failure;
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  PortResult<ResultType> result = function(transaction->snapshot());
  if (std::holds_alternative<wire::Error>(result)) {
    return result;
  }
  const auto committed = commit(*transaction);
  if (const auto* failure = std::get_if<wire::Error>(&committed)) {
    return *failure;
  }
  return result;
}
void timestamp(google::protobuf::Timestamp& target, WallTime time) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(time.time_since_epoch());
  target.set_seconds(seconds.count());
  target.set_nanos(static_cast<int>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch() - seconds)
          .count()));
}
wire::TaskResponse response(std::string_view id, const TaskEntry& entry) {
  wire::TaskResponse result;
  result.set_task_id(std::string(id));
  result.set_state(static_cast<wire::ScheduledTaskState>(entry.state));
  timestamp(*result.mutable_next_run_at(), entry.next_run_at);
  return result;
}
TaskEntry* find(RuntimeSnapshot& snapshot, std::string_view owner, std::string_view id) {
  const auto plugin = snapshot.tasks.find(std::string(owner));
  if (plugin == snapshot.tasks.end()) {
    return nullptr;
  }
  const auto task = plugin->second.find(std::string(id));
  return task == plugin->second.end() ? nullptr : &task->second;
}
PortResult<wire::TaskResponse> cancel_in(RuntimeSnapshot& snapshot, std::string_view owner,
                                         std::string_view id) {
  auto* entry = find(snapshot, owner, id);
  if (!entry) {
    return error(wire::NOT_FOUND);
  }
  if (entry->state == TaskState::Completed || entry->state == TaskState::Failed) {
    return error(wire::CONFLICT);
  }
  entry->cancel_requested = true;
  if (entry->state != TaskState::Running) {
    entry->state = TaskState::Cancelled;
    entry->claim_token.clear();
  }
  return response(id, *entry);
}
PortResult<TaskClaim> claim(std::string_view owner, const TaskEntry& entry) {
  TaskClaim result;
  result.task.owner_plugin_id = owner;
  if (!result.task.request.ParseFromString(entry.serialized_request)) {
    return error(wire::INTERNAL);
  }
  result.task.state = static_cast<wire::ScheduledTaskState>(entry.state);
  result.task.next_run_at = entry.next_run_at;
  result.task.attempts = entry.attempts;
  result.task.cancel_requested = entry.cancel_requested;
  result.claim_token = entry.claim_token;
  return result;
}
} // namespace

TaskScheduler::TaskScheduler(RuntimeStore& store, std::shared_ptr<ITimeZoneResolver> zones,
                             RuntimeClock clock, RuntimeLimits limits)
    : store_(store), zones_(std::move(zones)), clock_(std::move(clock)), limits_(limits) {}
RuntimeTimePoint TaskScheduler::now() const {
  return clock_ ? clock_() : std::chrono::system_clock::now();
}

PortResult<wire::TaskResponse> TaskScheduler::create_in(RuntimeSnapshot& snapshot,
                                                        std::string_view owner,
                                                        const wire::TaskCreateRequest& request) {
  const auto& id = request.task_id();
  if (owner.empty() || id.empty() || id.size() > 128 || request.target_conversation().empty() ||
      request.target_conversation().size() > 256 || request.payload().size() > 65536 ||
      !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '-' || c == '_';
      })) {
    return error(wire::INVALID_ARGUMENT);
  }
  if (auto* existing = find(snapshot, owner, id)) {
    wire::TaskCreateRequest original;
    if (!original.ParseFromString(existing->serialized_request)) {
      return error(wire::INTERNAL);
    }
    if (request_digest(original) != request_digest(request)) {
      return error(wire::CONFLICT);
    }
    return response(id, *existing);
  }
  WallTime run_at;
  if (request.has_run_at()) {
    const auto& date = request.run_at();
    if (date.seconds() < 0 || date.seconds() >= 4133980800LL || date.nanos() < 0 ||
        date.nanos() >= 1000000000) {
      return error(wire::INVALID_ARGUMENT);
    }
    run_at = WallTime(std::chrono::duration_cast<WallTime::duration>(
        std::chrono::seconds(date.seconds()) + std::chrono::nanoseconds(date.nanos())));
  } else if (request.has_local_time() && zones_) {
    const auto resolved = zones_->resolve(request.local_time());
    if (const auto* failure = std::get_if<wire::Error>(&resolved)) {
      return *failure;
    }
    run_at = std::get<WallTime>(resolved);
  } else {
    return error(wire::INVALID_ARGUMENT);
  }
  if (run_at <= now()) {
    return error(wire::INVALID_ARGUMENT);
  }
  auto& tasks = snapshot.tasks[std::string(owner)];
  if (tasks.size() >= kTasksPerPlugin) {
    return error(wire::RATE_LIMITED, true);
  }
  TaskEntry entry;
  entry.serialized_request = request.SerializeAsString();
  entry.scheduled_at = entry.next_run_at = run_at;
  entry.event_id = "timer-" + random_instance_id();
  const auto result = response(id, entry);
  tasks.emplace(id, std::move(entry));
  return result;
}

PortResult<wire::TaskResponse> TaskScheduler::create(std::string_view owner,
                                                     const wire::TaskCreateRequest& request) try {
  std::lock_guard lock(mutex_);
  return edit<wire::TaskResponse>(
      store_, [&](RuntimeSnapshot& snapshot) { return create_in(snapshot, owner, request); });
} catch (...) {
  return error(wire::INTERNAL);
}
PortResult<wire::TaskResponse> TaskScheduler::get(std::string_view owner, std::string_view id) try {
  std::lock_guard lock(mutex_);
  auto loaded = store_.read();
  if (const auto* failure = std::get_if<StorageError>(&loaded)) {
    return mapped(*failure);
  }
  auto* entry = find(std::get<RuntimeSnapshot>(loaded), owner, id);
  return entry ? PortResult<wire::TaskResponse>(response(id, *entry)) : error(wire::NOT_FOUND);
} catch (...) {
  return error(wire::INTERNAL);
}
PortResult<wire::TaskResponse> TaskScheduler::cancel(std::string_view owner,
                                                     std::string_view id) try {
  std::lock_guard lock(mutex_);
  return edit<wire::TaskResponse>(
      store_, [&](RuntimeSnapshot& snapshot) { return cancel_in(snapshot, owner, id); });
} catch (...) {
  return error(wire::INTERNAL);
}

wire::HostResponse TaskScheduler::dispatch(const PluginSession& session,
                                           const wire::HostRequest& request) try {
  std::lock_guard lock(mutex_);
  wire::HostResponse reply;
  reply.set_request_id(request.request_id());
  const auto reject = [&](wire::Error failure) {
    auto result = reply;
    result.clear_result();
    failure.set_request_id(request.request_id());
    failure.set_trace_id(request.trace_id());
    *result.mutable_error() = std::move(failure);
    return result;
  };
  if (const auto invalid = validate_host_request(request, now())) {
    return reject(*invalid);
  }
  if (!request.has_task_create() && !request.has_task_get() && !request.has_task_cancel()) {
    return reject(error(wire::CAPABILITY_NOT_SUPPORTED));
  }
  auto begun = begin(store_);
  if (const auto* failure = std::get_if<wire::Error>(&begun)) {
    return reject(*failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  auto& snapshot = transaction->snapshot();
  if (snapshot.audit.size() >= limits_.audit_limit) {
    return reject(error(wire::UPSTREAM_UNAVAILABLE, true));
  }
  const bool allowed = std::find(session.permissions.begin(), session.permissions.end(),
                                 "task.schedule") != session.permissions.end();
  const bool writing = !request.has_task_get();
  const auto original = snapshot;
  auto canonical = request;
  canonical.clear_request_id();
  canonical.clear_trace_id();
  canonical.clear_deadline();
  const auto fingerprint = request_digest(canonical);
  auto& receipts = snapshot.host_receipts[session.identity];
  if (allowed && writing) {
    const auto previous = receipts.find(request.idempotency_key());
    if (previous != receipts.end() && previous->second.expires_at > now()) {
      if (previous->second.fingerprint != fingerprint) {
        return reject(error(wire::CONFLICT));
      }
      if (!reply.ParseFromString(previous->second.serialized_response) || reply.has_error() ||
          (request.has_task_create() ? !reply.has_task_create() : !reply.has_task_cancel())) {
        return reject(error(wire::INTERNAL));
      }
      reply.set_request_id(request.request_id());
      return reply;
    }
  }
  PortResult<wire::TaskResponse> result = error(wire::PERMISSION_DENIED);
  if (allowed) {
    if (request.has_task_create()) {
      if (std::find(session.subscriptions.begin(), session.subscriptions.end(),
                    "event.timer.triggered") == session.subscriptions.end()) {
        result = error(wire::CAPABILITY_NOT_SUPPORTED);
      } else {
        result = create_in(snapshot, session.identity, request.task_create());
      }
    } else if (request.has_task_cancel()) {
      result = cancel_in(snapshot, session.identity, request.task_cancel().task_id());
    } else {
      const auto* entry = find(snapshot, session.identity, request.task_get().task_id());
      result = entry
                   ? PortResult<wire::TaskResponse>(response(request.task_get().task_id(), *entry))
                   : error(wire::NOT_FOUND);
    }
  }
  if (const auto* failure = std::get_if<wire::Error>(&result)) {
    reply = reject(*failure);
  } else {
    auto* output = request.has_task_create()   ? reply.mutable_task_create()
                   : request.has_task_cancel() ? reply.mutable_task_cancel()
                                               : reply.mutable_task_get();
    *output = std::get<wire::TaskResponse>(result);
    if (writing) {
      std::erase_if(receipts, [&](const auto& item) { return item.second.expires_at <= now(); });
      if (receipts.size() >= 10000) {
        reply = reject(error(wire::RATE_LIMITED, true));
      } else {
        receipts[request.idempotency_key()] = {fingerprint, reply.SerializeAsString(),
                                               now() + std::chrono::hours(168)};
      }
    }
  }
  if (reply.has_error()) {
    snapshot = original;
  }
  snapshot.audit.push_back({"plugin/" + session.identity, "task.schedule", session.identity,
                            request.request_id(), request.trace_id(), now(), allowed,
                            reply.has_error() ? wire::ErrorCode_Name(reply.error().code()) : "OK"});
  if (const auto overdue = validate_host_request(request, now())) {
    return reject(*overdue);
  }
  const auto committed = commit(*transaction);
  if (const auto* failure = std::get_if<wire::Error>(&committed)) {
    return reject(*failure);
  }
  return reply;
} catch (...) {
  wire::HostResponse result;
  result.set_request_id(request.request_id());
  *result.mutable_error() = error(wire::INTERNAL);
  return result;
}

PortResult<std::vector<TaskClaim>> TaskScheduler::claim_due(WallTime current,
                                                            std::size_t limit) try {
  std::lock_guard lock(mutex_);
  auto begun = begin(store_);
  if (const auto* failure = std::get_if<wire::Error>(&begun)) {
    return *failure;
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  std::vector<TaskClaim> result;
  for (auto& [owner, tasks] : transaction->snapshot().tasks) {
    for (auto& [id, entry] : tasks) {
      if (result.size() >= std::min(limit, std::size_t{32})) {
        break;
      }
      if ((entry.state != TaskState::Pending && entry.state != TaskState::Retrying) ||
          entry.next_run_at > current || entry.cancel_requested) {
        continue;
      }
      entry.state = TaskState::Running;
      ++entry.attempts;
      entry.claim_token = random_instance_id();
      auto claimed = claim(owner, entry);
      if (const auto* failure = std::get_if<wire::Error>(&claimed)) {
        return *failure;
      }
      result.push_back(std::get<TaskClaim>(std::move(claimed)));
    }
  }
  if (!result.empty()) {
    const auto committed = commit(*transaction);
    if (const auto* failure = std::get_if<wire::Error>(&committed)) {
      return *failure;
    }
  }
  return result;
} catch (...) {
  return error(wire::INTERNAL);
}

PortStatus TaskScheduler::finish(std::string_view owner, std::string_view id,
                                 std::string_view token, bool succeeded,
                                 std::optional<WallTime> retry_at) try {
  std::lock_guard lock(mutex_);
  return edit<std::monostate>(store_, [&](RuntimeSnapshot& snapshot) -> PortStatus {
    auto* entry = find(snapshot, owner, id);
    if (!entry) {
      return error(wire::NOT_FOUND);
    }
    if (entry->state != TaskState::Running || token.empty() || entry->claim_token != token) {
      return error(wire::CONFLICT);
    }
    if (entry->cancel_requested) {
      entry->state = TaskState::Cancelled;
    } else if (!succeeded) {
      if (retry_at && *retry_at > now() && entry->attempts < 5) {
        entry->state = TaskState::Retrying;
        entry->next_run_at = *retry_at;
      } else {
        entry->state = TaskState::Failed;
      }
    } else {
      wire::TaskCreateRequest request;
      if (!request.ParseFromString(entry->serialized_request)) {
        return error(wire::INTERNAL);
      }
      if (!snapshot.plugins.contains(std::string(owner))) {
        return error(wire::PLUGIN_UNAVAILABLE, true);
      }
      wire::EventEnvelope event;
      event.set_event_id(entry->event_id);
      event.set_trace_id(entry->event_id);
      event.set_schema_version("kiarabot.v1");
      event.set_delivery_mode(wire::AT_LEAST_ONCE);
      timestamp(*event.mutable_occurred_at(), now());
      event.mutable_subject()->set_conversation_id(request.target_conversation());
      auto* timer = event.mutable_timer_triggered();
      timer->set_task_id(request.task_id());
      timer->set_owner_plugin_id(std::string(owner));
      timer->set_payload(request.payload());
      timestamp(*timer->mutable_scheduled_at(), entry->scheduled_at);
      if (snapshot.events.contains(event.event_id())) {
        return error(wire::CONFLICT);
      }
      detail::RuntimeState state(snapshot);
      detail::RuntimeEngine engine(state, {}, clock_);
      const auto ingested = engine.ingest(
          {event.event_id(), event.trace_id(), "", 0, false, event.SerializeAsString()},
          {std::string(owner)});
      if (ingested != EventResult::accepted) {
        return error(ingested == EventResult::rate_limited ? wire::RATE_LIMITED : wire::INTERNAL,
                     true);
      }
      auto updated = state.snapshot();
      updated.audit = std::move(snapshot.audit);
      updated.commands = std::move(snapshot.commands);
      snapshot = std::move(updated);
      entry = find(snapshot, owner, id);
      entry->state = TaskState::Completed;
    }
    entry->claim_token.clear();
    if (snapshot.audit.size() >= limits_.audit_limit) {
      return error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    snapshot.audit.push_back(
        {"core/scheduler", "task.schedule", std::string(owner), entry->event_id, entry->event_id,
         now(), true,
         wire::ScheduledTaskState_Name(static_cast<wire::ScheduledTaskState>(entry->state))});
    return std::monostate{};
  });
} catch (...) {
  return error(wire::INTERNAL);
}

PortStatus TaskScheduler::recover() try {
  std::lock_guard lock(mutex_);
  return edit<std::monostate>(store_, [&](RuntimeSnapshot& snapshot) -> PortStatus {
    for (auto& [owner, tasks] : snapshot.tasks) {
      for (auto& [id, entry] : tasks) {
        if (entry.state != TaskState::Running) {
          continue;
        }
        entry.state = entry.cancel_requested ? TaskState::Cancelled
                      : entry.attempts >= 5  ? TaskState::Failed
                                             : TaskState::Retrying;
        entry.claim_token.clear();
        entry.next_run_at = now();
      }
    }
    return std::monostate{};
  });
} catch (...) {
  return error(wire::INTERNAL);
}

PortStatus TaskScheduler::tick() try {
  // Existing RUNNING claims are retried with their same token after transient write failure.
  auto loaded = store_.read();
  if (const auto* failure = std::get_if<StorageError>(&loaded)) {
    return mapped(*failure);
  }
  std::vector<TaskClaim> pending;
  for (const auto& [owner, tasks] : std::get<RuntimeSnapshot>(loaded).tasks) {
    for (const auto& [id, entry] : tasks) {
      if (entry.state == TaskState::Running && pending.size() < 32) {
        auto existing = claim(owner, entry);
        if (const auto* failure = std::get_if<wire::Error>(&existing)) {
          return *failure;
        }
        pending.push_back(std::get<TaskClaim>(std::move(existing)));
      }
    }
  }
  auto claimed = claim_due(now(), 32 - pending.size());
  if (const auto* failure = std::get_if<wire::Error>(&claimed)) {
    return *failure;
  }
  for (auto& next : std::get<std::vector<TaskClaim>>(claimed)) {
    pending.push_back(std::move(next));
  }
  for (const auto& item : pending) {
    auto result =
        finish(item.task.owner_plugin_id, item.task.request.task_id(), item.claim_token, true, {});
    if (const auto* failure = std::get_if<wire::Error>(&result)) {
      if (failure->code() == wire::RATE_LIMITED || failure->code() == wire::PLUGIN_UNAVAILABLE) {
        result = finish(item.task.owner_plugin_id, item.task.request.task_id(), item.claim_token,
                        false, now() + std::chrono::seconds(1));
      }
      if (std::holds_alternative<wire::Error>(result)) {
        return result;
      }
    }
  }
  return std::monostate{};
} catch (...) {
  return error(wire::INTERNAL);
}
} // namespace kiarabot::core
