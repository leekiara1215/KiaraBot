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

} // namespace kiarabot::core::target
