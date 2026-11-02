#include "kiarabot/core/runtime/platform/scheduler_task_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <kiarabot/v1/common.pb.h>
#include <kiarabot/v1/runtime.pb.h>
#include <kiarabot/v1/scheduler.pb.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace v1 = kiarabot::v1;

bool valid_wire_task_id(std::string_view value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-';
  });
}
t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure grpc_failure(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::INVALID_ARGUMENT:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  case grpc::StatusCode::PERMISSION_DENIED:
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
  case grpc::StatusCode::UNAUTHENTICATED:
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  case grpc::StatusCode::NOT_FOUND:
    return failure(t::ErrorCode::NotFound, t::Recovery::CorrectInput);
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
  case grpc::StatusCode::RESOURCE_EXHAUSTED:
    return failure(t::ErrorCode::RateLimited, t::Recovery::Backoff);
  case grpc::StatusCode::ABORTED:
  case grpc::StatusCode::ALREADY_EXISTS:
    return failure(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
  case grpc::StatusCode::UNAVAILABLE:
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  default:
    return {t::ErrorCode::Internal, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity,
            std::nullopt, std::nullopt};
  }
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = target_codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
std::optional<t::TaskState> task_state(v1::ScheduledTaskState value) {
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
  case v1::SCHEDULED_TASK_STATE_UNSPECIFIED:
    return std::nullopt;
  }
  return std::nullopt;
}
v1::ConversationRef::Type conversation_type(t::ConversationKey::Kind kind) {
  return kind == t::ConversationKey::Kind::Group ? v1::ConversationRef::GROUP
                                                 : v1::ConversationRef::PRIVATE;
}
bool encode_context(v1::RequestContext* wire, const t::SchedulerTaskContext& context,
                    std::chrono::milliseconds budget) {
  const auto now = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  wire->set_request_id(context.request_id);
  wire->set_trace_id(context.trace_id);
  return timestamp(std::min(context.deadline, now + budget), wire->mutable_deadline());
}
void scope(v1::ResourceScope* wire, const t::SchedulerTaskContext& context) {
  auto* resource = wire->add_resources();
  resource->set_kind(v1::RESOURCE_INSTANCE);
  resource->set_id(context.owner.value);
}
t::Outcome<t::ScheduledTask> decode(const v1::TaskRecord& wire) {
  const auto state = task_state(wire.state());
  if (wire.task_id().empty() || !state) {
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity);
  }
  t::ScheduledTask task;
  task.key = t::TaskKey{t::InstanceId{wire.owner_instance_id()}, wire.task_id()};
  task.installation = t::InstallationId{wire.installation_id()};
  task.target = t::ConversationKey{{},
                                   {},
                                   wire.target().id(),
                                   wire.target().type() == v1::ConversationRef::GROUP
                                       ? t::ConversationKey::Kind::Group
                                       : t::ConversationKey::Kind::Private};
  task.payload.resize(static_cast<std::size_t>(wire.payload().size()));
  std::memcpy(task.payload.data(), wire.payload().data(), task.payload.size());
  const auto run = target_codec::from_wire(wire.run_at());
  if (const auto* value = std::get_if<t::TimePoint>(&run)) {
    task.run_at = *value;
  }
  task.state = *state;
  task.version = t::ResourceVersion{wire.version()};
  if (!wire.occurrence_id().empty()) {
    task.occurrence = t::MessageId{wire.occurrence_id()};
  }
  return task;
}
} // namespace

std::function<t::Outcome<std::string>()> unavailable_scheduler_task_delegation() {
  return []() -> t::Outcome<std::string> {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  };
}

SchedulerTaskClient::SchedulerTaskClient(SchedulerTaskClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(v1::SchedulerService::NewStub(channel_)) {}

t::Outcome<std::unique_ptr<SchedulerTaskClient>>
SchedulerTaskClient::create(SchedulerTaskClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || !options.delegation_provider ||
      options.maximum_budget <= std::chrono::milliseconds::zero() ||
      options.maximum_budget > std::chrono::seconds{30}) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  return std::unique_ptr<SchedulerTaskClient>(new SchedulerTaskClient(std::move(options)));
}

SchedulerTaskClient::~SchedulerTaskClient() = default;

