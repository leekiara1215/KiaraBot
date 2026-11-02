#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/adapters.pb.h"
#include "kiarabot/v1/events.pb.h"
#include "kiarabot/v1/plugins.pb.h"
#include "kiarabot/v1/services.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace target = kiarabot::core::target;
namespace codec = kiarabot::core::platform::target_codec;
using target::Failure;
using target::Outcome;
using target::Status;

constexpr std::string_view kAuthorizationMetadata = "authorization";

Failure failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}
grpc::StatusCode status_of(target::ErrorCode code) {
  auto mapped = codec::grpc_status(code);
  if (const auto* status = std::get_if<grpc::StatusCode>(&mapped)) {
    return *status;
  }
  return grpc::StatusCode::UNKNOWN;
}
v1::Error wire_error(target::ErrorCode code, std::string_view request_id, std::string_view trace_id,
                     bool retryable = false) {
  auto made = codec::make_error(code, request_id, trace_id, retryable);
  if (const auto* value = std::get_if<v1::Error>(&made)) {
    return *value;
  }
  v1::Error fallback;
  fallback.set_code(v1::INTERNAL);
  return fallback;
}
// Exactly one bearer bootstrap credential; any missing/duplicate/foreign metadata fails closed.
bool bearer_metadata(grpc::ServerContext& context, std::string& credential) {
  bool seen = false;
  for (const auto& entry : context.client_metadata()) {
    const std::string_view key{entry.first.data(), entry.first.size()};
    const std::string_view value{entry.second.data(), entry.second.size()};
    if (key != kAuthorizationMetadata) {
      continue;
    }
    if (seen) {
      return false;
    }
    seen = true;
    if (!value.starts_with("Bearer ")) {
      return false;
    }
    credential = std::string{value.substr(7)};
  }
  return seen && !credential.empty();
}
bool fence_to_wire(const target::BridgeFence& value, v1::Fence* wire) {
  if (!wire) {
    return false;
  }
  wire->set_instance_id(value.instance.value);
  wire->set_runtime_epoch(value.runtime_epoch.value);
  wire->set_execution_generation(value.execution_generation.value);
  wire->set_connection_generation(value.connection_generation.value);
  return true;
}
target::Outcome<target::BridgeFence> fence_from_wire(const v1::Fence& wire) {
  if (wire.instance_id().empty() || wire.runtime_epoch().empty() ||
      !wire.has_execution_generation() || !wire.has_connection_generation() ||
      wire.execution_generation() == 0 || wire.connection_generation() == 0) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  return target::BridgeFence{target::InstanceId{wire.instance_id()},
                             target::RuntimeEpoch{wire.runtime_epoch()},
                             target::ExecutionGeneration{wire.execution_generation()},
                             target::ConnectionGeneration{wire.connection_generation()}};
}
target::Outcome<std::array<std::byte, 32>> proof_from_wire(const std::string& value) {
  if (value.size() != 32) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  std::array<std::byte, 32> proof{};
  for (std::size_t index = 0; index < proof.size(); ++index) {
    proof[index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
  }
  return proof;
}
void authorization_to_wire(const target::BridgeAuthorization& value,
                           v1::ConnectionAuthorization* wire) {
  if (!wire) {
    return;
  }
  fence_to_wire(value.fence, wire->mutable_fence());
  wire->set_proof(reinterpret_cast<const char*>(value.proof.data()), value.proof.size());
  wire->set_refresh_credential(reinterpret_cast<const char*>(value.refresh_credential.data()),
                               value.refresh_credential.size());
  wire->set_refresh_generation(value.refresh_generation);
  wire->set_credential_version(value.credential_version);
  wire->set_authorization_version(value.authorization_version);
  if (auto issued = codec::to_wire(value.issued_at);
      std::holds_alternative<google::protobuf::Timestamp>(issued)) {
    *wire->mutable_issued_at() = std::get<google::protobuf::Timestamp>(issued);
  }
  if (auto expires = codec::to_wire(value.expires_at);
      std::holds_alternative<google::protobuf::Timestamp>(expires)) {
    *wire->mutable_expires_at() = std::get<google::protobuf::Timestamp>(expires);
  }
  wire->set_principal_id(value.fence.instance.value);
}
template <typename Ack> void fill_heartbeat_ack(Ack* ack) {
  ack->set_received_at_unix_ms(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()));
}

// Shared post-registration loop state for one accepted stream.
struct StreamState {
  target::BridgePeerKind kind = target::BridgePeerKind::Adapter;
  target::BridgeAuthorization authorization;
  std::string adapter_id;
  target::TimePoint sampled_at;
  bool sampled = false;
};

