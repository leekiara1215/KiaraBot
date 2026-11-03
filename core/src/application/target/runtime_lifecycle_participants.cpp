#include "kiarabot/core/application/target/runtime_lifecycle_participants.hpp"

#include <chrono>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "kiarabot/core/domain/target/host_command_records.hpp"

namespace kiarabot::core::target {
namespace {
LifecycleParticipantResult blocked(ErrorCode code, TimePoint now) {
  return {LifecycleParticipantDisposition::Blocked, code,
          now + std::chrono::duration_cast<TimePoint::duration>(std::chrono::seconds{5})};
}
LifecycleParticipantResult done() {
  return {LifecycleParticipantDisposition::Done, std::nullopt, std::nullopt};
}
class RuntimeHostLifecycleParticipant final : public HostLifecycleParticipantPort {
public:
  RuntimeHostLifecycleParticipant(RuntimeUnitOfWorkFactory& transactions,
                                  RuntimeSecretPort& secrets)
      : transactions_(transactions), secrets_(secrets) {}

  LifecycleParticipantResult stop_instance(const ResourceRef& resource) override {
    if (resource.kind != ResourceKind::Instance || resource.id.value.empty()) {
      return {LifecycleParticipantDisposition::Blocked, ErrorCode::InvalidArgument, std::nullopt};
    }
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return {LifecycleParticipantDisposition::Blocked, failure->code, std::nullopt};
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return {LifecycleParticipantDisposition::Blocked, ErrorCode::Internal, std::nullopt};
      }
      auto now_result = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&now_result)) {
        work->rollback();
        return {LifecycleParticipantDisposition::Blocked, failure->code, std::nullopt};
      }
      const auto now = std::get<TimePoint>(now_result);
      auto found = work->instances().find(InstanceId{resource.id.value});
      if (const auto* failure = std::get_if<Failure>(&found)) {
        work->rollback();
        return blocked(failure->code, now);
      }
      const auto instance = std::get<std::optional<PluginInstance>>(found);
      if (!instance) {
        work->rollback();
        return done();
      }
      if (!instance->host || instance->state == LifecycleState::Deleted ||
          instance->state == LifecycleState::Purged) {
        work->rollback();
        return done();
      }
      const auto generation = instance->current ? instance->current->execution_generation.value : 1;
      const std::string canonical = instance->id.value + "|" + instance->host->value + "|" +
                                    std::to_string(generation) + "|" +
                                    std::to_string(instance->desired_version.value) + "|" +
                                    std::to_string(instance->version.value) + "|" +
                                    std::to_string(static_cast<int>(instance->desired));
      auto digest = secrets_.sha256(canonical);
      if (const auto* failure = std::get_if<Failure>(&digest)) {
        work->rollback();
        return blocked(failure->code, now);
      }
      const std::string command_id = "hc-" + hex_digest(std::get<Sha256Digest>(digest));
      auto existing = work->host_commands().find(command_id);
      if (const auto* failure = std::get_if<Failure>(&existing)) {
        work->rollback();
        return blocked(failure->code, now);
      }
      if (const auto& command = std::get<std::optional<HostCommandOutboxRecord>>(existing)) {
        work->rollback();
        if (command->receipt_persisted.has_value()) {
          if (*command->receipt_persisted && command->receipt_outcome &&
              (*command->receipt_outcome == ExecutionOutcome::KnownSucceeded ||
               *command->receipt_outcome == ExecutionOutcome::KnownFailed)) {
            return done();
          }
          if (*command->receipt_persisted &&
              (!command->receipt_outcome ||
               *command->receipt_outcome == ExecutionOutcome::Unknown)) {
            // Persisted receipt without a known terminal process outcome: still UNKNOWN, never
            // "stopped"; retry/observation may resolve it later.
            return blocked(ErrorCode::UpstreamUnavailable, now);
          }
          // Host explicitly rejected the stop: keep the lifecycle operation blocked for
          // operator/retry handling and never claim completion.
          return blocked(ErrorCode::Conflict, now);
        }
        if (command->state == HostCommandDeliveryState::Unknown) {
          return blocked(ErrorCode::UpstreamUnavailable, now);
        }
        return blocked(ErrorCode::UpstreamUnavailable, now);
      }
      auto installation = work->installations().find(instance->installation);
      if (const auto* failure = std::get_if<Failure>(&installation)) {
        work->rollback();
        return blocked(failure->code, now);
      }
      const auto stored_installation = std::get<std::optional<PluginInstallation>>(installation);
      if (!stored_installation) {
        work->rollback();
        return blocked(ErrorCode::NotFound, now);
      }
      auto package_digest = secrets_.sha256(
          std::string_view(reinterpret_cast<const char*>(stored_installation->manifest.data()),
                           stored_installation->manifest.size()));
      if (const auto* failure = std::get_if<Failure>(&package_digest)) {
        work->rollback();
        return blocked(failure->code, now);
      }
      // Full Delete participant snapshot: the same installation/package fields as the apply
      // snapshot, with the desired state fixed to stopped. launch_profile/arguments/environment
      // stay deployment facts (Runtime does not parse the manifest); the StopExecution frame only
      // needs host/instance/generation, so an empty profile is honest rather than invented.
      HostCommandOutboxRecord command;
      command.command_id = command_id;
      command.host_id = instance->host->value;
      command.instance_id = instance->id.value;
      command.installation_id = instance->installation.value;
      command.kind = HostCommandKind::StopExecution;
      command.execution_generation = generation == 0 ? 1 : generation;
      command.desired_version = instance->desired_version.value;
      command.desired_state = DesiredState::Stopped;
      command.package_sha256 = hex_digest(std::get<Sha256Digest>(package_digest));
      command.launch_profile = "";
      command.arguments = {};
      command.environment_names = {};
      command.payload_sha256 = hex_digest(std::get<Sha256Digest>(digest));
      command.state = HostCommandDeliveryState::Pending;
      command.created_at = now;
      command.updated_at = now;
      auto inserted = work->host_commands().insert(command);
      if (const auto* failure = std::get_if<Failure>(&inserted)) {
        work->rollback();
        return blocked(failure->code, now);
      }
      if (work->commit() != CommitResult::Committed) {
        return blocked(ErrorCode::UpstreamUnavailable, now);
      }
      // Dispatched but no receipt yet: the Host may not have observed the process stop.
      return blocked(ErrorCode::UpstreamUnavailable, now);
    } catch (const std::bad_alloc&) {
      return {LifecycleParticipantDisposition::Blocked, ErrorCode::UpstreamUnavailable,
              std::nullopt};
    } catch (...) {
      return {LifecycleParticipantDisposition::Blocked, ErrorCode::Internal, std::nullopt};
    }
  }

