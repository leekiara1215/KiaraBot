#include "kiarabot/core/application/target/runtime_task_application.hpp"

#include <algorithm>

namespace kiarabot::core::target {
namespace {
Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
bool valid_task_id(std::string_view value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-';
  });
}
bool valid_context(const SchedulerTaskContext& context) {
  return !context.owner.value.empty() && !context.installation.value.empty() &&
         !context.request_id.empty() && !context.trace_id.empty() &&
         context.idempotency_key.size() <= 256;
}
} // namespace

RuntimeTaskApplication::RuntimeTaskApplication(RuntimeTaskApplicationDependencies dependencies)
    : dependencies_(dependencies) {}

Outcome<SchedulerTaskPort*> RuntimeTaskApplication::port(const SchedulerTaskContext& context) {
  if (!valid_context(context)) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (dependencies_.scheduler == nullptr || !dependencies_.scheduler->available()) {
    return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
  }
  return dependencies_.scheduler;
}

Outcome<ScheduledTask> RuntimeTaskApplication::create_task(const SchedulerTaskContext& context,
                                                           TaskCreationInput input) {
  auto selected = port(context);
  if (const auto* rejected = std::get_if<Failure>(&selected)) {
    return *rejected;
  }
  if (context.idempotency_key.empty() || !valid_task_id(input.task_id) ||
      input.target.conversation_id.empty() || input.target.conversation_id.size() > 256 ||
      input.payload.size() > dependencies_.maximum_payload_bytes ||
      (!input.run_at.has_value() && !input.local_time.has_value())) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::get<SchedulerTaskPort*>(selected)->create_task(context, input);
}

Outcome<ScheduledTask> RuntimeTaskApplication::get_task(const SchedulerTaskContext& context,
                                                        std::string_view task_id) {
  auto selected = port(context);
  if (const auto* rejected = std::get_if<Failure>(&selected)) {
    return *rejected;
  }
  if (!valid_task_id(task_id)) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::get<SchedulerTaskPort*>(selected)->get_task(context, task_id);
}

Outcome<ScheduledTask> RuntimeTaskApplication::cancel_task(const SchedulerTaskContext& context,
                                                           std::string_view task_id) {
  auto selected = port(context);
  if (const auto* rejected = std::get_if<Failure>(&selected)) {
    return *rejected;
  }
  if (!valid_task_id(task_id)) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::get<SchedulerTaskPort*>(selected)->cancel_task(context, task_id);
}

} // namespace kiarabot::core::target