target::Outcome<target::BridgeFence> frame_fence(const v1::Fence& wire, const std::string& proof,
                                                 std::array<std::byte, 32>& parsed_proof) {
  auto fence = fence_from_wire(wire);
  if (const auto* rejected = std::get_if<target::Failure>(&fence)) {
    return *rejected;
  }
  auto parsed = proof_from_wire(proof);
  if (const auto* rejected = std::get_if<target::Failure>(&parsed)) {
    return *rejected;
  }
  parsed_proof = std::get<std::array<std::byte, 32>>(parsed);
  return std::get<target::BridgeFence>(fence);
}
target::Status authorize_frame(target::RuntimeBridgeApplication& application, StreamState& state,
                               const v1::Fence& fence, const std::string& proof) {
  std::array<std::byte, 32> parsed{};
  auto expected = frame_fence(fence, proof, parsed);
  if (const auto* rejected = std::get_if<target::Failure>(&expected)) {
    return *rejected;
  }
  auto sampled = application.current_time();
  if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
    return *rejected;
  }
  state.sampled_at = std::get<target::TimePoint>(sampled);
  state.sampled = true;
  return application.authorize_frame(state.kind, state.authorization,
                                     std::get<target::BridgeFence>(expected), parsed,
                                     state.sampled_at);
}
// Serialized access to one live gRPC stream. Sinks capture a weak_ptr and can never touch a raw
// stream pointer after handler teardown; teardown sets alive=false under the same mutex, so an
// in-flight sink write either completes before teardown or sees the closed guard.
template <typename Out, typename In> struct StreamGuard {
  grpc::ServerReaderWriter<Out, In>* stream;
  grpc::ServerContext* context;
  std::mutex mutex;
  bool alive = true;
  StreamGuard(grpc::ServerReaderWriter<Out, In>* value, grpc::ServerContext* server)
      : stream(value), context(server) {}
  bool write(const Out& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!alive) {
      return false;
    }
    if (!stream->Write(frame)) {
      alive = false;
      return false;
    }
    return true;
  }
  void close() {
    std::lock_guard<std::mutex> lock(mutex);
    alive = false;
  }
};
// Bounded shutdown write: a watchdog cancels the call if Write does not return within the budget.
// The writer holds the guard mutex, so handler teardown waits for an in-flight write and no raw
// stream pointer can outlive the handler.
template <typename Out, typename In>
bool bounded_write(const std::shared_ptr<StreamGuard<Out, In>>& guard, const Out& frame,
                   std::chrono::milliseconds budget) {
  if (!guard) {
    return false;
  }
  std::atomic<bool> finished{false};
  std::thread watchdog([guard, &finished, budget] {
    std::this_thread::sleep_for(budget);
    if (!finished.load()) {
      guard->context->TryCancel();
    }
  });
  const bool written = guard->write(frame);
  finished.store(true);
  watchdog.join();
  return written;
}
class FirstFrameWatchdog {
public:
  explicit FirstFrameWatchdog(grpc::ServerContext* context) : context_(context) {
    thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!finished_ &&
          !condition_.wait_for(lock, std::chrono::seconds{5}, [this] { return finished_; })) {
        context_->TryCancel();
      }
    });
  }
  void finish() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finished_ = true;
    }
    condition_.notify_all();
  }
  ~FirstFrameWatchdog() {
    finish();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  grpc::ServerContext* context_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool finished_ = false;
  std::thread thread_;
};
bool transport_registration_failure(target::ErrorCode code) {
  switch (code) {
  case target::ErrorCode::Unauthenticated:
  case target::ErrorCode::PermissionDenied:
  case target::ErrorCode::InvalidArgument:
  case target::ErrorCode::UpstreamUnavailable:
  case target::ErrorCode::Timeout:
  case target::ErrorCode::Internal:
    return true;
  default:
    return false;
  }
}
target::Failure retryable_failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Backoff, {}, {}};
}

grpc::Status rejected_status(const target::Failure& rejected) {
  return {status_of(rejected.code), "bridge request rejected"};
}
target::Outcome<target::TimePoint> register_deadline(const v1::RequestContext& context) {
  if (!context.has_deadline()) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto deadline = codec::from_wire(context.deadline());
  if (const auto* value = std::get_if<target::TimePoint>(&deadline)) {
    return *value;
  }
  return failure(target::ErrorCode::InvalidArgument);
}
std::vector<target::Capability>
capabilities_from_wire(const google::protobuf::RepeatedPtrField<v1::Capability>& wire) {
  std::vector<target::Capability> result;
  result.reserve(static_cast<std::size_t>(wire.size()));
  for (const auto& capability : wire) {
    result.push_back({capability.name(), capability.version()});
  }
  return result;
}
void capabilities_to_wire(const std::vector<target::Capability>& values,
                          google::protobuf::RepeatedPtrField<v1::Capability>* wire) {
  for (const auto& value : values) {
    auto* capability = wire->Add();
    capability->set_name(value.name);
    capability->set_version(value.version);
    capability->set_supported(true);
  }
}
target::Outcome<target::BridgeAuthorization>
refresh(target::RuntimeBridgeApplication& application, StreamState& state,
        const v1::RefreshConnectionAuthorization& wire);