private:
  static std::string hex_digest(const Sha256Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.bytes.size() * 2);
    for (const std::byte byte : digest.bytes) {
      const auto value = static_cast<unsigned char>(byte);
      text.push_back(kHex[value >> 4U]);
      text.push_back(kHex[value & 0x0fU]);
    }
    return text;
  }
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeSecretPort& secrets_;
};
} // namespace

Outcome<std::unique_ptr<HostLifecycleParticipantPort>>
make_runtime_host_lifecycle_participant(RuntimeUnitOfWorkFactory& transactions,
                                        RuntimeSecretPort& secrets) {
  try {
    return std::unique_ptr<HostLifecycleParticipantPort>(
        std::make_unique<RuntimeHostLifecycleParticipant>(transactions, secrets));
  } catch (const std::bad_alloc&) {
    return Failure{ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, Recovery::Backoff,
                   std::nullopt, std::nullopt};
  } catch (...) {
    return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, std::nullopt,
                   std::nullopt};
  }
}

namespace {
LifecycleParticipantResult scheduler_blocked(ErrorCode code) {
  return {LifecycleParticipantDisposition::Blocked, code, std::nullopt};
}
LifecycleParticipantResult scheduler_done() {
  return {LifecycleParticipantDisposition::Done, std::nullopt, std::nullopt};
}
class RuntimeSchedulerLifecycleParticipant final : public SchedulerLifecycleParticipantPort {
public:
  RuntimeSchedulerLifecycleParticipant(SchedulerTaskPort& scheduler_tasks, RuntimeIdPort& ids)
      : scheduler_tasks_(scheduler_tasks), ids_(ids) {}

