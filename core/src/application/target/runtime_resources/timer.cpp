#include <algorithm>
#include <set>

#include "common.hpp"
#include "components.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/host_command_records.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
namespace {
class RuntimeTimerApplicationImpl final : public RuntimeTimerApplication {
public:
  RuntimeTimerApplicationImpl(const RuntimeResourceDependencies& dependencies)
      : transactions_(dependencies.transactions), secrets_(dependencies.secrets),
        authorization_(dependencies.authorization) {}

  Outcome<TimerTriggerAcceptance>
  accept_timer_trigger(const AcceptTimerTriggerRequest& request) override {
    if (request.platform_peer_authorized) {
      // Platform service peer (Scheduler) verified by the mTLS boundary. The frozen design
      // authorizes service peer + handoff integrity, not a delegated user; only request identity
      // fields are required here.
      if (!canonical(request.context.request_id) || !canonical(request.context.trace_id) ||
          request.context.method != "kiarabot.v1.RuntimeService/AcceptTimerTrigger" ||
          request.context.deadline.time_since_epoch().count() <= 0) {
        return invalid_runtime_input();
      }
    } else if (const auto failure =
                   status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    const auto safe_text = [](std::string_view value, std::size_t maximum) {
      return !value.empty() && value.size() <= maximum &&
             value.find('\0') == std::string_view::npos;
    };
    if (!canonical(request.occurrence_id.value) || !canonical(request.owner_instance_id.value) ||
        !safe_text(request.task_id, 128) || request.schedule_version.value == 0 ||
        request.conversation_id.empty() || request.conversation_id.size() > 128 ||
        request.conversation_id.find('\0') != std::string::npos ||
        request.conversation_kind == ConversationKey::Kind::Unspecified ||
        request.payload.size() > 65536 || request.serialized_envelope.empty() ||
        request.serialized_envelope.size() > 1024 * 1024 ||
        request.handoff.source != Service::Scheduler ||
        request.handoff.message != request.occurrence_id ||
        request.handoff.operation.owner != Service::Scheduler ||
        request.handoff.operation.resource.kind != ResourceKind::Task ||
        request.handoff.operation.resource.id.value != request.task_id ||
        request.handoff.resource.kind != ResourceKind::Task ||
        request.handoff.resource.id.value != request.task_id ||
        request.handoff.version != request.schedule_version) {
      return invalid_runtime_input();
    }
    const auto payload_digest = secrets_.sha256(std::string_view(
        reinterpret_cast<const char*>(request.payload.data()), request.payload.size()));
    if (const auto* failed = std::get_if<Failure>(&payload_digest)) {
      return *failed;
    }
    if (std::get<Sha256Digest>(payload_digest) != request.handoff.payload_hash) {
      return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
    }
    if (!request.platform_peer_authorized) {
      auto checked =
          authorize(authorization_, request.context.authentication, Permission::TimerAccept,
                    request.context.method, {ResourceRef{ResourceKind::Task, {request.task_id}}},
                    request.context.deadline);
      if (const auto* failure = std::get_if<Failure>(&checked)) {
        return *failure;
      }
    }
    auto begun = transactions_.begin();
    if (const auto* failure = std::get_if<Failure>(&begun)) {
      return *failure;
    }
    auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    auto sample = work->current_time();
    if (const auto* failure = std::get_if<Failure>(&sample)) {
      work->rollback();
      return *failure;
    }
    const auto now = std::get<TimePoint>(sample);
    if (request.handoff.valid_until <= now) {
      work->rollback();
      return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
    }
    std::string canonical;
    canonical.reserve(request.payload.size() + request.serialized_envelope.size() + 512);
    const auto append = [&canonical](std::string_view value) {
      canonical.append(std::to_string(value.size()));
      canonical.push_back(':');
      canonical.append(value);
    };
    append(request.occurrence_id.value);
    append(request.task_id);
    append(request.owner_instance_id.value);
    append(std::to_string(request.schedule_version.value));
    append(request.conversation_id);
    append(std::to_string(static_cast<int>(request.conversation_kind)));
    append(std::string_view(reinterpret_cast<const char*>(request.payload.data()),
                            request.payload.size()));
    // The serialized envelope carries boundary-generated trace/time fields; it must not change the
    // handoff idempotency fingerprint. Semantic content is covered above. A changed envelope for
    // the same occurrence is therefore still a duplicate, while changed task/payload/version
    // conflicts.
    const auto fingerprint = secrets_.sha256(canonical);
    if (const auto* failed = std::get_if<Failure>(&fingerprint)) {
      work->rollback();
      return *failed;
    }
    EventRecord event;
    event.key = {ResourceId{std::string{"scheduler"}}, request.occurrence_id.value};
    event.fingerprint = std::get<Sha256Digest>(fingerprint);
    event.payload = EventPayloadKind::TimerTriggered;
    event.has_conversation = false;
    event.serialized_envelope = request.serialized_envelope;
    event.occurred_at = now;
    event.retain_until = now + std::chrono::hours{24 * 7};
    DeliveryRecord target;
    target.delivery = {event.key, request.owner_instance_id};
    target.ordered = false;
    target.trace_id = request.context.trace_id;
    auto accepted = work->events().accept(event, {target}, EventCapacityLimits{});
    if (const auto* failure = std::get_if<Failure>(&accepted)) {
      work->rollback();
      return *failure;
    }
    const bool duplicate = std::get<EventAcceptanceResult>(accepted).duplicate;
    if (duplicate) {
      work->rollback();
      return TimerTriggerAcceptance{request.occurrence_id, request.occurrence_id, true, now};
    }
    if (work->commit() != CommitResult::Committed) {
      return unknown_result();
    }
    return TimerTriggerAcceptance{request.occurrence_id, request.occurrence_id, false, now};
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeSecretPort& secrets_;
  RuntimeAuthorizationPort& authorization_;
};
} // namespace

std::unique_ptr<RuntimeTimerApplication>
make_timer(const RuntimeResourceDependencies& dependencies) {
  return std::make_unique<RuntimeTimerApplicationImpl>(dependencies);
}

} // namespace kiarabot::core::target::runtime_resources_detail