target::Outcome<target::BridgeEventIngress>
event_ingress(const v1::EventEnvelope& envelope, const target::InstanceId& adapter_instance,
              std::string_view adapter_id) {
  if (envelope.event_id().empty() || envelope.event_id().size() > 256 ||
      envelope.schema_version() != "kiarabot.v1" || !envelope.has_occurred_at() ||
      envelope.source().adapter_id().empty() || envelope.source().account_id().empty() ||
      envelope.source().adapter_id() != adapter_id) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto occurred = codec::from_wire(envelope.occurred_at());
  if (const auto* rejected = std::get_if<target::Failure>(&occurred)) {
    return *rejected;
  }
  if (!envelope.source().adapter_instance_id().empty() &&
      envelope.source().adapter_instance_id() != adapter_instance.value) {
    return failure(target::ErrorCode::PermissionDenied);
  }
  target::BridgeEventIngress ingress;
  ingress.adapter_id = envelope.source().adapter_id();
  ingress.account_id = envelope.source().account_id();
  ingress.event_id = envelope.event_id();
  ingress.trace_id = envelope.trace_id();
  ingress.occurred_at = std::get<target::TimePoint>(occurred);
  if (envelope.sequence() != 0) {
    ingress.sequence = envelope.sequence();
  }
  switch (envelope.payload_case()) {
  case v1::EventEnvelope::kMessageReceived: {
    ingress.payload = target::EventPayloadKind::MessageReceived;
    const auto& conversation = envelope.message_received().conversation();
    ingress.has_conversation = !conversation.id().empty();
    ingress.conversation_id = conversation.id();
    ingress.conversation_type = conversation.type() == v1::ConversationRef::GROUP
                                    ? target::ConversationKey::Kind::Group
                                : conversation.type() == v1::ConversationRef::PRIVATE
                                    ? target::ConversationKey::Kind::Private
                                    : target::ConversationKey::Kind::Unspecified;
    break;
  }
  case v1::EventEnvelope::kMessageSent: {
    ingress.payload = target::EventPayloadKind::MessageSent;
    const auto& conversation = envelope.message_sent().conversation();
    ingress.has_conversation = !conversation.id().empty();
    ingress.conversation_id = conversation.id();
    ingress.conversation_type = conversation.type() == v1::ConversationRef::GROUP
                                    ? target::ConversationKey::Kind::Group
                                : conversation.type() == v1::ConversationRef::PRIVATE
                                    ? target::ConversationKey::Kind::Private
                                    : target::ConversationKey::Kind::Unspecified;
    break;
  }
  case v1::EventEnvelope::kGroupMemberChanged:
    ingress.payload = target::EventPayloadKind::GroupMemberChanged;
    break;
  case v1::EventEnvelope::kFriendRequest:
    ingress.payload = target::EventPayloadKind::FriendRequest;
    break;
  case v1::EventEnvelope::kAdapterStatusChanged:
    ingress.payload = target::EventPayloadKind::AdapterStatusChanged;
    break;
  default:
    return failure(target::ErrorCode::CapabilityNotSupported);
  }
  if (ingress.trace_id.empty()) {
    ingress.trace_id = ingress.event_id;
  }
  if (!envelope.SerializeToString(&ingress.serialized_envelope)) {
    return failure(target::ErrorCode::Internal);
  }
  return ingress;
}
std::optional<target::Failure> failure_from_wire(const v1::Error& error) {
  auto code = codec::from_wire(error.code());
  if (const auto* mapped = std::get_if<target::ErrorCode>(&code)) {
    return target::Failure{*mapped,
                           target::Acceptance::NotAccepted,
                           error.retryable() ? target::Recovery::Backoff
                                             : target::Recovery::Diagnose,
                           {},
                           {}};
  }
  return std::nullopt;
}
v1::DeliveryAttempt attempt_to_wire(const target::DeliveryAttempt& attempt) {
  v1::DeliveryAttempt wire;
  wire.set_event_id(attempt.delivery.event.event_id);
  wire.set_attempt_id(attempt.attempt.value);
  fence_to_wire(target::BridgeFence{attempt.fence.instance, attempt.fence.runtime_epoch,
                                    attempt.fence.execution_generation,
                                    attempt.fence.connection_generation},
                wire.mutable_fence());
  if (auto started = codec::to_wire(attempt.started_at);
      std::holds_alternative<google::protobuf::Timestamp>(started)) {
    *wire.mutable_started_at() = std::get<google::protobuf::Timestamp>(started);
  }
  if (auto lease = codec::to_wire(attempt.lease_until);
      std::holds_alternative<google::protobuf::Timestamp>(lease)) {
    *wire.mutable_lease_until() = std::get<google::protobuf::Timestamp>(lease);
  }
  wire.set_attempt_number(attempt.attempt_number);
  return wire;
}
target::Outcome<target::BridgeAttemptRef> attempt_from_wire(const v1::DeliveryAttempt& wire) {
  if (wire.event_id().empty() || wire.attempt_id().empty() || !wire.has_fence()) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto fence = fence_from_wire(wire.fence());
  if (const auto* rejected = std::get_if<target::Failure>(&fence)) {
    return *rejected;
  }
  return target::BridgeAttemptRef{wire.event_id(), target::AttemptId{wire.attempt_id()},
                                  std::get<target::BridgeFence>(fence)};
}
void receipt_to_wire(const target::DeliveryReceipt& receipt, v1::DeliveryReceipt* wire) {
  *wire->mutable_attempt() = attempt_to_wire(receipt.attempt);
  switch (receipt.disposition) {
  case target::DeliveryDisposition::Acknowledged:
    wire->set_disposition(v1::DELIVERY_ACKNOWLEDGED);
    break;
  case target::DeliveryDisposition::Duplicate:
    wire->set_disposition(v1::DELIVERY_DUPLICATE);
    break;
  case target::DeliveryDisposition::Renewed:
    wire->set_disposition(v1::DELIVERY_RENEWED);
    break;
  case target::DeliveryDisposition::Rejected:
    wire->set_disposition(v1::DELIVERY_REJECTED);
    break;
  case target::DeliveryDisposition::Stale:
  default:
    wire->set_disposition(v1::DELIVERY_STALE);
    break;
  }
  if (receipt.failure) {
    auto* failure_wire = wire->mutable_failure();
    *failure_wire->mutable_error() =
        wire_error(receipt.failure->code, receipt.attempt.delivery.event.event_id, {});
  }
}