  LifecycleParticipantResult cancel_tasks(const ResourceRef& resource) override {
    if (resource.kind == ResourceKind::Installation) {
      // Approved scope A is instance-scoped ListTasks/CancelTask only. Enumerating an
      // installation would cross instances, so keep the operation Blocked instead of faking a
      // complete cancellation or issuing a forbidden cross-instance query.
      return scheduler_blocked(ErrorCode::CapabilityNotSupported);
    }
    if (resource.kind != ResourceKind::Instance || resource.id.value.empty()) {
      return scheduler_blocked(ErrorCode::InvalidArgument);
    }
    const InstanceId owner{resource.id.value};
    const auto request = ids_.operation_id();
    if (const auto* failure = std::get_if<Failure>(&request)) {
      return scheduler_blocked(failure->code);
    }
    const auto trace = ids_.operation_id();
    if (const auto* failure = std::get_if<Failure>(&trace)) {
      return scheduler_blocked(failure->code);
    }
    const auto idempotency = ids_.operation_id();
    if (const auto* failure = std::get_if<Failure>(&idempotency)) {
      return scheduler_blocked(failure->code);
    }
    const auto now = TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
    SchedulerTaskContext context;
    context.owner = owner;
    context.request_id = std::get<OperationId>(request).value;
    context.trace_id = std::get<OperationId>(trace).value;
    context.idempotency_key = std::get<OperationId>(idempotency).value;
    context.deadline = now + std::chrono::seconds{2};
    auto listed = scheduler_tasks_.list_tasks(context, TaskState::Pending, 100);
    if (const auto* failure = std::get_if<Failure>(&listed)) {
      return scheduler_blocked(failure->code);
    }
    for (const auto& task : std::get<std::vector<ScheduledTask>>(listed)) {
      if (task.key.owner != owner || task.state != TaskState::Pending) {
        // Defence in depth: the Scheduler side filters by instance scope, but Runtime never
        // cancels a task that does not belong to this exact instance.
        continue;
      }
      auto cancel_context = context;
      const auto cancel_key = ids_.operation_id();
      if (const auto* failure = std::get_if<Failure>(&cancel_key)) {
        return scheduler_blocked(failure->code);
      }
      cancel_context.idempotency_key = std::get<OperationId>(cancel_key).value;
      auto cancelled = scheduler_tasks_.cancel_task(cancel_context, task.key.task_id);
      if (const auto* failure = std::get_if<Failure>(&cancelled)) {
        // Conflict/NotFound can mean the task started or another actor changed it; keep the
        // operation Blocked so the next maintenance round re-lists instead of claiming success.
        return scheduler_blocked(failure->code);
      }
    }
    return scheduler_done();
  }

private:
  SchedulerTaskPort& scheduler_tasks_;
  RuntimeIdPort& ids_;
};
} // namespace

Outcome<std::unique_ptr<SchedulerLifecycleParticipantPort>>
make_runtime_scheduler_lifecycle_participant(SchedulerTaskPort& scheduler_tasks,
                                             RuntimeIdPort& ids) {
  try {
    return std::unique_ptr<SchedulerLifecycleParticipantPort>(
        std::make_unique<RuntimeSchedulerLifecycleParticipant>(scheduler_tasks, ids));
  } catch (const std::bad_alloc&) {
    return Failure{ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, Recovery::Backoff,
                   std::nullopt, std::nullopt};
  } catch (...) {
    return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, std::nullopt,
                   std::nullopt};
  }
}

} // namespace kiarabot::core::target
