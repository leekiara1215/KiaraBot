#include "kiarabot/core/runtime/platform/scheduler_rpc.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include "kiarabot/core/runtime/platform/operation_observation_rpc.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/common.pb.h"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace codec = target_codec;
namespace t = target;
namespace v1 = kiarabot::v1;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure invalid() {
  return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
t::Failure internal_failure() {
  return {t::ErrorCode::Internal, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity,
          std::nullopt, std::nullopt};
}

std::string service_text(t::Service value) {
  switch (value) {
  case t::Service::Identity:
    return "Identity";
  case t::Service::Runtime:
    return "Runtime";
  case t::Service::Host:
    return "Host";
  case t::Service::Scheduler:
    return "Scheduler";
  case t::Service::Model:
    return "Model";
  case t::Service::Control:
    return "Control";
  }
  return "Scheduler";
}
bool canonical_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
std::string random_uuid() {
  static std::mutex mutex;
  static std::mt19937_64 generator{std::random_device{}()};
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  {
    std::lock_guard lock(mutex);
    high = generator();
    low = generator();
  }
  high = (high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
  low = (low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text(36, '-');
  const std::uint64_t parts[2] = {high, low};
  std::size_t output = 0;
  for (int index = 15; index >= 0; --index) {
    const auto byte = static_cast<unsigned char>((parts[index / 8] >> ((index % 8) * 8)) & 0xff);
    if (output == 8 || output == 13 || output == 18 || output == 23) {
      ++output;
    }
    text[output++] = kHex[byte >> 4];
    text[output++] = kHex[byte & 0x0f];
  }
  return text;
}
bool safe_key(std::string_view value) {
  return value.size() >= 16 && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return character >= 33 && character < 127; });
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
grpc::Status error_status(const t::Failure& input, const std::string& request,
                          const std::string& trace) {
  v1::RpcFailure result;
  const bool retryable =
      input.recovery == t::Recovery::Backoff || input.recovery == t::Recovery::QuerySameIdentity;
  // Early validation failures may not carry a usable request/trace id; a stable RpcFailure still
  // needs syntactically valid identifiers, never free-form/internal fallback.
  const std::string safe_request = canonical_uuid(request) ? request : random_uuid();
  const std::string safe_trace = canonical_uuid(trace) ? trace : random_uuid();
  const auto error = codec::make_error(input.code, safe_request, safe_trace, retryable);
  if (const auto* stable = std::get_if<v1::Error>(&error)) {
    *result.mutable_error() = *stable;
  }
  auto* context = result.mutable_context();
  switch (input.acceptance) {
  case t::Acceptance::NotAccepted:
    context->set_acceptance(v1::NOT_ACCEPTED);
    break;
  case t::Acceptance::Accepted:
    context->set_acceptance(v1::DURABLY_ACCEPTED);
    break;
  case t::Acceptance::Unknown:
    context->set_acceptance(v1::ACCEPTANCE_UNKNOWN);
    break;
  }
  switch (input.recovery) {
  case t::Recovery::CorrectInput:
    context->set_recovery(v1::CORRECT_INPUT);
    break;
  case t::Recovery::Authenticate:
    context->set_recovery(v1::REAUTHENTICATE);
    break;
  case t::Recovery::QuerySameIdentity:
    context->set_recovery(v1::QUERY_SAME_INTENT);
    break;
  case t::Recovery::Backoff:
    context->set_recovery(v1::RETRY_SAME_ID);
    break;
  case t::Recovery::ReadCurrent:
    context->set_recovery(v1::RESYNC_SNAPSHOT);
    break;
  case t::Recovery::Diagnose:
    context->set_recovery(v1::MANUAL_REVIEW);
    break;
  }
  if (input.retry_after && input.retry_after->count() > 0) {
    context->set_retry_after_ms(
        static_cast<std::uint32_t>(std::min<std::int64_t>(input.retry_after->count(), UINT32_MAX)));
  }
  if (input.operation) {
    auto* operation = context->mutable_operation();
    operation->set_owner_service("Scheduler");
    operation->set_operation_id(input.operation->id.value);
    const auto resource = codec::to_wire(input.operation->resource);
    if (const auto* wire = std::get_if<v1::ResourceRef>(&resource)) {
      *operation->mutable_resource() = *wire;
    }
  }
  const auto status = codec::to_grpc_failure(result);
  if (const auto* mapped = std::get_if<grpc::Status>(&status)) {
    return *mapped;
  }
  return {grpc::StatusCode::INTERNAL, "INTERNAL"};
}

v1::ScheduledTaskState task_state(t::TaskState value) {
  switch (value) {
  case t::TaskState::Pending:
    return v1::SCHEDULED_TASK_PENDING;
  case t::TaskState::Running:
    return v1::SCHEDULED_TASK_RUNNING;
  case t::TaskState::Retrying:
    return v1::SCHEDULED_TASK_RETRYING;
  case t::TaskState::Completed:
    return v1::SCHEDULED_TASK_COMPLETED;
  case t::TaskState::Failed:
    return v1::SCHEDULED_TASK_FAILED;
  case t::TaskState::Cancelled:
    return v1::SCHEDULED_TASK_CANCELLED;
  }
  return v1::SCHEDULED_TASK_STATE_UNSPECIFIED;
}
std::optional<t::TaskState> task_state_from_wire(v1::ScheduledTaskState value) {
  switch (value) {
  case v1::SCHEDULED_TASK_PENDING:
    return t::TaskState::Pending;
  case v1::SCHEDULED_TASK_RUNNING:
    return t::TaskState::Running;
  case v1::SCHEDULED_TASK_RETRYING:
    return t::TaskState::Retrying;
  case v1::SCHEDULED_TASK_COMPLETED:
    return t::TaskState::Completed;
  case v1::SCHEDULED_TASK_FAILED:
    return t::TaskState::Failed;
  case v1::SCHEDULED_TASK_CANCELLED:
    return t::TaskState::Cancelled;
  default:
    return std::nullopt;
  }
}
v1::OperationState operation_state(t::OperationState value) {
  switch (value) {
  case t::OperationState::Accepted:
    return v1::OPERATION_ACCEPTED;
  case t::OperationState::Running:
    return v1::OPERATION_RUNNING;
  case t::OperationState::Blocked:
    return v1::OPERATION_BLOCKED;
  case t::OperationState::Succeeded:
    return v1::OPERATION_SUCCEEDED;
  case t::OperationState::Failed:
    return v1::OPERATION_FAILED;
  case t::OperationState::Cancelled:
    return v1::OPERATION_CANCELLED;
  case t::OperationState::Expired:
    return v1::OPERATION_EXPIRED;
  }
  return v1::OPERATION_STATE_UNSPECIFIED;
}
v1::ExecutionOutcome execution_outcome(t::ExecutionOutcome value) {
  switch (value) {
  case t::ExecutionOutcome::NotStarted:
    return v1::EXECUTION_NOT_STARTED;
  case t::ExecutionOutcome::KnownSucceeded:
    return v1::EXECUTION_KNOWN_SUCCEEDED;
  case t::ExecutionOutcome::KnownFailed:
    return v1::EXECUTION_KNOWN_FAILED;
  case t::ExecutionOutcome::Unknown:
    return v1::EXECUTION_UNKNOWN;
  }
  return v1::EXECUTION_OUTCOME_UNSPECIFIED;
}
v1::ConversationRef::Type conversation_type(t::ConversationKey::Kind value) {
  switch (value) {
  case t::ConversationKey::Kind::Private:
    return v1::ConversationRef::PRIVATE;
  case t::ConversationKey::Kind::Group:
    return v1::ConversationRef::GROUP;
  default:
    return v1::ConversationRef::TYPE_UNSPECIFIED;
  }
}
std::optional<t::ConversationKey::Kind>
conversation_type_from_wire(v1::ConversationRef::Type value) {
  switch (value) {
  case v1::ConversationRef::PRIVATE:
    return t::ConversationKey::Kind::Private;
  case v1::ConversationRef::GROUP:
    return t::ConversationKey::Kind::Group;
  default:
    return std::nullopt;
  }
}
bool conversation(const t::ConversationKey& value, v1::ConversationRef* output) {
  if (value.conversation_id.empty()) {
    return false;
  }
  output->set_id(value.conversation_id);
  output->set_type(conversation_type(value.kind));
  return true;
}
bool error_code(const std::optional<t::ErrorCode>& value, v1::Error* output,
                const std::string& request, const std::string& trace) {
  if (!value) {
    return true;
  }
  const auto encoded = codec::to_wire(*value);
  const auto* wire = std::get_if<v1::ErrorCode>(&encoded);
  if (!wire) {
    return false;
  }
  output->set_code(*wire);
  output->set_request_id(request);
  output->set_trace_id(trace);
  output->set_message(service_text(t::Service::Scheduler));
  return true;
}
bool handoff(const t::HandoffContext& value, v1::HandoffContext* output) {
  output->set_source_service(service_text(value.source));
  output->set_message_id(value.message.value);
  auto* operation = output->mutable_operation();
  operation->set_owner_service(service_text(value.operation.owner));
  operation->set_operation_id(value.operation.id.value);
  const auto resource = codec::to_wire(value.operation.resource);
  if (const auto* wire = std::get_if<v1::ResourceRef>(&resource)) {
    *operation->mutable_resource() = *wire;
  }
  if (value.causation) {
    output->set_causation_id(value.causation->value);
  }
  const auto handoff_resource = codec::to_wire(value.resource);
  if (const auto* wire = std::get_if<v1::ResourceRef>(&handoff_resource)) {
    *output->mutable_resource() = *wire;
  }
  output->set_resource_version(value.version.value);
  output->set_payload_sha256(
      std::string(reinterpret_cast<const char*>(value.payload_hash.bytes.data()),
                  value.payload_hash.bytes.size()));
  if (!timestamp(value.created_at, output->mutable_created_at()) ||
      !timestamp(value.valid_until, output->mutable_valid_until())) {
    return false;
  }
  return true;
}
bool resource(const t::ResourceRef& value, v1::ResourceRef* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<v1::ResourceRef>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
bool operation(const t::Operation& value, v1::Operation* output) {
  auto* ref = output->mutable_ref();
  ref->set_owner_service(service_text(value.ref.owner));
  ref->set_operation_id(value.ref.id.value);
  if (!resource(value.ref.resource, ref->mutable_resource())) {
    return false;
  }
  output->set_version(value.version.value);
  output->set_state(operation_state(value.state));
  output->set_execution_outcome(execution_outcome(value.execution_outcome));
  if (value.blocked_reason) {
    auto* reason = output->mutable_reason();
    if (!error_code(value.blocked_reason, reason, "", "")) {
      return false;
    }
  }
  if (value.next_attempt && !timestamp(*value.next_attempt, output->mutable_next_attempt_at())) {
    return false;
  }
  output->set_stalled(false);
  return timestamp(now(), output->mutable_observed_at());
}
bool task(const t::ScheduledTask& value, v1::TaskRecord* output, const std::string& request,
          const std::string& trace) {
  output->set_task_id(value.key.task_id);
  output->set_owner_instance_id(value.key.owner.value);
  output->set_installation_id(value.installation.value);
  if (!conversation(value.target, output->mutable_target())) {
    return false;
  }
  output->set_payload(
      std::string(reinterpret_cast<const char*>(value.payload.data()), value.payload.size()));
  if (!timestamp(value.run_at, output->mutable_run_at()) ||
      !timestamp(value.created_at, output->mutable_created_at()) ||
      !timestamp(value.updated_at, output->mutable_updated_at())) {
    return false;
  }
  output->set_state(task_state(value.state));
  output->set_version(value.version.value);
  output->set_occurrence_id(value.occurrence.value);
  output->set_cancel_requested(value.cancel_requested);
  return error_code(value.reason, output->mutable_reason(), request, trace);
}
bool occurrence(const t::Occurrence& value, v1::OccurrenceRecord* output,
                const std::string& request, const std::string& trace) {
  output->set_occurrence_id(value.occurrence_id.value);
  output->set_task_id(value.task_id);
  output->set_state(operation_state(value.state));
  if (!handoff(value.handoff, output->mutable_handoff())) {
    return false;
  }
  if (value.accepted_event_id) {
    output->set_accepted_event_id(*value.accepted_event_id);
  }
  if (value.acknowledged_at &&
      !timestamp(*value.acknowledged_at, output->mutable_acknowledged_at())) {
    return false;
  }
  output->set_attempts(value.attempts);
  if (!error_code(value.last_error, output->mutable_last_error(), request, trace)) {
    return false;
  }
  if (!timestamp(value.created_at, output->mutable_created_at()) ||
      !timestamp(value.updated_at, output->mutable_updated_at())) {
    return false;
  }
  return true;
}

struct Prepared {
  t::SchedulerCommandContext command;
  std::string request;
  std::string trace;
};
t::Outcome<Prepared> prepare(grpc::ServerContext& transport, const v1::RequestContext& input,
                             std::string_view method, const v1::ResourceScope* scope,
                             bool idempotent, const SchedulerRpcOptions& options) {
  if (!canonical_uuid(input.request_id()) || !canonical_uuid(input.trace_id()) ||
      !input.has_deadline()) {
    return invalid();
  }
  const auto decoded = codec::from_wire(input.deadline());
  const auto* requested = std::get_if<t::TimePoint>(&decoded);
  if (!requested) {
    return invalid();
  }
  const auto started = now();
  auto effective = std::min(*requested, started + std::chrono::seconds{30});
  const auto transport_deadline = transport.deadline();
  if (transport_deadline == std::chrono::system_clock::time_point::max()) {
    effective = std::min(effective, started + std::chrono::seconds{2});
  } else if (transport_deadline <= std::chrono::system_clock::now()) {
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
  } else if (transport_deadline < std::chrono::system_clock::now() + std::chrono::seconds{30}) {
    effective =
        std::min(effective, t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                transport_deadline.time_since_epoch())});
  }
  if (options.invocation_deadline) {
    auto own = options.invocation_deadline();
    if (const auto* rejected = std::get_if<t::Failure>(&own)) {
      return *rejected;
    }
    effective = std::min(effective, std::get<t::TimePoint>(own));
  }
  if (effective <= now() || transport.IsCancelled()) {
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
  }
  if (idempotent && !safe_key(input.idempotency_key())) {
    return invalid();
  }
  if (!options.authorize) {
    return unauthenticated();
  }
  auto authenticated = options.authorize(transport, input, method, scope);
  if (const auto* rejected = std::get_if<t::Failure>(&authenticated)) {
    return *rejected;
  }
  auto command = std::move(std::get<t::SchedulerCommandContext>(authenticated));
  if (!canonical_uuid(command.owner.value) || !canonical_uuid(command.installation.value)) {
    return unauthenticated();
  }
  if (idempotent) {
    command.idempotency_key = input.idempotency_key();
  }
  Prepared prepared;
  prepared.request = input.request_id();
  prepared.trace = input.trace_id();
  command.request_id = prepared.request;
  command.trace_id = prepared.trace;
  command.deadline = effective;
  prepared.command = std::move(command);
  return prepared;
}

t::Outcome<std::optional<Prepared>>
prepare_platform_peer(grpc::ServerContext& transport, const v1::RequestContext& input,
                      std::string_view method, const v1::ResourceScope* scope, bool idempotent,
                      const SchedulerRpcOptions& options) {
  if (!options.platform_peer) {
    return std::optional<Prepared>{};
  }
  // P05b-5: the platform-peer allowlist is reserved for the delegation-free lifecycle path
  // (P13h). A request carrying a user/instance delegation must be authorized through the normal
  // delegation path instead; otherwise CancelTask/ListTasks plugin calls would be silently
  // reattributed to the deployment allowlist entry and lose the delegation subject/scope.
  if (!input.delegation().empty()) {
    return std::optional<Prepared>{};
  }
  auto peer = options.platform_peer(transport, method, scope);
  if (const auto* rejected = std::get_if<t::Failure>(&peer)) {
    return *rejected;
  }
  auto selected = std::get<std::optional<SchedulerPlatformPeer>>(peer);
  if (!selected) {
    return std::optional<Prepared>{};
  }
  if (!canonical_uuid(input.request_id()) || !canonical_uuid(input.trace_id()) ||
      !input.has_deadline()) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const auto decoded = codec::from_wire(input.deadline());
  const auto* requested = std::get_if<t::TimePoint>(&decoded);
  if (!requested) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  auto effective = std::min(*requested, now() + std::chrono::seconds{30});
  if (effective <= now() || transport.IsCancelled()) {
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
  }
  if (idempotent && !safe_key(input.idempotency_key())) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const bool list = method.ends_with("/ListTasks");
  const bool cancel = method.ends_with("/CancelTask");
  if (!list && !cancel) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  if ((list && !selected->task_read) || (cancel && !selected->task_schedule)) {
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
  }
  if (scope == nullptr || scope->resources_size() != 1 ||
      scope->resources(0).kind() != v1::RESOURCE_INSTANCE ||
      scope->resources(0).id() != selected->instance.value) {
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
  }
  Prepared prepared;
  prepared.request = input.request_id();
  prepared.trace = input.trace_id();
  prepared.command.owner = selected->instance;
  prepared.command.installation = selected->installation;
  prepared.command.account = selected->account;
  prepared.command.platform_peer = true;
  prepared.command.request_id = prepared.request;
  prepared.command.trace_id = prepared.trace;
  prepared.command.deadline = effective;
  if (idempotent) {
    prepared.command.idempotency_key = input.idempotency_key();
  }
  return std::optional<Prepared>{std::move(prepared)};
}

class SchedulerRpc final : public v1::SchedulerService::Service {
public:
  SchedulerRpc(t::SchedulerCommandHandler& application, SchedulerRpcOptions options)
      : application_(application), options_(std::move(options)) {}

