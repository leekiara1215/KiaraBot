#include "kiarabot/core/runtime/platform/host_runtime_client.hpp"

#include <cstdint>
#include <mutex>
#include <random>
#include <string_view>
#include <utility>

#include <kiarabot/v1/common.pb.h>
#include <kiarabot/v1/platform.pb.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace v1 = kiarabot::v1;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure invalid() {
  return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
t::Failure unconfirmed() {
  return {t::ErrorCode::UpstreamUnavailable, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity,
          std::nullopt, std::nullopt};
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
    return unconfirmed();
  default:
    return {t::ErrorCode::Internal, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity,
            std::nullopt, std::nullopt};
  }
}
bool fence_to_wire(const t::HostFence& fence, v1::Fence* output) {
  if (fence.host.value.empty() || fence.connection_generation == 0 || fence.runtime_epoch.empty()) {
    return false;
  }
  output->set_instance_id(fence.host.value);
  output->set_runtime_epoch(fence.runtime_epoch);
  output->set_connection_generation(fence.connection_generation);
  return !output->has_execution_generation();
}
bool operation_to_wire(const t::OperationRef& operation, v1::OperationRef* output) {
  if (operation.owner != t::Service::Host || operation.id.value.empty() ||
      operation.resource.kind != t::ResourceKind::Host || operation.resource.id.value.empty()) {
    return false;
  }
  const auto resource = target_codec::to_wire(operation.resource);
  if (!std::holds_alternative<v1::ResourceRef>(resource)) {
    return false;
  }
  output->set_owner_service("host");
  output->set_operation_id(operation.id.value);
  *output->mutable_resource() = std::get<v1::ResourceRef>(resource);
  return true;
}
bool operation_from_wire(const v1::OperationRef& wire, t::OperationRef* output) {
  if (wire.owner_service() != "host" || wire.operation_id().empty() || !wire.has_resource()) {
    return false;
  }
  const auto resource = target_codec::from_wire(wire.resource());
  if (!std::holds_alternative<t::ResourceRef>(resource)) {
    return false;
  }
  const auto& decoded = std::get<t::ResourceRef>(resource);
  if (decoded.kind != t::ResourceKind::Host || decoded.id.value.empty()) {
    return false;
  }
  *output = t::OperationRef{t::Service::Host, t::OperationId{wire.operation_id()}, decoded};
  return true;
}
v1::HostAdmissionState admission_wire(bool open) {
  return open ? v1::HOST_ADMISSION_OPEN : v1::HOST_ADMISSION_CLOSED;
}
} // namespace

HostRuntimeClient::HostRuntimeClient(HostRuntimeClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(channel_ ? v1::RuntimeService::NewStub(channel_) : nullptr) {}

target::Outcome<std::unique_ptr<HostRuntimeClient>>
HostRuntimeClient::create(HostRuntimeClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || !options.fence ||
      options.budget <= std::chrono::milliseconds::zero() ||
      options.budget > std::chrono::seconds{30} || options.maximum_budget < options.budget ||
      options.maximum_budget > std::chrono::seconds{30}) {
    return invalid();
  }
  return std::unique_ptr<HostRuntimeClient>(new HostRuntimeClient(std::move(options)));
}

HostRuntimeClient::~HostRuntimeClient() = default;

bool HostRuntimeClient::available() const {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

target::Outcome<target::HostAdmissionGateReceipt>
HostRuntimeClient::set_admission(const target::HostAdmissionGateRequest& request) {
  if (request.operation.owner != t::Service::Host || request.operation.id.value.empty() ||
      request.operation.resource.kind != t::ResourceKind::Host ||
      request.maintenance_version == 0) {
    return invalid();
  }
  auto fence = options_.fence();
  if (const auto* error = std::get_if<t::Failure>(&fence)) {
    return *error;
  }
  const auto& current = std::get<t::HostFence>(fence);
  if (current.host != request.operation.resource.id || current.connection_generation == 0 ||
      current.runtime_epoch.empty()) {
    // A gate for another Host or a missing current registration is never sent.
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  v1::RuntimeSetHostAdmissionGateRequest wire;
  wire.set_host_id(current.host.value);
  if (!fence_to_wire(current, wire.mutable_fence()) ||
      !operation_to_wire(request.operation, wire.mutable_operation()) ||
      request.maintenance_version == 0) {
    return invalid();
  }
  wire.set_maintenance_version(request.maintenance_version);
  if (request.expected_gate_version != 0) {
    wire.set_expected_gate_version(request.expected_gate_version);
  }
  wire.set_admission(admission_wire(request.admission_open));
  auto* context = wire.mutable_context();
  context->set_request_id(random_uuid());
  context->set_trace_id(random_uuid());
  context->set_idempotency_key(random_uuid());
  const auto started = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  const auto deadline = started + options_.budget;
  if (!timestamp(deadline, context->mutable_deadline())) {
    return failure(t::ErrorCode::Internal);
  }
  grpc::ClientContext client;
  client.set_deadline(std::chrono::system_clock::now() + options_.budget);
  v1::RuntimeSetHostAdmissionGateResponse response;
  const auto status = stub_->SetHostAdmissionGate(&client, wire, &response);
  if (!status.ok()) {
    return grpc_failure(status);
  }
  t::OperationRef echoed;
  if (response.host_id() != current.host.value ||
      response.maintenance_version() != request.maintenance_version ||
      response.gate_version() == 0 ||
      response.admission() != admission_wire(request.admission_open) ||
      !operation_from_wire(response.operation(), &echoed) || echoed != request.operation) {
    // A mismatched echo cannot prove the durable gate; keep the outcome unconfirmed.
    return unconfirmed();
  }
  return t::HostAdmissionGateReceipt{echoed, response.maintenance_version(),
                                     response.gate_version(), request.admission_open};
}

} // namespace kiarabot::core::platform