class AdapterBridgeImpl final : public v1::AdapterBridge::Service {
public:
  explicit AdapterBridgeImpl(target::RuntimeBridgeApplication& application)
      : application_(application) {}

  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<v1::CoreToAdapter, v1::AdapterToCore>* stream) override {
    FirstFrameWatchdog watchdog(context);
    guard_ = std::make_shared<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>>(stream, context);
    std::string bearer;
    if (!bearer_metadata(*context, bearer)) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bridge authorization missing"};
    }
    v1::AdapterToCore first;
    if (!stream->Read(&first) || first.body_case() != v1::AdapterToCore::kRegister) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "registration frame required"};
    }
    watchdog.finish();
    const auto& registered = first.register_();
    auto deadline = register_deadline(registered.context());
    if (const auto* rejected = std::get_if<target::Failure>(&deadline)) {
      return rejected_status(*rejected);
    }
    target::BridgeAdapterRegistration command;
    command.request_id = registered.context().request_id();
    command.trace_id = registered.context().trace_id();
    command.deadline = std::get<target::TimePoint>(deadline);
    command.adapter = target::ResourceId{registered.adapter_id()};
    command.implementation = registered.implementation();
    command.implementation_version = registered.implementation_version();
    command.protocol_version = registered.protocol_version();
    if (!registered.adapter_instance_id().empty()) {
      command.presented_instance = target::InstanceId{registered.adapter_instance_id()};
    }
    for (const auto& account : registered.accounts()) {
      command.declared_accounts.push_back(target::ResourceId{account.account_id()});
    }
    command.declared_capabilities = capabilities_from_wire(registered.capabilities());
    command.bearer_credential = bearer;
    auto outcome = application_.register_adapter(command);
    if (const auto* rejected = std::get_if<target::Failure>(&outcome)) {
      if (transport_registration_failure(rejected->code)) {
        return rejected_status(*rejected);
      }
      v1::CoreToAdapter response;
      auto* rejected_response = response.mutable_register_result();
      rejected_response->set_accepted(false);
      *rejected_response->mutable_error() =
          wire_error(rejected->code, command.request_id, command.trace_id,
                     rejected->recovery == target::Recovery::Backoff);
      (void)stream->Write(response);
      return grpc::Status::OK;
    }
    const auto& result = std::get<target::BridgeRegistrationResult>(outcome);
    StreamState state{
        target::BridgePeerKind::Adapter, result.authorization, command.adapter.value, {}, false};
    const auto stream_instance = result.authorization.fence.instance;
    const auto stream_generation = result.authorization.fence.connection_generation;
    bool shutdown_attached = false;
    struct Exit {
      target::RuntimeBridgeApplication& application;
      std::shared_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> guard;
      target::InstanceId instance;
      target::ConnectionGeneration generation;
      bool* attached;
      ~Exit() {
        if (*attached) {
          application.detach_shutdown_sink(target::BridgePeerKind::Adapter, instance, generation);
        }
        guard->close();
        application.disconnect(target::BridgePeerKind::Adapter, instance, generation);
      }
    } exit_guard{application_, guard_, stream_instance, stream_generation, &shutdown_attached};
    {
      v1::CoreToAdapter response;
      auto* registered_response = response.mutable_register_result();
      registered_response->set_accepted(true);
      registered_response->set_assigned_instance_id(result.assigned_instance_id.value);
      registered_response->set_required_protocol_version(result.required_protocol_version);
      registered_response->set_heartbeat_interval_seconds(result.heartbeat_interval_seconds);
      fence_to_wire(result.authorization.fence, registered_response->mutable_fence());
      capabilities_to_wire(result.effective_capabilities,
                           registered_response->mutable_effective_capabilities());
      authorization_to_wire(result.authorization, registered_response->mutable_authorization());
      fence_to_wire(result.authorization.fence, response.mutable_fence());
      if (!write(*stream, response)) {
        return grpc::Status::OK;
      }
    }
    const auto shutdown_fence = state.authorization.fence;
    {
      std::weak_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> weak = guard_;
      application_.attach_shutdown_sink(
          target::BridgePeerKind::Adapter, stream_instance, stream_generation,
          [weak, shutdown_fence]() -> target::Status {
            auto guard = weak.lock();
            if (!guard) {
              return retryable_failure(target::ErrorCode::UpstreamUnavailable);
            }
            v1::CoreToAdapter notice;
            fence_to_wire(shutdown_fence, notice.mutable_fence());
            notice.mutable_shutdown()->set_reason("shutdown");
            notice.mutable_shutdown()->set_drain_timeout_ms(1000);
            if (!bounded_write(guard, notice, std::chrono::milliseconds{1000})) {
              return retryable_failure(target::ErrorCode::UpstreamUnavailable);
            }
            return std::monostate{};
          });
      shutdown_attached = true;
    }
    grpc::Status status = grpc::Status::OK;
    while (!context->IsCancelled()) {
      v1::AdapterToCore frame;
      if (!stream->Read(&frame)) {
        break;
      }
      auto authorized =
          authorize_frame(application_, state, frame.fence(), frame.connection_proof());
      if (const auto* rejected = std::get_if<target::Failure>(&authorized)) {
        status = rejected_status(*rejected);
        break;
      }
      const auto body = frame.body_case();
      if (body == v1::AdapterToCore::kHeartbeat) {
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        fill_heartbeat_ack(response.mutable_heartbeat_ack());
        state.sampled = false;
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::AdapterToCore::kRefreshAuthorization) {
        auto refreshed = refresh(application_, state, frame.refresh_authorization());
        if (const auto* rejected = std::get_if<target::Failure>(&refreshed)) {
          if (rejected->code == target::ErrorCode::Unauthenticated ||
              rejected->code == target::ErrorCode::PermissionDenied) {
            status = rejected_status(*rejected);
            break;
          }
          v1::CoreToAdapter response;
          fence_to_wire(state.authorization.fence, response.mutable_fence());
          auto* result = response.mutable_authorization_result();
          result->set_request_id(frame.refresh_authorization().request_id());
          *result->mutable_error() =
              wire_error(rejected->code, frame.refresh_authorization().request_id(), {});
          state.sampled = false;
          if (!write(*stream, response)) {
            break;
          }
          status = rejected_status(*rejected);
          break;
        }
        state.authorization = std::get<target::BridgeAuthorization>(refreshed);
        state.sampled = false;
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* result = response.mutable_authorization_result();
        result->set_request_id(frame.refresh_authorization().request_id());
        authorization_to_wire(state.authorization, result->mutable_authorization());
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::AdapterToCore::kRegister) {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "duplicate registration frame"};
        break;
      } else if (body == v1::AdapterToCore::kEvent) {
        auto ingress =
            event_ingress(frame.event(), state.authorization.fence.instance, state.adapter_id);
        if (const auto* rejected = std::get_if<target::Failure>(&ingress)) {
          v1::CoreToAdapter response;
          fence_to_wire(state.authorization.fence, response.mutable_fence());
          auto* ack = response.mutable_event_ack();
          ack->set_event_id(frame.event().event_id());
          ack->set_trace_id(frame.event().trace_id());
          ack->set_status(v1::AdapterEventAck::REJECTED);
          *ack->mutable_error() =
              wire_error(rejected->code, frame.event().event_id(), frame.event().trace_id());
          if (!write(*stream, response)) {
            break;
          }
          continue;
        }
        auto acknowledged = application_.ingest_event(
            state.authorization.fence.instance, std::get<target::BridgeEventIngress>(ingress));
        if (const auto* rejected = std::get_if<target::Failure>(&acknowledged)) {
          status = rejected_status(*rejected);
          break;
        }
        const auto& ack_result = std::get<target::BridgeEventAck>(acknowledged);
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* ack = response.mutable_event_ack();
        ack->set_event_id(frame.event().event_id());
        ack->set_trace_id(frame.event().trace_id());
        switch (ack_result.status) {
        case target::BridgeAckStatus::Persisted:
          ack->set_status(v1::AdapterEventAck::PERSISTED);
          break;
        case target::BridgeAckStatus::Duplicate:
          ack->set_status(v1::AdapterEventAck::DUPLICATE);
          break;
        case target::BridgeAckStatus::Rejected:
          ack->set_status(v1::AdapterEventAck::REJECTED);
          break;
        }
        if (ack_result.failure) {
          *ack->mutable_error() = wire_error(ack_result.failure->code, frame.event().event_id(),
                                             frame.event().trace_id(), ack_result.retryable);
          if (ack_result.retry_after_ms) {
            ack->set_retry_after_ms(ack_result.retry_after_ms);
          }
        }
        if (!write(*stream, response)) {
          break;
        }
        if (ack_result.close_stream) {
          return grpc::Status::OK;
        }
      } else {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "unsupported bridge frame"};
        break;
      }
    }
    return status;
  }