bool SchedulerTaskClient::available() const {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

t::Outcome<t::ScheduledTask>
SchedulerTaskClient::create_task(const t::SchedulerTaskContext& context,
                                 const t::TaskCreationInput& input) {
  if (context.owner.value.empty() || !valid_wire_task_id(input.task_id) ||
      input.target.conversation_id.empty() || (!input.run_at && !input.local_time)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  auto delegation = options_.delegation_provider();
  if (const auto* rejected = std::get_if<t::Failure>(&delegation)) {
    return *rejected;
  }
  auto& proof = std::get<std::string>(delegation);
  if (proof.empty() || proof.size() > 32768) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  v1::SchedulerCreateTaskRequest wire;
  if (!encode_context(wire.mutable_context(), context, options_.maximum_budget)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  wire.mutable_context()->set_delegation(proof);
  wire.set_task_id(input.task_id);
  auto* target = wire.mutable_target();
  target->set_id(input.target.conversation_id);
  target->set_type(conversation_type(input.target.kind));
  wire.set_payload(
      std::string(reinterpret_cast<const char*>(input.payload.data()), input.payload.size()));
  if (input.run_at) {
    if (!timestamp(*input.run_at, wire.mutable_run_at())) {
      return failure(t::ErrorCode::Internal);
    }
  } else {
    auto* local = wire.mutable_local_time();
    local->set_year(input.local_time->year);
    local->set_month(input.local_time->month);
    local->set_day(input.local_time->day);
    local->set_hour(input.local_time->hour);
    local->set_minute(input.local_time->minute);
    local->set_second(input.local_time->second);
    local->set_time_zone(input.local_time->time_zone);
  }
  grpc::ClientContext call;
  call.set_deadline(std::chrono::system_clock::now() + options_.maximum_budget);
  v1::SchedulerCreateTaskResponse response;
  const auto status = stub_->CreateTask(&call, wire, &response);
  if (!status.ok()) {
    return grpc_failure(status);
  }
  return decode(response.task());
}

t::Outcome<t::ScheduledTask> SchedulerTaskClient::get_task(const t::SchedulerTaskContext& context,
                                                           std::string_view task_id) {
  if (!valid_wire_task_id(task_id)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  auto delegation = options_.delegation_provider();
  if (const auto* rejected = std::get_if<t::Failure>(&delegation)) {
    return *rejected;
  }
  auto& proof = std::get<std::string>(delegation);
  if (proof.empty() || proof.size() > 32768) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  v1::SchedulerGetTaskRequest wire;
  if (!encode_context(wire.mutable_context(), context, options_.maximum_budget)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  wire.mutable_context()->set_delegation(proof);
  wire.set_task_id(std::string{task_id});
  scope(wire.mutable_scope(), context);
  grpc::ClientContext call;
  call.set_deadline(std::chrono::system_clock::now() + options_.maximum_budget);
  v1::SchedulerGetTaskResponse response;
  const auto status = stub_->GetTask(&call, wire, &response);
  if (!status.ok()) {
    return grpc_failure(status);
  }
  return decode(response.task());
}

t::Outcome<t::ScheduledTask>
SchedulerTaskClient::cancel_task(const t::SchedulerTaskContext& context, std::string_view task_id) {
  if (!valid_wire_task_id(task_id)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  auto delegation = options_.delegation_provider();
  if (const auto* rejected = std::get_if<t::Failure>(&delegation)) {
    return *rejected;
  }
  auto& proof = std::get<std::string>(delegation);
  if (proof.empty() || proof.size() > 32768) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  v1::SchedulerCancelTaskRequest wire;
  if (!encode_context(wire.mutable_context(), context, options_.maximum_budget)) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  wire.mutable_context()->set_delegation(proof);
  wire.set_task_id(std::string{task_id});
  scope(wire.mutable_scope(), context);
  grpc::ClientContext call;
  call.set_deadline(std::chrono::system_clock::now() + options_.maximum_budget);
  v1::SchedulerCancelTaskResponse response;
  const auto status = stub_->CancelTask(&call, wire, &response);
  if (!status.ok()) {
    return grpc_failure(status);
  }
  return decode(response.task());
}

} // namespace kiarabot::core::platform
