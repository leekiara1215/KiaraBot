#include "kiarabot/core/runtime/platform/scheduler_runtime_client.hpp"

#include <array>
#include <chrono>
#include <random>
#include <string>
#include <string_view>

#include <google/protobuf/util/time_util.h>
#include <kiarabot/v1/common.pb.h>
#include <kiarabot/v1/events.pb.h>
#include <kiarabot/v1/platform.pb.h>
#include <kiarabot/v1/runtime.grpc.pb.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace v1 = kiarabot::v1;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
std::string random_uuid() {
  static std::mutex mutex;
  static std::mt19937_64 generator{std::random_device{}()};
  std::array<unsigned char, 16> bytes{};
  {
    std::lock_guard lock(mutex);
    const auto high = generator();
    const auto low = generator();
    for (std::size_t index = 0; index < 8; ++index) {
      bytes[index] = static_cast<unsigned char>((high >> ((7 - index) * 8)) & 0xffU);
      bytes[8 + index] = static_cast<unsigned char>((low >> ((7 - index) * 8)) & 0xffU);
    }
  }
  // RFC 4122 version 4 and variant bits are part of the frozen wire identity; owners reject a
  // non-canonical request/trace id as INVALID_ARGUMENT.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text += '-';
    }
    text += kHex[bytes[index] >> 4U];
    text += kHex[bytes[index] & 0x0fU];
  }
  return text;
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = target_codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
t::Failure grpc_failure(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::INVALID_ARGUMENT:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  case grpc::StatusCode::PERMISSION_DENIED:
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
  case grpc::StatusCode::UNAUTHENTICATED:
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
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
v1::ResourceKind resource_kind(t::ResourceKind kind) {
  const auto encoded = target_codec::to_wire(kind);
  if (const auto* wire = std::get_if<v1::ResourceKind>(&encoded)) {
    return *wire;
  }
  return v1::RESOURCE_KIND_UNSPECIFIED;
}
} // namespace

SchedulerRuntimeClient::SchedulerRuntimeClient(SchedulerRuntimeClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(v1::RuntimeService::NewStub(channel_)) {
  available_.store(channel_ != nullptr);
}

target::Outcome<std::unique_ptr<SchedulerRuntimeClient>>
SchedulerRuntimeClient::create(SchedulerRuntimeClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials ||
      options.budget <= std::chrono::milliseconds::zero() ||
      options.budget > std::chrono::seconds{30}) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  return std::unique_ptr<SchedulerRuntimeClient>(new SchedulerRuntimeClient(std::move(options)));
}

SchedulerRuntimeClient::~SchedulerRuntimeClient() = default;

bool SchedulerRuntimeClient::available() const {
  // Derive liveness from the channel so readiness recovers when Runtime returns, instead of
  // staying false until the next successful handoff (which may wait for an outbox lease).
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

target::Outcome<target::TimerDeliveryReceipt>
SchedulerRuntimeClient::accept(const target::TimerDeliveryRequest& request) {
  google::protobuf::Timestamp scratch;
  (void)scratch;
  v1::RuntimeAcceptTimerTriggerRequest wire;
  auto* context = wire.mutable_context();
  context->set_request_id(random_uuid());
  context->set_trace_id(random_uuid());
  if (!timestamp(t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())} +
                     options_.budget,
                 context->mutable_deadline())) {
    return failure(t::ErrorCode::Internal);
  }
  auto* handoff = wire.mutable_handoff();
  handoff->set_source_service("Scheduler");
  handoff->set_message_id(request.handoff.message.value);
  auto* operation = handoff->mutable_operation();
  operation->set_owner_service("Scheduler");
  operation->set_operation_id(request.handoff.operation.id.value);
  operation->mutable_resource()->set_kind(resource_kind(request.handoff.operation.resource.kind));
  operation->mutable_resource()->set_id(request.handoff.operation.resource.id.value);
  if (request.handoff.causation) {
    handoff->set_causation_id(request.handoff.causation->value);
  }
  handoff->mutable_resource()->set_kind(resource_kind(request.handoff.resource.kind));
  handoff->mutable_resource()->set_id(request.handoff.resource.id.value);
  handoff->set_resource_version(request.handoff.version.value);
  handoff->set_payload_sha256(
      std::string(reinterpret_cast<const char*>(request.handoff.payload_hash.bytes.data()),
                  request.handoff.payload_hash.bytes.size()));
  if (!timestamp(request.handoff.created_at, handoff->mutable_created_at()) ||
      !timestamp(request.handoff.valid_until, handoff->mutable_valid_until())) {
    return failure(t::ErrorCode::Internal);
  }
  wire.set_occurrence_id(request.occurrence_id.value);
  wire.set_task_id(request.task_id);
  wire.set_owner_instance_id(request.owner_instance_id.value);
  wire.set_schedule_version(request.schedule_version.value);
  wire.mutable_target()->set_id(request.target.conversation_id);
  wire.mutable_target()->set_type(request.target.kind == t::ConversationKey::Kind::Private
                                      ? v1::ConversationRef::PRIVATE
                                      : v1::ConversationRef::GROUP);
  wire.set_payload(
      std::string(reinterpret_cast<const char*>(request.payload.data()), request.payload.size()));
  grpc::ClientContext grpc_context;
  grpc_context.set_deadline(std::chrono::system_clock::now() + options_.budget);
  v1::RuntimeAcceptTimerTriggerResponse response;
  const auto status = stub_->AcceptTimerTrigger(&grpc_context, wire, &response);
  if (!status.ok()) {
    if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
        status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
      available_.store(false);
    }
    return grpc_failure(status);
  }
  available_.store(true);
  if (!response.has_receipt()) {
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                      t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
  const auto& receipt = response.receipt();
  target::TimerDeliveryReceipt decoded;
  decoded.event_id = t::MessageId{receipt.event_id()};
  decoded.duplicate = receipt.duplicate();
  const auto persisted = target_codec::from_wire(receipt.persisted_at());
  if (const auto* value = std::get_if<t::TimePoint>(&persisted)) {
    decoded.persisted_at = *value;
  }
  return decoded;
}

} // namespace kiarabot::core::platform