  grpc::Status CreateTask(grpc::ServerContext* context_,
                          const v1::SchedulerCreateTaskRequest* request,
                          v1::SchedulerCreateTaskResponse* response) override {
    const auto prepared =
        prepare(*context_, request->context(), "kiarabot.v1.SchedulerService/CreateTask", nullptr,
                true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    const auto& command = std::get<Prepared>(prepared);
    t::TaskCreationInput input;
    input.task_id = request->task_id();
    const auto kind = conversation_type_from_wire(request->target().type());
    if (!kind || request->target().id().empty()) {
      return error_status(invalid(), command.request, command.trace);
    }
    input.target = t::ConversationKey{{}, {}, request->target().id(), *kind};
    input.payload.reserve(request->payload().size());
    for (const unsigned char byte : request->payload()) {
      input.payload.push_back(static_cast<std::byte>(byte));
    }
    if (request->when_case() == v1::SchedulerCreateTaskRequest::kRunAt) {
      const auto decoded = codec::from_wire(request->run_at());
      if (const auto* rejected = std::get_if<t::Failure>(&decoded)) {
        return error_status(*rejected, command.request, command.trace);
      }
      input.run_at = std::get<t::TimePoint>(decoded);
    } else if (request->when_case() == v1::SchedulerCreateTaskRequest::kLocalTime) {
      const auto& local = request->local_time();
      input.local_time =
          t::LocalCivilTime{local.year(),   local.month(),  local.day(),      local.hour(),
                            local.minute(), local.second(), local.time_zone()};
    } else {
      return error_status(invalid(), command.request, command.trace);
    }
    auto created = application_.create_task(command.command, input);
    if (const auto* rejected = std::get_if<t::Failure>(&created)) {
      return error_status(*rejected, command.request, command.trace);
    }
    if (!task(std::get<t::ScheduledTask>(created), response->mutable_task(), command.request,
              command.trace)) {
      return error_status(internal_failure(), command.request, command.trace);
    }
    return grpc::Status::OK;
  }

  grpc::Status CancelTask(grpc::ServerContext* context_,
                          const v1::SchedulerCancelTaskRequest* request,
                          v1::SchedulerCancelTaskResponse* response) override {
    auto platform = prepare_platform_peer(*context_, request->context(),
                                          "kiarabot.v1.SchedulerService/CancelTask",
                                          &request->scope(), true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&platform)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    std::optional<Prepared> selected = std::get<std::optional<Prepared>>(std::move(platform));
    if (!selected) {
      const auto prepared =
          prepare(*context_, request->context(), "kiarabot.v1.SchedulerService/CancelTask",
                  &request->scope(), true, options_);
      if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
        return error_status(*rejected, request->context().request_id(),
                            request->context().trace_id());
      }
      selected = std::get<Prepared>(prepared);
    }
    const auto& command = *selected;
    auto cancelled = application_.cancel_task(command.command, request->task_id());
    if (const auto* rejected = std::get_if<t::Failure>(&cancelled)) {
      return error_status(*rejected, command.request, command.trace);
    }
    if (!task(std::get<t::ScheduledTask>(cancelled), response->mutable_task(), command.request,
              command.trace)) {
      return error_status(internal_failure(), command.request, command.trace);
    }
    return grpc::Status::OK;
  }