private:
  bool write(grpc::ServerReaderWriter<v1::CoreToAdapter, v1::AdapterToCore>&,
             const v1::CoreToAdapter& frame) {
    return guard_ && guard_->write(frame);
  }
  target::RuntimeBridgeApplication& application_;
  std::shared_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> guard_;
};

class PluginBridgeImpl final : public v1::PluginBridge::Service {
public:
  explicit PluginBridgeImpl(target::RuntimeBridgeApplication& application)
      : application_(application) {}

  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<v1::CoreToPlugin, v1::PluginToCore>* stream) override {
    FirstFrameWatchdog watchdog(context);
    guard_ = std::make_shared<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>>(stream, context);
    std::string bearer;
    if (!bearer_metadata(*context, bearer)) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bridge authorization missing"};
    }
    v1::PluginToCore first;
    if (!stream->Read(&first) || first.body_case() != v1::AdapterToCore::kRegister) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "registration frame required"};
    }
    watchdog.finish();
    const auto& registered = first.register_();
    target::BridgePluginRegistration command;
    command.request_id = "plugin-register-" + registered.instance_id();
    command.instance_id = registered.instance_id();
    command.bearer_credential = bearer;
    command.manifest.id = registered.manifest().id();
    command.manifest.name = registered.manifest().name();
    command.manifest.version = registered.manifest().version();
    command.manifest.api_version = registered.manifest().api_version();
    command.manifest.runtime = registered.manifest().runtime();
    command.manifest.entrypoint = registered.manifest().entrypoint();
    command.manifest.isolation = registered.manifest().isolation();
    command.manifest.config_schema = registered.manifest().config_schema();
    command.manifest.ordered = registered.manifest().ordered();
    command.manifest.capabilities.assign(registered.manifest().capabilities().begin(),
                                         registered.manifest().capabilities().end());
    command.manifest.permissions.assign(registered.manifest().permissions().begin(),
                                        registered.manifest().permissions().end());
    command.manifest.dependencies.assign(registered.manifest().dependencies().begin(),
                                         registered.manifest().dependencies().end());
    command.supported_features.assign(registered.supported_features().begin(),
                                      registered.supported_features().end());
    if (!registered.installation_id().empty()) {
      command.installation_id = target::InstallationId{registered.installation_id()};
    }
    if (registered.has_execution_generation()) {
      command.execution_generation = registered.execution_generation();
    }
    auto outcome = application_.register_plugin(command);
    if (const auto* rejected = std::get_if<target::Failure>(&outcome)) {
      if (transport_registration_failure(rejected->code)) {
        return rejected_status(*rejected);
      }
      v1::CoreToPlugin response;
      auto* rejected_response = response.mutable_register_result();
      rejected_response->set_accepted(false);
      *rejected_response->mutable_error() =
          wire_error(rejected->code, {}, {}, rejected->recovery == target::Recovery::Backoff);
      (void)stream->Write(response);
      return grpc::Status::OK;
    }
    const auto& result = std::get<target::BridgeRegistrationResult>(outcome);
    StreamState state{target::BridgePeerKind::Plugin, result.authorization, {}, {}, false};
    const auto stream_instance = result.authorization.fence.instance;
    const auto stream_generation = result.authorization.fence.connection_generation;
    bool delivery_attached = false;
    bool shutdown_attached = false;
    struct Exit {
      target::RuntimeBridgeApplication& application;
      std::shared_ptr<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>> guard;
      target::InstanceId instance;
      target::ConnectionGeneration generation;
      bool* delivery_attached;
      bool* shutdown_attached;
      ~Exit() {
        if (*delivery_attached) {
          application.detach_plugin_delivery(instance, generation);
        }
        if (*shutdown_attached) {
          application.detach_shutdown_sink(target::BridgePeerKind::Plugin, instance, generation);
        }
        guard->close();
        application.disconnect(target::BridgePeerKind::Plugin, instance, generation);
      }
    } exit_guard{application_,      guard_, stream_instance, stream_generation, &delivery_attached,
                 &shutdown_attached};
    {
      v1::CoreToPlugin response;
      auto* registered_response = response.mutable_register_result();
      registered_response->set_accepted(true);
      registered_response->set_required_api_version(result.required_api_version);
      for (const auto& permission : result.granted_permissions) {
        registered_response->add_granted_permissions(permission);
      }
      for (const auto& feature : result.enabled_features) {
        registered_response->add_enabled_features(feature);
      }
      registered_response->set_desired_config_version(result.desired_config_version);
      fence_to_wire(result.authorization.fence, registered_response->mutable_fence());
      authorization_to_wire(result.authorization, registered_response->mutable_authorization());
      fence_to_wire(result.authorization.fence, response.mutable_fence());
      if (!write(*stream, response)) {
        return grpc::Status::OK;
      }
    }
    const auto delivered_instance = result.authorization.fence.instance;
    {
      std::weak_ptr<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>> weak = guard_;
      target::DeliverySink delivery_sink = [weak](const target::EventRecord& event,
                                                  const target::DeliveryAttempt& attempt,
                                                  std::string_view) -> target::Status {
        auto guard = weak.lock();
        if (!guard) {
          return target::Failure{target::ErrorCode::UpstreamUnavailable,
                                 target::Acceptance::NotAccepted,
                                 target::Recovery::Backoff,
                                 {},
                                 {}};
        }
        v1::EventEnvelope envelope;
        if (!envelope.ParseFromString(event.serialized_envelope)) {
          return target::Failure{target::ErrorCode::Internal,
                                 target::Acceptance::NotAccepted,
                                 target::Recovery::Diagnose,
                                 {},
                                 {}};
        }
        v1::CoreToPlugin frame;
        fence_to_wire(target::BridgeFence{attempt.fence.instance, attempt.fence.runtime_epoch,
                                          attempt.fence.execution_generation,
                                          attempt.fence.connection_generation},
                      frame.mutable_fence());
        frame.mutable_delivery_attempt()->CopyFrom(attempt_to_wire(attempt));
        *frame.mutable_event() = std::move(envelope);
        if (!guard->write(frame)) {
          return target::Failure{target::ErrorCode::UpstreamUnavailable,
                                 target::Acceptance::NotAccepted,
                                 target::Recovery::Backoff,
                                 {},
                                 {}};
        }
        return std::monostate{};
      };
      application_.attach_plugin_delivery(delivered_instance, stream_generation, delivery_sink);
      delivery_attached = true;
    }
    const auto shutdown_fence = state.authorization.fence;
    {
      std::weak_ptr<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>> weak = guard_;
      application_.attach_shutdown_sink(
          target::BridgePeerKind::Plugin, delivered_instance, stream_generation,
          [weak, shutdown_fence]() -> target::Status {
            auto guard = weak.lock();
            if (!guard) {
              return retryable_failure(target::ErrorCode::UpstreamUnavailable);
            }
            v1::CoreToPlugin notice;
            fence_to_wire(shutdown_fence, notice.mutable_fence());
            notice.mutable_shutdown()->set_reason("shutdown");
            notice.mutable_shutdown()->set_drain_timeout_ms(1000);
            if (!bounded_write(guard, notice, std::chrono::milliseconds{1000})) {
              return retryable_failure(target::ErrorCode::UpstreamUnavailable);
            }
            return std::monostate{};
          });
      shutdown_attached = true;
    }
    grpc::Status status = grpc::Status::OK;
    while (!context->IsCancelled()) {
      v1::PluginToCore frame;
      if (!stream->Read(&frame)) {
        break;
      }
      auto authorized =
          authorize_frame(application_, state, frame.fence(), frame.connection_proof());
      if (const auto* rejected = std::get_if<target::Failure>(&authorized)) {
        status = rejected_status(*rejected);
        break;
      }
      const auto body = frame.body_case();
      if (body == v1::PluginToCore::kHeartbeat) {
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        fill_heartbeat_ack(response.mutable_heartbeat_ack());
        state.sampled = false;
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kRefreshAuthorization) {
        auto refreshed = refresh(application_, state, frame.refresh_authorization());
        if (const auto* rejected = std::get_if<target::Failure>(&refreshed)) {
          if (rejected->code == target::ErrorCode::Unauthenticated ||
              rejected->code == target::ErrorCode::PermissionDenied) {
            status = rejected_status(*rejected);
            break;
          }
          v1::CoreToPlugin response;
          fence_to_wire(state.authorization.fence, response.mutable_fence());
          auto* result = response.mutable_authorization_result();
          result->set_request_id(frame.refresh_authorization().request_id());
          *result->mutable_error() =
              wire_error(rejected->code, frame.refresh_authorization().request_id(), {});
          state.sampled = false;
          if (!write(*stream, response)) {
            break;
          }
          status = rejected_status(*rejected);
          break;
        }
        state.authorization = std::get<target::BridgeAuthorization>(refreshed);
        state.sampled = false;
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* result = response.mutable_authorization_result();
        result->set_request_id(frame.refresh_authorization().request_id());
        authorization_to_wire(state.authorization, result->mutable_authorization());
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kEventAck) {
        auto reference = attempt_from_wire(frame.event_ack().attempt());
        if (const auto* rejected = std::get_if<target::Failure>(&reference)) {
          status = rejected_status(*rejected);
          break;
        }
        const auto& ack_wire = frame.event_ack();
        std::optional<target::Failure> failure;
        if (ack_wire.has_error()) {
          failure = failure_from_wire(ack_wire.error());
          if (!failure) {
            status = {grpc::StatusCode::INVALID_ARGUMENT, "invalid error code"};
            break;
          }
        }
        if (!ack_wire.accepted() && !failure) {
          status = {grpc::StatusCode::INVALID_ARGUMENT, "rejected ack requires an error"};
          break;
        }
        auto receipt = application_.acknowledge_delivery(
            state.authorization.fence.instance, std::get<target::BridgeAttemptRef>(reference),
            ack_wire.accepted(), failure, ack_wire.has_error() && ack_wire.error().retryable());
        if (const auto* rejected = std::get_if<target::Failure>(&receipt)) {
          status = rejected_status(*rejected);
          break;
        }
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        receipt_to_wire(std::get<target::DeliveryReceipt>(receipt),
                        response.mutable_delivery_receipt());
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kRenewDelivery) {
        auto reference = attempt_from_wire(frame.renew_delivery());
        if (const auto* rejected = std::get_if<target::Failure>(&reference)) {
          status = rejected_status(*rejected);
          break;
        }
        auto receipt = application_.renew_delivery(state.authorization.fence.instance,
                                                   std::get<target::BridgeAttemptRef>(reference));
        if (const auto* rejected = std::get_if<target::Failure>(&receipt)) {
          status = rejected_status(*rejected);
          break;
        }
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        receipt_to_wire(std::get<target::DeliveryReceipt>(receipt),
                        response.mutable_delivery_receipt());
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kRegister) {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "duplicate registration frame"};
        break;
      } else {
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        *response.mutable_error() = wire_error(target::ErrorCode::CapabilityNotSupported, {}, {});
        if (!write(*stream, response)) {
          break;
        }
      }
    }
    return status;
  }