  grpc::Status GetTask(grpc::ServerContext* context_, const v1::SchedulerGetTaskRequest* request,
                       v1::SchedulerGetTaskResponse* response) override {
    const auto prepared =
        prepare(*context_, request->context(), "kiarabot.v1.SchedulerService/GetTask",
                &request->scope(), false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    const auto& command = std::get<Prepared>(prepared);
    auto found = application_.get_task(command.command, request->task_id());
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return error_status(*rejected, command.request, command.trace);
    }
    if (!task(std::get<t::ScheduledTask>(found), response->mutable_task(), command.request,
              command.trace)) {
      return error_status(internal_failure(), command.request, command.trace);
    }
    return grpc::Status::OK;
  }

  grpc::Status ListTasks(grpc::ServerContext* context_,
                         const v1::SchedulerListTasksRequest* request,
                         v1::SchedulerListTasksResponse* response) override {
    auto platform = prepare_platform_peer(*context_, request->context(),
                                          "kiarabot.v1.SchedulerService/ListTasks",
                                          &request->scope(), false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&platform)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    std::optional<Prepared> selected = std::get<std::optional<Prepared>>(std::move(platform));
    if (!selected) {
      const auto prepared =
          prepare(*context_, request->context(), "kiarabot.v1.SchedulerService/ListTasks",
                  &request->scope(), false, options_);
      if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
        return error_status(*rejected, request->context().request_id(),
                            request->context().trace_id());
      }
      selected = std::get<Prepared>(prepared);
    }
    const auto& command = *selected;
    std::optional<t::TaskState> state;
    if (request->has_state()) {
      state = task_state_from_wire(request->state());
      if (!state) {
        return error_status(invalid(), command.request, command.trace);
      }
    }
    const auto page_size = request->page().page_size() == 0 ? 20U : request->page().page_size();
    if (page_size > 100) {
      return error_status(invalid(), command.request, command.trace);
    }
    auto listed =
        application_.list_tasks(command.command, state, page_size, request->page().page_token());
    if (const auto* rejected = std::get_if<t::Failure>(&listed)) {
      return error_status(*rejected, command.request, command.trace);
    }
    const auto& page = std::get<t::SchedulerListPage>(listed);
    for (const auto& value : page.tasks) {
      if (!task(value, response->add_tasks(), command.request, command.trace)) {
        return error_status(internal_failure(), command.request, command.trace);
      }
    }
    auto* output_page = response->mutable_page();
    output_page->set_page_size(page_size);
    output_page->set_next_page_token(page.next_page_token);
    return grpc::Status::OK;
  }