private:
  bool write(grpc::ServerReaderWriter<v1::CoreToPlugin, v1::PluginToCore>&,
             const v1::CoreToPlugin& frame) {
    return guard_ && guard_->write(frame);
  }
  target::RuntimeBridgeApplication& application_;
  std::shared_ptr<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>> guard_;
};

target::Outcome<target::BridgeAuthorization>
refresh(target::RuntimeBridgeApplication& application, StreamState& state,
        const v1::RefreshConnectionAuthorization& wire) {
  target::BridgeRefreshRequest request;
  auto fence = fence_from_wire(wire.fence());
  if (const auto* rejected = std::get_if<target::Failure>(&fence)) {
    return *rejected;
  }
  request.fence = std::get<target::BridgeFence>(fence);
  auto proof = proof_from_wire(wire.proof());
  if (const auto* rejected = std::get_if<target::Failure>(&proof)) {
    return *rejected;
  }
  request.proof = std::get<std::array<std::byte, 32>>(proof);
  auto refresh_material = proof_from_wire(wire.refresh_credential());
  if (const auto* rejected = std::get_if<target::Failure>(&refresh_material)) {
    return *rejected;
  }
  request.refresh_credential = std::get<std::array<std::byte, 32>>(refresh_material);
  request.refresh_generation = wire.refresh_generation();
  return application.refresh(state.kind, state.authorization.fence.instance, state.authorization,
                             request);
}
} // namespace

target::Outcome<std::unique_ptr<v1::AdapterBridge::Service>>
make_adapter_bridge(target::RuntimeBridgeApplication& application) {
  try {
    return std::unique_ptr<v1::AdapterBridge::Service>(
        std::make_unique<AdapterBridgeImpl>(application));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
target::Outcome<std::unique_ptr<v1::PluginBridge::Service>>
make_plugin_bridge(target::RuntimeBridgeApplication& application) {
  try {
    return std::unique_ptr<v1::PluginBridge::Service>(
        std::make_unique<PluginBridgeImpl>(application));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