  grpc::Status GetOccurrence(grpc::ServerContext* context_,
                             const v1::SchedulerGetOccurrenceRequest* request,
                             v1::SchedulerGetOccurrenceResponse* response) override {
    const auto prepared =
        prepare(*context_, request->context(), "kiarabot.v1.SchedulerService/GetTask",
                &request->scope(), false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    const auto& command = std::get<Prepared>(prepared);
    auto found = application_.get_occurrence(command.command, request->occurrence_id());
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return error_status(*rejected, command.request, command.trace);
    }
    if (!occurrence(std::get<t::Occurrence>(found), response->mutable_occurrence(), command.request,
                    command.trace)) {
      return error_status(internal_failure(), command.request, command.trace);
    }
    return grpc::Status::OK;
  }

  grpc::Status GetOperation(grpc::ServerContext* context_,
                            const v1::SchedulerGetOperationRequest* request,
                            v1::SchedulerGetOperationResponse* response) override {
    const auto prepared =
        prepare(*context_, request->context(), "kiarabot.v1.SchedulerService/GetOperation", nullptr,
                false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    const auto& command = std::get<Prepared>(prepared);
    if (!request->has_operation() || request->operation().owner_service() != "Scheduler" ||
        request->operation().operation_id().empty() || !request->operation().has_resource()) {
      return error_status(invalid(), command.request, command.trace);
    }
    const auto resource = codec::from_wire(request->operation().resource());
    if (const auto* rejected = std::get_if<t::Failure>(&resource)) {
      return error_status(*rejected, command.request, command.trace);
    }
    t::OperationRef reference{t::Service::Scheduler,
                              t::OperationId{request->operation().operation_id()},
                              std::get<t::ResourceRef>(resource)};
    auto found = application_.get_operation(command.command, reference);
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return error_status(*rejected, command.request, command.trace);
    }
    if (!operation(std::get<t::Operation>(found), response->mutable_operation())) {
      return error_status(internal_failure(), command.request, command.trace);
    }
    return grpc::Status::OK;
  }

  grpc::Status ListOperationChanges(grpc::ServerContext* transport,
                                    const v1::OperationChangesRequest* request,
                                    v1::OperationChangesResponse* response) override {
    const auto prepared = prepare_operation_observation(*transport, *request, t::Service::Scheduler,
                                                        options_.invocation_deadline);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    const auto& command = std::get<t::OperationObservationCommand>(prepared);
    if (!options_.observations) {
      return error_status(failure(t::ErrorCode::CapabilityNotSupported), command.context.request_id,
                          command.context.trace_id);
    }
    const auto found = options_.observations->query(command);
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return error_status(*rejected, command.context.request_id, command.context.trace_id);
    }
    if (transport->IsCancelled() || now() >= command.context.deadline) {
      return error_status(failure(t::ErrorCode::Timeout, t::Recovery::Backoff),
                          command.context.request_id, command.context.trace_id);
    }
    const auto encoded = encode_operation_observation_page(
        std::get<t::OperationObservationPage>(found), request->context(), *response);
    if (const auto* rejected = std::get_if<t::Failure>(&encoded)) {
      return error_status(*rejected, command.context.request_id, command.context.trace_id);
    }
    return grpc::Status::OK;
  }

private:
  t::SchedulerCommandHandler& application_;
  SchedulerRpcOptions options_;
};

} // namespace

t::Outcome<std::unique_ptr<v1::SchedulerService::Service>>
make_scheduler_rpc(t::SchedulerCommandHandler& application, SchedulerRpcOptions options) {
  return std::unique_ptr<v1::SchedulerService::Service>(
      std::make_unique<SchedulerRpc>(application, std::move(options)));
}

} // namespace kiarabot::core::platform
