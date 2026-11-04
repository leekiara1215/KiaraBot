#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openssl/sha.h>

#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/ports/target/runtime_model.hpp"
#include "kiarabot/core/runtime/platform/host_request_shape.hpp"
#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"
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

bool action_status_to_wire(target::ActionState state, v1::ActionStatus* wire) {
  switch (state) {
  case target::ActionState::Accepted:
    *wire = v1::ACCEPTED;
    return true;
  case target::ActionState::Succeeded:
    *wire = v1::SUCCEEDED;
    return true;
  case target::ActionState::Failed:
    *wire = v1::FAILED;
    return true;
  case target::ActionState::Expired:
    *wire = v1::EXPIRED;
    return true;
  case target::ActionState::Cancelled:
    *wire = v1::CANCELLED;
    return true;
  }
  return false;
}
bool action_outcome_to_wire(target::ExecutionOutcome outcome, v1::ExecutionOutcome* wire) {
  switch (outcome) {
  case target::ExecutionOutcome::NotStarted:
    *wire = v1::EXECUTION_NOT_STARTED;
    return true;
  case target::ExecutionOutcome::KnownSucceeded:
    *wire = v1::EXECUTION_KNOWN_SUCCEEDED;
    return true;
  case target::ExecutionOutcome::KnownFailed:
    *wire = v1::EXECUTION_KNOWN_FAILED;
    return true;
  case target::ExecutionOutcome::Unknown:
    *wire = v1::EXECUTION_UNKNOWN;
    return true;
  }
  return false;
}
bool action_outcome_from_wire(v1::ExecutionOutcome wire, target::ExecutionOutcome* outcome) {
  switch (wire) {
  case v1::EXECUTION_NOT_STARTED:
    *outcome = target::ExecutionOutcome::NotStarted;
    return true;
  case v1::EXECUTION_KNOWN_SUCCEEDED:
    *outcome = target::ExecutionOutcome::KnownSucceeded;
    return true;
  case v1::EXECUTION_KNOWN_FAILED:
    *outcome = target::ExecutionOutcome::KnownFailed;
    return true;
  case v1::EXECUTION_UNKNOWN:
    *outcome = target::ExecutionOutcome::Unknown;
    return true;
  default:
    return false;
  }
}
bool action_result_to_wire(const target::ActionRecord& record, v1::ActionResult* wire) {
  const auto snapshot = target::action_wire_snapshot(record);
  if (!snapshot) {
    return false;
  }
  wire->set_action_id(record.action_id.value);
  v1::ActionStatus status = v1::ACTION_STATUS_UNSPECIFIED;
  if (!action_status_to_wire(snapshot->first, &status)) {
    return false;
  }
  wire->set_status(status);
  v1::ExecutionOutcome outcome = v1::EXECUTION_OUTCOME_UNSPECIFIED;
  if (!action_outcome_to_wire(snapshot->second, &outcome)) {
    return false;
  }
  wire->set_execution_outcome(outcome);
  if (record.platform_message_id) {
    wire->set_platform_message_id(*record.platform_message_id);
  }
  if (record.error_code) {
    auto encoded = codec::to_wire(*record.error_code);
    if (const auto* value = std::get_if<v1::ErrorCode>(&encoded)) {
      wire->mutable_error()->set_code(*value);
    }
  }
  if (record.dispatch_attempt_id) {
    wire->set_dispatch_attempt_id(record.dispatch_attempt_id->value);
  }
  if (record.completed_at) {
    auto completed = codec::to_wire(*record.completed_at);
    if (const auto* value = std::get_if<google::protobuf::Timestamp>(&completed)) {
      wire->mutable_completed_at()->CopyFrom(*value);
    }
  }
  return true;
}
v1::ActionReceiptDisposition receipt_disposition_to_wire(target::ActionReceiptDisposition value) {
  switch (value) {
  case target::ActionReceiptDisposition::Recorded:
    return v1::ACTION_RECEIPT_RECORDED;
  case target::ActionReceiptDisposition::Duplicate:
    return v1::ACTION_RECEIPT_DUPLICATE;
  case target::ActionReceiptDisposition::EvidenceOnly:
    return v1::ACTION_RECEIPT_EVIDENCE_ONLY;
  case target::ActionReceiptDisposition::Conflict:
    return v1::ACTION_RECEIPT_CONFLICT;
  }
  return v1::ACTION_RECEIPT_DISPOSITION_UNSPECIFIED;
}
bool decode_plugin_action(const v1::ActionEnvelope& wire, const std::string& serialized,
                          target::BridgeActionSubmit* out) {
  if (wire.action_id().empty() || wire.idempotency_key().empty() ||
      wire.dispatch_attempt_id().size() != 0 || wire.dispatch_sequence() != 0 ||
      wire.target_source().adapter_id().empty() || wire.target_source().account_id().empty() ||
      !wire.has_deadline()) {
    return false;
  }
  auto deadline = codec::from_wire(wire.deadline());
  if (!std::holds_alternative<target::TimePoint>(deadline)) {
    return false;
  }
  std::string conversation = wire.target().conversation_id();
  uint8_t conversation_kind = 2;
  switch (wire.payload_case()) {
  case v1::ActionEnvelope::kSendMessage: {
    const auto& payload = wire.send_message();
    if (conversation.empty()) {
      conversation = payload.conversation().id();
    }
    if (payload.conversation().type() == v1::ConversationRef::GROUP) {
      conversation_kind = 1;
    } else if (payload.conversation().type() == v1::ConversationRef::PRIVATE) {
      conversation_kind = 0;
    }
    out->kind = target::ActionKind::SendMessage;
    break;
  }
  case v1::ActionEnvelope::kReactMessage: {
    const auto& payload = wire.react_message();
    if (conversation.empty()) {
      conversation = payload.conversation().id();
    }
    if (payload.conversation().type() == v1::ConversationRef::GROUP) {
      conversation_kind = 1;
    } else if (payload.conversation().type() == v1::ConversationRef::PRIVATE) {
      conversation_kind = 0;
    }
    out->kind = target::ActionKind::ReactMessage;
    break;
  }
  case v1::ActionEnvelope::kDeleteMessage: {
    const auto& payload = wire.delete_message();
    if (conversation.empty()) {
      conversation = payload.conversation().id();
    }
    if (payload.conversation().type() == v1::ConversationRef::GROUP) {
      conversation_kind = 1;
    } else if (payload.conversation().type() == v1::ConversationRef::PRIVATE) {
      conversation_kind = 0;
    }
    out->kind = target::ActionKind::DeleteMessage;
    break;
  }
  case v1::ActionEnvelope::kMuteMember:
    conversation = wire.mute_member().group_id();
    conversation_kind = 1;
    out->kind = target::ActionKind::MuteMember;
    break;
  case v1::ActionEnvelope::kKickMember:
    conversation = wire.kick_member().group_id();
    conversation_kind = 1;
    out->kind = target::ActionKind::KickMember;
    break;
  case v1::ActionEnvelope::kApproveRequest:
    out->kind = target::ActionKind::ApproveRequest;
    break;
  default:
    return false;
  }
  out->request_id = wire.action_id();
  out->trace_id = wire.trace_id().empty() ? wire.action_id() : wire.trace_id();
  out->action_id = target::ActionId{wire.action_id()};
  out->account_id = target::ResourceId{wire.target_source().account_id()};
  out->adapter_id = target::ResourceId{wire.target_source().adapter_id()};
  out->idempotency_key = wire.idempotency_key();
  out->serialized_envelope = serialized;
  out->target_conversation = std::move(conversation);
  out->target_conversation_kind = conversation_kind;
  out->deadline = std::get<target::TimePoint>(deadline);
  return true;
}

// Shared post-registration loop state for one accepted stream.
class PluginStreamHub;

struct StreamState {
  target::BridgePeerKind kind = target::BridgePeerKind::Adapter;
  target::BridgeAuthorization authorization;
  std::string adapter_id;
  target::TimePoint sampled_at;
  bool sampled = false;
  // Negotiated registration grants; plugin host frames are only served when the exact permission
  // was granted at registration. Empty for adapters.
  std::vector<std::string> permissions;
  std::vector<std::string> features;
  std::shared_ptr<PluginStreamHub> streams;
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
    const auto stream_guard =
        std::make_shared<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>>(stream, context);
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
    StreamState state{target::BridgePeerKind::Adapter,
                      result.authorization,
                      command.adapter.value,
                      {},
                      false,
                      {},
                      {},
                      nullptr};
    const auto stream_instance = result.authorization.fence.instance;
    const auto stream_generation = result.authorization.fence.connection_generation;
    bool shutdown_attached = false;
    bool action_attached = false;
    struct Exit {
      target::RuntimeBridgeApplication& application;
      std::shared_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> guard;
      target::InstanceId instance;
      target::ConnectionGeneration generation;
      bool* shutdown_attached;
      bool* action_attached;
      ~Exit() {
        if (*action_attached) {
          application.detach_adapter_action_sink(instance, generation);
        }
        if (*shutdown_attached) {
          application.detach_shutdown_sink(target::BridgePeerKind::Adapter, instance, generation);
        }
        guard->close();
        application.disconnect(target::BridgePeerKind::Adapter, instance, generation);
      }
    } exit_guard{application_,      stream_guard,       stream_instance,
                 stream_generation, &shutdown_attached, &action_attached};
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
      if (!stream_guard->write(response)) {
        return grpc::Status::OK;
      }
    }
    const auto shutdown_fence = state.authorization.fence;
    {
      std::weak_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> weak = stream_guard;
      const auto attached = application_.attach_shutdown_sink(
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
      if (const auto* rejected = std::get_if<target::Failure>(&attached)) {
        return rejected_status(*rejected);
      }
      shutdown_attached = true;
    }
    {
      std::weak_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> weak = stream_guard;
      const auto dispatch_fence = state.authorization.fence;
      auto attached = application_.attach_adapter_action_sink(
          stream_instance, stream_generation,
          [weak, dispatch_fence](const target::ActionDispatchFrame& frame) -> target::Status {
            auto guard = weak.lock();
            if (!guard) {
              return retryable_failure(target::ErrorCode::UpstreamUnavailable);
            }
            v1::ActionEnvelope envelope;
            if (!envelope.ParseFromString(frame.serialized_envelope)) {
              return failure(target::ErrorCode::Internal);
            }
            envelope.set_dispatch_attempt_id(frame.dispatch_attempt_id.value);
            envelope.set_dispatch_sequence(frame.dispatch_sequence);
            v1::CoreToAdapter response;
            fence_to_wire(dispatch_fence, response.mutable_fence());
            *response.mutable_action() = std::move(envelope);
            if (!guard->write(response)) {
              return retryable_failure(target::ErrorCode::UpstreamUnavailable);
            }
            return std::monostate{};
          });
      if (const auto* rejected = std::get_if<target::Failure>(&attached)) {
        return rejected_status(*rejected);
      }
      action_attached = true;
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
        if (!stream_guard->write(response)) {
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
          if (!stream_guard->write(response)) {
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
        if (!stream_guard->write(response)) {
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
          if (!stream_guard->write(response)) {
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
        if (!stream_guard->write(response)) {
          break;
        }
        if (ack_result.close_stream) {
          return grpc::Status::OK;
        }
      } else if (body == v1::AdapterToCore::kActionResult) {
        const auto& result = frame.action_result();
        auto dispatch_attempt_id =
            result.dispatch_attempt_id().empty()
                ? std::optional<target::AttemptId>{}
                : std::optional<target::AttemptId>{target::AttemptId{result.dispatch_attempt_id()}};
        target::BridgeActionReport report;
        report.action_id = target::ActionId{result.action_id()};
        report.dispatch_attempt_id = dispatch_attempt_id;
        report.trace_id = result.action_id();
        if (result.status() == v1::SUCCEEDED) {
          report.result.state = target::ActionState::Succeeded;
          report.result.execution_outcome = target::ExecutionOutcome::KnownSucceeded;
        } else if (result.status() == v1::FAILED) {
          report.result.state = target::ActionState::Failed;
          report.result.execution_outcome = target::ExecutionOutcome::KnownFailed;
          if (!result.has_error()) {
            status = {grpc::StatusCode::INVALID_ARGUMENT, "failed result requires an error"};
            break;
          }
          auto code = codec::from_wire(result.error().code());
          if (!std::holds_alternative<target::ErrorCode>(code)) {
            status = {grpc::StatusCode::INVALID_ARGUMENT, "invalid result error code"};
            break;
          }
          report.result.error_code = std::get<target::ErrorCode>(code);
        } else {
          status = {grpc::StatusCode::INVALID_ARGUMENT, "adapter result must be terminal"};
          break;
        }
        if (!result.platform_message_id().empty()) {
          report.result.platform_message_id = result.platform_message_id();
        }
        auto recorded =
            application_.record_adapter_result(state.authorization.fence.instance, report);
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* receipt = response.mutable_action_receipt();
        receipt->set_action_id(result.action_id());
        if (!result.dispatch_attempt_id().empty()) {
          receipt->set_dispatch_attempt_id(result.dispatch_attempt_id());
        }
        if (const auto* rejected = std::get_if<target::Failure>(&recorded)) {
          receipt->set_disposition(v1::ACTION_RECEIPT_CONFLICT);
          *receipt->mutable_error() = wire_error(rejected->code, result.action_id(), {});
        } else {
          const auto& accepted = std::get<target::ActionReportReceipt>(recorded);
          receipt->set_disposition(receipt_disposition_to_wire(accepted.disposition));
          if (accepted.record) {
            (void)action_result_to_wire(*accepted.record, receipt->mutable_result());
          }
          if (accepted.failure) {
            *receipt->mutable_error() = wire_error(accepted.failure->code, result.action_id(), {});
          }
        }
        if (!stream_guard->write(response)) {
          break;
        }
      } else if (body == v1::AdapterToCore::kActionEvidence) {
        const auto& evidence = frame.action_evidence();
        target::BridgeEvidenceReport report;
        report.action_id = target::ActionId{evidence.action_id()};
        report.dispatch_attempt_id = target::AttemptId{evidence.dispatch_attempt_id()};
        report.trace_id = evidence.action_id();
        if (!action_outcome_from_wire(evidence.execution_outcome(), &report.execution_outcome)) {
          status = {grpc::StatusCode::INVALID_ARGUMENT, "invalid evidence outcome"};
          break;
        }
        if (evidence.has_last_error()) {
          auto code = codec::from_wire(evidence.last_error().code());
          if (!std::holds_alternative<target::ErrorCode>(code)) {
            status = {grpc::StatusCode::INVALID_ARGUMENT, "invalid evidence error code"};
            break;
          }
          report.error_code = std::get<target::ErrorCode>(code);
        }
        if (!evidence.platform_reference().empty()) {
          report.platform_reference = evidence.platform_reference();
        }
        if (evidence.has_observed_at()) {
          auto observed = codec::from_wire(evidence.observed_at());
          if (std::holds_alternative<target::TimePoint>(observed)) {
            report.observed_at = std::get<target::TimePoint>(observed);
          }
        }
        if (report.observed_at.time_since_epoch().count() <= 0) {
          status = {grpc::StatusCode::INVALID_ARGUMENT, "evidence observed_at required"};
          break;
        }
        auto recorded =
            application_.record_adapter_evidence(state.authorization.fence.instance, report);
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* receipt = response.mutable_action_receipt();
        receipt->set_action_id(evidence.action_id());
        receipt->set_dispatch_attempt_id(evidence.dispatch_attempt_id());
        if (const auto* rejected = std::get_if<target::Failure>(&recorded)) {
          receipt->set_disposition(v1::ACTION_RECEIPT_CONFLICT);
          *receipt->mutable_error() = wire_error(rejected->code, evidence.action_id(), {});
        } else {
          const auto& accepted = std::get<target::ActionReportReceipt>(recorded);
          receipt->set_disposition(receipt_disposition_to_wire(accepted.disposition));
          if (accepted.record) {
            (void)action_result_to_wire(*accepted.record, receipt->mutable_result());
          }
        }
        if (!stream_guard->write(response)) {
          break;
        }
      } else {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "unsupported bridge frame"};
        break;
      }
    }
    return status;
  }

private:
  target::RuntimeBridgeApplication& application_;
};

bool bridge_uuid(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
      value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      continue;
    }
    const char character = value[index];
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
bool time_to_wire(target::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *timestamp;
    return true;
  }
  return false;
}
// P09b: plugin bridge storage frame conversion. All namespace/owner facts are derived from the
// authenticated registered instance/profile; the plugin frame only supplies key/value/condition/
// TTL. Any absent/unknown case fails closed before the application call.
std::optional<target::CompareCondition>
storage_condition_from_wire(const v1::StorageCondition& wire) {
  switch (wire.expected_case()) {
  case v1::StorageCondition::EXPECTED_NOT_SET:
    return std::nullopt;
  case v1::StorageCondition::kAbsent:
    if (!wire.absent()) {
      return std::nullopt;
    }
    return target::CompareCondition{target::ExpectedAbsent{}};
  case v1::StorageCondition::kVersion:
    if (wire.version() == 0) {
      return std::nullopt;
    }
    return target::CompareCondition{target::ResourceVersion{wire.version()}};
  }
  return std::nullopt;
}
target::Sha256Digest storage_fingerprint(const google::protobuf::Message& operation) {
  std::string serialized;
  (void)operation.SerializeToString(&serialized);
  target::Sha256Digest digest;
  unsigned char output[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(serialized.data()), serialized.size(), output);
  static_assert(sizeof(digest.bytes) == SHA256_DIGEST_LENGTH);
  std::memcpy(digest.bytes.data(), output, sizeof(digest.bytes));
  return digest;
}
bool storage_entry_to_wire(const target::NamespaceValue& value, const std::string& key,
                           v1::StorageEntry* output) {
  if (!output) {
    return false;
  }
  output->set_key(key);
  output->set_value(value.bytes.data(), value.bytes.size());
  output->set_version(value.version.value);
  if (value.expires_at) {
    return time_to_wire(*value.expires_at, output->mutable_expires_at());
  }
  return true;
}

using PluginStreamGuard = StreamGuard<v1::CoreToPlugin, v1::PluginToCore>;

// A frame may either continue the read loop, stop after a failed write (OK), or terminate with a
// transport rejection. Business Host/Action failures stay response frames on the same connection.
struct PluginFrameResult {
  bool read_next = true;
  grpc::Status status = grpc::Status::OK;
};

class PluginHostReply {
public:
  PluginHostReply(const target::BridgeFence& fence, const v1::HostRequest& request,
                  const std::shared_ptr<PluginStreamGuard>& guard)
      : request_(request), guard_(guard) {
    fence_to_wire(fence, frame_.mutable_fence());
    body()->set_request_id(request.request_id());
  }
  v1::HostResponse* body() {
    return frame_.mutable_host_response();
  }
  bool write() {
    return guard_->write(frame_);
  }
  bool reject(target::ErrorCode code, bool retryable = false) {
    *body()->mutable_error() =
        wire_error(code, request_.request_id(), request_.trace_id(), retryable);
    return write();
  }

private:
  const v1::HostRequest& request_;
  const std::shared_ptr<PluginStreamGuard>& guard_;
  v1::CoreToPlugin frame_;
};

// ---------------------------------------------------------------------------
// STR07: Runtime -> plugin Model streaming sessions.
//
// One session owns a strict Model-stream consumer (producer) and a serialized writer. The producer
// never touches the gRPC stream: it only enqueues bounded wire frames; the writer performs the
// bounded network writes (one-second budget) so a slow plugin consumer cancels the attempt instead
// of blocking the reader/heartbeat/refresh/cancel loop. Admission (feature/permission/profile) is
// done by the transport before start; this hub only owns limits and lifetime.
class PluginStreamHub {
public:
  PluginStreamHub(std::weak_ptr<PluginStreamGuard> guard, target::RuntimeModelPort& model,
                  std::atomic<std::size_t>& process_active, target::BridgeFence fence)
      : guard_(std::move(guard)), model_(model), process_active_(process_active),
        fence_(std::make_shared<target::BridgeFence>(std::move(fence))) {}
  ~PluginStreamHub() {
    shutdown();
  }
  PluginStreamHub(const PluginStreamHub&) = delete;
  PluginStreamHub& operator=(const PluginStreamHub&) = delete;

  void set_fence(const target::BridgeFence& fence) {
    std::lock_guard lock(hub_mutex_);
    *fence_ = fence;
  }
  // Refresh/rotate invalidates the previous connection delegation: every in-flight stream is
  // cancelled and reports its terminal under the current fence.
  void cancel_all() {
    std::lock_guard lock(hub_mutex_);
    for (const auto& [id, session] : sessions_) {
      (void)id;
      session->cancel.request_stop();
      session->changed.notify_all();
    }
  }
  void reap() {
    std::vector<std::shared_ptr<Session>> finished;
    {
      std::lock_guard lock(hub_mutex_);
      for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        const auto& session = iterator->second;
        std::lock_guard session_lock{session->mutex};
        if (session->producer_done && session->writer_done) {
          finished_ids_.emplace_back(session->start_request_id, session->logical_call_id);
          if (finished_ids_.size() > 64) {
            finished_ids_.pop_front();
          }
          finished.push_back(session);
          iterator = sessions_.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
    // Join outside the hub lock: a finishing thread may be inside write_event() waiting for that
    // same lock. Sessions are only erased after both workers have finished.
    for (auto& session : finished) {
      join_session(session);
    }
  }
  void shutdown() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
      std::lock_guard lock(hub_mutex_);
      for (auto& [id, session] : sessions_) {
        (void)id;
        session->cancel.request_stop();
        {
          std::lock_guard session_lock{session->mutex};
          session->hard_stop = true;
        }
        session->changed.notify_all();
        sessions.push_back(session);
      }
      sessions_.clear();
    }
    for (auto& session : sessions) {
      join_session(session);
    }
  }

  target::Status start(const v1::HostRequest& host, const v1::ModelStreamStartRequest& start,
                       const target::BridgeFence& fence, target::TimePoint deadline,
                       const target::InstallationId& installation) {
    reap();
    {
      std::lock_guard lock(hub_mutex_);
      if (sessions_.size() >= target::kModelStreamMaximumConnectionCalls) {
        return target::Failure{target::ErrorCode::RateLimited,
                               target::Acceptance::NotAccepted,
                               target::Recovery::Backoff,
                               {},
                               {}};
      }
      if (sessions_.find(host.request_id()) != sessions_.end()) {
        return target::Failure{target::ErrorCode::InvalidArgument,
                               target::Acceptance::NotAccepted,
                               target::Recovery::CorrectInput,
                               {},
                               {}};
      }
      auto active = process_active_.load();
      if (active >= target::kModelStreamMaximumProcessCalls) {
        return target::Failure{target::ErrorCode::RateLimited,
                               target::Acceptance::NotAccepted,
                               target::Recovery::Backoff,
                               {},
                               {}};
      }
      auto session =
          std::make_shared<Session>(host.request_id(), host.trace_id(), start.logical_call_id(),
                                    fence.instance.value, deadline, &process_active_);
      sessions_.emplace(session->start_request_id, session);
      target::RuntimeModelStreamRequest request;
      request.instance = fence.instance;
      // The sealed model parent envelope binds the registered installation; the stream start must
      // carry it exactly like unary submit_plugin_model or delegation acquisition fails closed.
      request.installation = installation;
      request.request_id = host.request_id();
      request.trace_id = host.trace_id();
      request.logical_call_id = start.logical_call_id();
      request.model = start.model();
      request.messages.reserve(static_cast<std::size_t>(start.messages_size()));
      for (const auto& message : start.messages()) {
        request.messages.push_back({message.role(), message.text()});
      }
      std::weak_ptr<Session> weak = session;
      session->writer = std::jthread([this, weak] { drain(weak); });
      session->producer =
          std::jthread([this, weak, request = std::move(request)] { produce(weak, request); });
    }
    return std::monostate{};
  }

  v1::ModelStreamCancelDisposition cancel(std::string_view start_request_id,
                                          std::string_view logical_call_id) {
    std::lock_guard lock(hub_mutex_);
    const auto found = sessions_.find(std::string{start_request_id});
    if (found == sessions_.end()) {
      const auto receipt = std::pair<std::string, std::string>{std::string{start_request_id},
                                                               std::string{logical_call_id}};
      for (const auto& finished : finished_ids_) {
        if (finished == receipt) {
          return v1::MODEL_STREAM_ALREADY_TERMINAL;
        }
      }
      return v1::MODEL_STREAM_NOT_ACTIVE;
    }
    // Only the owning connection's plugin instance may address this attempt; another plugin's
    // identical ids never reach this hub, and a rotated connection stays NOT_ACTIVE here.
    if (found->second->logical_call_id != logical_call_id ||
        found->second->instance != fence_->instance.value) {
      return v1::MODEL_STREAM_NOT_ACTIVE;
    }
    const auto& session = found->second;
    {
      std::lock_guard session_lock{session->mutex};
      // The terminal may already be produced and in flight; a cancel arriving after that point is
      // reported as ALREADY_TERMINAL and never requests a second termination.
      if (session->terminal_queued || session->terminal_written ||
          (session->producer_done && !session->terminal_queued)) {
        return v1::MODEL_STREAM_ALREADY_TERMINAL;
      }
    }
    // Duplicate cancellation is harmless and returns the same local receipt.
    session->cancel.request_stop();
    session->changed.notify_all();
    return v1::MODEL_STREAM_CANCELLATION_REQUESTED;
  }

private:
  struct Session {
    Session(std::string id, std::string trace, std::string logical, std::string owning_instance,
            target::TimePoint deadline_at, std::atomic<std::size_t>* process)
        : start_request_id(std::move(id)), trace_id(std::move(trace)),
          logical_call_id(std::move(logical)), instance(std::move(owning_instance)),
          deadline(deadline_at), process_active(process) {
      if (process_active) {
        process_active->fetch_add(1);
      }
    }
    ~Session() {
      // The hub joins both workers before erasing a session, so no join may happen here.
      if (process_active) {
        process_active->fetch_sub(1);
      }
    }
    std::string start_request_id;
    std::string trace_id;
    std::string logical_call_id;
    std::string instance;
    target::TimePoint deadline;
    std::atomic<std::size_t>* process_active = nullptr;
    std::stop_source cancel;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<v1::ModelStreamEvent> queue;
    std::size_t queued_bytes = 0;
    bool producer_done = false;
    bool writer_done = false;
    bool terminal_queued = false;
    bool terminal_written = false;
    bool hard_stop = false;
    bool started_delivered = false;
    std::uint64_t delivered_bytes = 0;
    std::uint64_t delivered_deltas = 0;
    std::jthread producer;
    std::jthread writer;
  };
  class Sink final : public target::RuntimeModelStreamSink {
  public:
    explicit Sink(std::weak_ptr<Session> session) : session_(std::move(session)) {}
    target::ModelStreamSinkResult try_emit(const target::ModelStreamEvent& event) override {
      const auto session = session_.lock();
      if (!session || session->cancel.stop_requested()) {
        // The terminal control frame is still allowed after cancellation so the attempt has one
        // authoritative local end; deltas are never appended after a stop request.
        if (!std::holds_alternative<target::ModelStreamTerminal>(event.body)) {
          return target::ModelStreamSinkResult::Cancelled;
        }
      }
      auto encoded = encode_model_stream_event(event);
      if (!std::holds_alternative<v1::ModelStreamEvent>(encoded)) {
        return target::ModelStreamSinkResult::Cancelled;
      }
      auto wire = std::get<v1::ModelStreamEvent>(std::move(encoded));
      const bool terminal = wire.has_terminal();
      std::lock_guard lock{session->mutex};
      const auto* delta = std::get_if<target::ModelStreamDelta>(&event.body);
      const auto bytes = delta ? delta->text.size() : 0;
      if (!terminal && (session->queue.size() >= target::kModelStreamMaximumQueuedFrames ||
                        session->queued_bytes + bytes > target::kModelStreamMaximumQueuedBytes)) {
        return target::ModelStreamSinkResult::Full;
      }
      std::fprintf(stderr, "[hub-sink] seq=%llu terminal=%d\n",
                   static_cast<unsigned long long>(event.sequence), static_cast<int>(terminal));
      if (terminal) {
        session->terminal_queued = true;
      } else {
        session->queue.push_back(std::move(wire));
        session->queued_bytes += bytes;
        if (event.sequence == 0) {
          session->started_delivered = true;
        }
        session->delivered_bytes += bytes;
        if (delta) {
          ++session->delivered_deltas;
        }
      }
      session->changed.notify_all();
      return target::ModelStreamSinkResult::Accepted;
    }

  private:
    std::weak_ptr<Session> session_;
  };
  bool write_event(const v1::ModelStreamEvent& event) {
    auto guard = guard_.lock();
    if (!guard) {
      return false;
    }
    v1::CoreToPlugin frame;
    {
      std::lock_guard lock(hub_mutex_);
      fence_to_wire(*fence_, frame.mutable_fence());
    }
    *frame.mutable_model_stream_event() = event;
    return bounded_write(guard, frame, std::chrono::milliseconds{1000});
  }
  bool write_host_error(std::string_view request_id, std::string_view trace_id,
                        const target::Failure& failure) {
    auto guard = guard_.lock();
    if (!guard) {
      return false;
    }
    v1::CoreToPlugin frame;
    {
      std::lock_guard lock(hub_mutex_);
      fence_to_wire(*fence_, frame.mutable_fence());
    }
    auto* response = frame.mutable_host_response();
    response->set_request_id(std::string{request_id});
    *response->mutable_error() = wire_error(failure.code, request_id, trace_id,
                                            failure.recovery == target::Recovery::Backoff ||
                                                failure.code == target::ErrorCode::RateLimited);
    return bounded_write(guard, frame, std::chrono::milliseconds{1000});
  }
  static void join_session(const std::shared_ptr<Session>& session) {
    if (session->producer.joinable()) {
      session->producer.join();
    }
    if (session->writer.joinable()) {
      session->writer.join();
    }
  }
  void finish_writer(const std::shared_ptr<Session>& session) {
    {
      std::lock_guard lock{session->mutex};
      session->writer_done = true;
    }
    session->changed.notify_all();
  }
  void drain(std::weak_ptr<Session> weak) {
    auto session = weak.lock();
    if (!session) {
      return;
    }
    for (;;) {
      std::optional<v1::ModelStreamEvent> next;
      bool finished = false;
      {
        std::unique_lock lock{session->mutex};
        session->changed.wait_for(lock, std::chrono::milliseconds{10});
        if (session->hard_stop) {
          break;
        }
        if (session->cancel.stop_requested() && !session->terminal_queued) {
          session->queue.clear();
          session->queued_bytes = 0;
        }
        if (!session->queue.empty()) {
          next = std::move(session->queue.front());
          session->queue.pop_front();
          session->queued_bytes -= next->has_delta() ? next->delta().text().size() : 0;
        } else if (session->producer_done) {
          finished = true;
        }
      }
      if (finished) {
        break;
      }
      if (!next) {
        continue;
      }
      const bool terminal = next->has_terminal();
      std::fprintf(stderr, "[hub-write] seq=%llu terminal=%d\n",
                   static_cast<unsigned long long>(next->sequence()), static_cast<int>(terminal));
      if (!write_event(*next)) {
        session->cancel.request_stop();
        break;
      }
      if (terminal) {
        {
          std::lock_guard lock{session->mutex};
          session->terminal_written = true;
        }
        break;
      }
    }
    finish_writer(session);
  }
  bool enqueue_terminal(const std::shared_ptr<Session>& session,
                        const target::ModelStreamTerminal& terminal) {
    target::ModelStreamEvent event{session->start_request_id, session->logical_call_id, 0,
                                   session->trace_id, terminal};
    {
      std::lock_guard lock{session->mutex};
      event.sequence = session->started_delivered ? session->delivered_deltas + 1 : 0;
    }
    auto encoded = encode_model_stream_event(event);
    if (!std::holds_alternative<v1::ModelStreamEvent>(encoded)) {
      return false;
    }
    std::lock_guard lock{session->mutex};
    session->queue.push_back(std::get<v1::ModelStreamEvent>(std::move(encoded)));
    session->terminal_queued = true;
    session->changed.notify_all();
    return true;
  }
  bool enqueue_cancelled(const std::shared_ptr<Session>& session) {
    target::ModelStreamTerminal terminal;
    terminal.termination = target::ModelStreamTermination::Cancelled;
    {
      std::lock_guard lock{session->mutex};
      terminal.emitted_bytes = session->delivered_bytes;
      terminal.emitted_delta_count = session->delivered_deltas;
    }
    terminal.partial = terminal.emitted_bytes != 0;
    terminal.usage = {};
    terminal.may_have_billed = true;
    return enqueue_terminal(session, terminal);
  }
  void produce(std::weak_ptr<Session> weak, target::RuntimeModelStreamRequest request) {
    auto session = weak.lock();
    if (!session) {
      return;
    }
    Sink sink{weak};
    auto result = model_.chat_stream(request, sink, session->cancel.get_token(), session->deadline);
    bool unknown = false;
    bool pre_start = false;
    if (const auto* failure = std::get_if<target::Failure>(&result)) {
      {
        std::lock_guard lock{session->mutex};
        pre_start = !session->started_delivered;
      }
      if (pre_start) {
        (void)write_host_error(session->start_request_id, session->trace_id, *failure);
      } else {
        unknown = true;
      }
    } else {
      const auto& value = std::get<target::RuntimeModelStreamResult>(result);
      if (value.terminal) {
        unknown = !enqueue_terminal(session, *value.terminal);
      } else if (value.cancelled_by_consumer) {
        unknown = !enqueue_cancelled(session);
      } else {
        unknown = true;
      }
    }
    {
      std::lock_guard lock{session->mutex};
      session->producer_done = true;
    }
    session->changed.notify_all();
    if (unknown) {
      // No terminal will ever be produced for this attempt: terminate the bridge connection so the
      // plugin treats every in-flight delivery as unknown instead of waiting forever.
      if (auto guard = guard_.lock()) {
        if (guard->context) {
          guard->context->TryCancel();
        }
      }
    }
  }

  std::weak_ptr<PluginStreamGuard> guard_;
  target::RuntimeModelPort& model_;
  std::atomic<std::size_t>& process_active_;
  std::mutex hub_mutex_;
  std::shared_ptr<target::BridgeFence> fence_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
  std::deque<std::pair<std::string, std::string>> finished_ids_;
};

target::BridgePluginRegistration
plugin_registration_from_wire(const v1::RegisterPluginRequest& registered,
                              const std::string& bearer) {
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
  return command;
}

target::Outcome<target::TaskCreationInput>
task_creation_from_wire(const v1::TaskCreateRequest& request) {
  target::TaskCreationInput input;
  input.task_id = request.task_id();
  input.target = target::ConversationKey{
      {}, {}, request.target_conversation(), target::ConversationKey::Kind::Group};
  const auto& payload = request.payload();
  input.payload.resize(static_cast<std::size_t>(payload.size()));
  std::memcpy(input.payload.data(), payload.data(), input.payload.size());
  if (request.when_case() == v1::TaskCreateRequest::kRunAt) {
    const auto decoded = codec::from_wire(request.run_at());
    const auto* value = std::get_if<target::TimePoint>(&decoded);
    if (!value) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    input.run_at = *value;
  } else if (request.when_case() == v1::TaskCreateRequest::kLocalTime) {
    const auto& local = request.local_time();
    input.local_time =
        target::LocalCivilTime{local.year(),   local.month(),  local.day(),      local.hour(),
                               local.minute(), local.second(), local.time_zone()};
  } else {
    return failure(target::ErrorCode::InvalidArgument);
  }
  return input;
}

// Owns exactly one accepted connection. Delivery and shutdown callbacks hold weak guards; cleanup
// detaches callbacks, closes the guard, then disconnects the matching generation in that order.
class PluginConnection {
public:
  PluginConnection(target::RuntimeBridgeApplication& application,
                   std::shared_ptr<PluginStreamGuard> guard, target::BridgeFence fence)
      : application_(application), guard_(std::move(guard)), fence_(std::move(fence)) {}
  PluginConnection(const PluginConnection&) = delete;
  PluginConnection& operator=(const PluginConnection&) = delete;
  ~PluginConnection() {
    if (delivery_attached_) {
      application_.detach_plugin_delivery(fence_.instance, fence_.connection_generation);
    }
    if (shutdown_attached_) {
      application_.detach_shutdown_sink(target::BridgePeerKind::Plugin, fence_.instance,
                                        fence_.connection_generation);
    }
    guard_->close();
    application_.disconnect(target::BridgePeerKind::Plugin, fence_.instance,
                            fence_.connection_generation);
  }
  target::Status attach_delivery() {
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
    const auto attached = application_.attach_plugin_delivery(
        fence_.instance, fence_.connection_generation, delivery_sink);
    if (const auto* rejected = std::get_if<target::Failure>(&attached)) {
      return *rejected;
    }
    delivery_attached_ = true;
    return std::monostate{};
  }
  target::Status attach_shutdown() {
    const auto shutdown_fence = fence_;
    std::weak_ptr<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>> weak = guard_;
    const auto attached = application_.attach_shutdown_sink(
        target::BridgePeerKind::Plugin, fence_.instance, fence_.connection_generation,
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
    if (const auto* rejected = std::get_if<target::Failure>(&attached)) {
      return *rejected;
    }
    shutdown_attached_ = true;
    return std::monostate{};
  }

private:
  target::RuntimeBridgeApplication& application_;
  std::shared_ptr<PluginStreamGuard> guard_;
  target::BridgeFence fence_;
  bool delivery_attached_ = false;
  bool shutdown_attached_ = false;
};

struct StorageWireOperation {
  const v1::NamespaceScope* scope;
  const google::protobuf::Message* message;
  std::string method;
  bool mutating;
};

target::Outcome<StorageWireOperation> storage_operation_from_wire(const v1::HostRequest& host) {
  const v1::NamespaceScope* wire_scope = nullptr;
  const google::protobuf::Message* operation = nullptr;
  std::string method;
  bool mutating = true;
  switch (host.operation_case()) {
  case v1::HostRequest::kStorageGet:
    wire_scope = host.storage_get().has_scope() ? &host.storage_get().scope() : nullptr;
    operation = &host.storage_get();
    method = "plugin.storage.get";
    mutating = false;
    break;
  case v1::HostRequest::kStorageSet:
    wire_scope = host.storage_set().has_scope() ? &host.storage_set().scope() : nullptr;
    operation = &host.storage_set();
    method = "plugin.storage.set";
    break;
  case v1::HostRequest::kStorageDelete:
    wire_scope = host.storage_delete().has_scope() ? &host.storage_delete().scope() : nullptr;
    operation = &host.storage_delete();
    method = "plugin.storage.delete";
    break;
  case v1::HostRequest::kStorageCompareAndSet:
    wire_scope = host.storage_compare_and_set().has_scope()
                     ? &host.storage_compare_and_set().scope()
                     : nullptr;
    operation = &host.storage_compare_and_set();
    method = "plugin.storage.compare_and_set";
    break;
  default:
    return failure(target::ErrorCode::CapabilityNotSupported);
  }
  return StorageWireOperation{wire_scope, operation, std::move(method), mutating};
}

target::Outcome<target::RequestedScopeKind>
storage_scope_from_wire(const v1::NamespaceScope* wire_scope, const target::InstanceId& instance,
                        const target::PluginCapabilityProfile& profile) {
  auto requested = target::RequestedScopeKind::DefaultPrivate;
  if (wire_scope != nullptr) {
    switch (wire_scope->scope_case()) {
    case v1::NamespaceScope::SCOPE_NOT_SET:
    case v1::NamespaceScope::kInstancePrivate:
      if (wire_scope->scope_case() == v1::NamespaceScope::kInstancePrivate &&
          !wire_scope->instance_private()) {
        return failure(target::ErrorCode::InvalidArgument);
      }
      break;
    case v1::NamespaceScope::kInstallationShared:
      if (const auto resolved = target::plugin_storage_resource(
              instance, profile, target::RequestedScopeKind::InstallationShared);
          std::holds_alternative<target::Failure>(resolved)) {
        return std::get<target::Failure>(resolved);
      }
      if (!wire_scope->installation_shared()) {
        return failure(target::ErrorCode::InvalidArgument);
      }
      requested = target::RequestedScopeKind::InstallationShared;
      break;
    case v1::NamespaceScope::kInstallationSharedId:
      // Plugin frames never self-report a namespace id; it is derived from the registered
      // installation. Only Runtime internal dispatcher frames may use the explicit id.
      return failure(target::ErrorCode::CapabilityNotSupported);
    }
  }
  return requested;
}

class PluginBridgeImpl final : public v1::PluginBridge::Service {
public:
  PluginBridgeImpl(target::RuntimeBridgeApplication& application, PluginBridgeOptions options)
      : application_(application), options_(std::move(options)) {}

  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<v1::CoreToPlugin, v1::PluginToCore>* stream) override {
    FirstFrameWatchdog watchdog(context);
    const auto stream_guard =
        std::make_shared<StreamGuard<v1::CoreToPlugin, v1::PluginToCore>>(stream, context);
    std::string bearer;
    if (!bearer_metadata(*context, bearer)) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bridge authorization missing"};
    }
    v1::PluginToCore first;
    if (!stream->Read(&first) || first.body_case() != v1::PluginToCore::kRegister) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "registration frame required"};
    }
    watchdog.finish();
    const auto command = plugin_registration_from_wire(first.register_(), bearer);
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
    StreamState state{target::BridgePeerKind::Plugin, result.authorization,    {},     {}, false,
                      result.granted_permissions,     result.enabled_features, nullptr};
    if (options_.model) {
      state.streams = std::make_shared<PluginStreamHub>(
          std::weak_ptr<PluginStreamGuard>{stream_guard}, *options_.model, plugin_streams_,
          state.authorization.fence);
    }
    PluginConnection connection{application_, stream_guard, state.authorization.fence};
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
      if (!stream_guard->write(response)) {
        return grpc::Status::OK;
      }
    }
    if (const auto attached = connection.attach_delivery();
        std::holds_alternative<target::Failure>(attached)) {
      return rejected_status(std::get<target::Failure>(attached));
    }
    if (const auto attached = connection.attach_shutdown();
        std::holds_alternative<target::Failure>(attached)) {
      return rejected_status(std::get<target::Failure>(attached));
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
      const auto handled = dispatch_frame(state, frame, stream_guard);
      if (!handled.read_next) {
        status = handled.status;
        break;
      }
    }
    if (state.streams) {
      state.streams->shutdown();
    }
    return status;
  }

private:
  PluginFrameResult handle_heartbeat(StreamState& state, const v1::PluginToCore&,
                                     const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    grpc::Status status = grpc::Status::OK;
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    fill_heartbeat_ack(response.mutable_heartbeat_ack());
    state.sampled = false;
    if (!stream_guard->write(response)) {
      return {false, status};
    }
    return {};
  }
  PluginFrameResult handle_refresh(StreamState& state, const v1::PluginToCore& frame,
                                   const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    grpc::Status status = grpc::Status::OK;
    auto refreshed = refresh(application_, state, frame.refresh_authorization());
    if (const auto* rejected = std::get_if<target::Failure>(&refreshed)) {
      if (rejected->code == target::ErrorCode::Unauthenticated ||
          rejected->code == target::ErrorCode::PermissionDenied) {
        status = rejected_status(*rejected);
        return {false, status};
      }
      v1::CoreToPlugin response;
      fence_to_wire(state.authorization.fence, response.mutable_fence());
      auto* result = response.mutable_authorization_result();
      result->set_request_id(frame.refresh_authorization().request_id());
      *result->mutable_error() =
          wire_error(rejected->code, frame.refresh_authorization().request_id(), {});
      state.sampled = false;
      if (!stream_guard->write(response)) {
        return {false, status};
      }
      status = rejected_status(*rejected);
      return {false, status};
    }
    state.authorization = std::get<target::BridgeAuthorization>(refreshed);
    if (state.streams) {
      state.streams->set_fence(state.authorization.fence);
      state.streams->cancel_all();
    }
    state.sampled = false;
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    auto* result = response.mutable_authorization_result();
    result->set_request_id(frame.refresh_authorization().request_id());
    authorization_to_wire(state.authorization, result->mutable_authorization());
    if (!stream_guard->write(response)) {
      return {false, status};
    }
    return {};
  }
  PluginFrameResult handle_ack(StreamState& state, const v1::PluginToCore& frame,
                               const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    grpc::Status status = grpc::Status::OK;
    auto reference = attempt_from_wire(frame.event_ack().attempt());
    if (const auto* rejected = std::get_if<target::Failure>(&reference)) {
      status = rejected_status(*rejected);
      return {false, status};
    }
    const auto& ack_wire = frame.event_ack();
    std::optional<target::Failure> failure;
    if (ack_wire.has_error()) {
      failure = failure_from_wire(ack_wire.error());
      if (!failure) {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "invalid error code"};
        return {false, status};
      }
    }
    if (!ack_wire.accepted() && !failure) {
      status = {grpc::StatusCode::INVALID_ARGUMENT, "rejected ack requires an error"};
      return {false, status};
    }
    auto receipt = application_.acknowledge_delivery(
        state.authorization.fence.instance, std::get<target::BridgeAttemptRef>(reference),
        ack_wire.accepted(), failure, ack_wire.has_error() && ack_wire.error().retryable());
    if (const auto* rejected = std::get_if<target::Failure>(&receipt)) {
      status = rejected_status(*rejected);
      return {false, status};
    }
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    receipt_to_wire(std::get<target::DeliveryReceipt>(receipt),
                    response.mutable_delivery_receipt());
    if (!stream_guard->write(response)) {
      return {false, status};
    }
    return {};
  }
  PluginFrameResult handle_renew(StreamState& state, const v1::PluginToCore& frame,
                                 const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    grpc::Status status = grpc::Status::OK;
    auto reference = attempt_from_wire(frame.renew_delivery());
    if (const auto* rejected = std::get_if<target::Failure>(&reference)) {
      status = rejected_status(*rejected);
      return {false, status};
    }
    auto receipt = application_.renew_delivery(state.authorization.fence.instance,
                                               std::get<target::BridgeAttemptRef>(reference));
    if (const auto* rejected = std::get_if<target::Failure>(&receipt)) {
      status = rejected_status(*rejected);
      return {false, status};
    }
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    receipt_to_wire(std::get<target::DeliveryReceipt>(receipt),
                    response.mutable_delivery_receipt());
    if (!stream_guard->write(response)) {
      return {false, status};
    }
    return {};
  }
  bool handle_action_submit(StreamState& state, const v1::PluginToCore& frame,
                            const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    const auto& request = frame.action();
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    auto* action_response = response.mutable_action_response();
    action_response->set_request_id(request.request_id());
    std::string serialized;
    (void)request.action().SerializeToString(&serialized);
    target::BridgeActionSubmit submit;
    if (!decode_plugin_action(request.action(), serialized, &submit)) {
      *action_response->mutable_error() =
          wire_error(target::ErrorCode::InvalidArgument, request.request_id(),
                     request.action().trace_id(), false);
      return stream_guard->write(response);
    }
    auto submitted = application_.submit_plugin_action(state.authorization.fence.instance, submit);
    if (const auto* rejected = std::get_if<target::Failure>(&submitted)) {
      *action_response->mutable_error() =
          wire_error(rejected->code, request.request_id(), request.action().trace_id(),
                     rejected->recovery == target::Recovery::Backoff);
    } else {
      (void)action_result_to_wire(std::get<target::ActionRecord>(submitted),
                                  action_response->mutable_result());
    }
    return stream_guard->write(response);
  }
  bool handle_action_query(StreamState& state, const v1::PluginToCore& frame,
                           const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    const auto& query = frame.action_query();
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    auto* action_response = response.mutable_action_response();
    action_response->set_request_id(query.request_id());
    std::optional<target::ActionId> action_id;
    std::optional<std::string> idempotency_key;
    if (query.selector_case() == v1::PluginActionQuery::kActionId) {
      action_id = target::ActionId{query.action_id()};
    } else if (query.selector_case() == v1::PluginActionQuery::kIdempotencyKey) {
      idempotency_key = query.idempotency_key();
    } else {
      *action_response->mutable_error() = wire_error(target::ErrorCode::InvalidArgument,
                                                     query.request_id(), query.trace_id(), false);
      return stream_guard->write(response);
    }
    auto found = application_.query_plugin_action(state.authorization.fence.instance,
                                                  std::move(action_id), std::move(idempotency_key));
    if (const auto* rejected = std::get_if<target::Failure>(&found)) {
      *action_response->mutable_error() =
          wire_error(rejected->code, query.request_id(), query.trace_id(), false);
    } else {
      (void)action_result_to_wire(std::get<target::ActionRecord>(found),
                                  action_response->mutable_result());
    }
    return stream_guard->write(response);
  }
  bool handle_action_cancel(StreamState& state, const v1::PluginToCore& frame,
                            const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    const auto& cancel = frame.action_cancel();
    v1::CoreToPlugin response;
    fence_to_wire(state.authorization.fence, response.mutable_fence());
    auto* action_response = response.mutable_action_response();
    action_response->set_request_id(cancel.request_id());
    if (cancel.action_id().empty()) {
      *action_response->mutable_error() = wire_error(target::ErrorCode::InvalidArgument,
                                                     cancel.request_id(), cancel.trace_id(), false);
      return stream_guard->write(response);
    }
    auto cancelled = application_.cancel_plugin_action(state.authorization.fence.instance,
                                                       target::ActionId{cancel.action_id()});
    if (const auto* rejected = std::get_if<target::Failure>(&cancelled)) {
      *action_response->mutable_error() =
          wire_error(rejected->code, cancel.request_id(), cancel.trace_id(), false);
    } else {
      (void)action_result_to_wire(std::get<target::ActionRecord>(cancelled),
                                  action_response->mutable_result());
    }
    return stream_guard->write(response);
  }
  bool finish_task(v1::HostRequest::OperationCase operation,
                   const target::Outcome<target::ScheduledTask>& result, PluginHostReply& reply) {
    if (const auto* rejected = std::get_if<target::Failure>(&result)) {
      const bool retryable = rejected->recovery == target::Recovery::Backoff ||
                             rejected->recovery == target::Recovery::QuerySameIdentity;
      return reply.reject(rejected->code, retryable);
    }
    const auto& task = std::get<target::ScheduledTask>(result);
    v1::TaskResponse* wire = nullptr;
    switch (operation) {
    case v1::HostRequest::kTaskCreate:
      wire = reply.body()->mutable_task_create();
      break;
    case v1::HostRequest::kTaskGet:
      wire = reply.body()->mutable_task_get();
      break;
    case v1::HostRequest::kTaskCancel:
      wire = reply.body()->mutable_task_cancel();
      break;
    default:
      return reply.reject(target::ErrorCode::Internal);
    }
    wire->set_task_id(task.key.task_id);
    switch (task.state) {
    case target::TaskState::Pending:
      wire->set_state(v1::SCHEDULED_TASK_PENDING);
      break;
    case target::TaskState::Running:
      wire->set_state(v1::SCHEDULED_TASK_RUNNING);
      break;
    case target::TaskState::Retrying:
      wire->set_state(v1::SCHEDULED_TASK_RETRYING);
      break;
    case target::TaskState::Completed:
      wire->set_state(v1::SCHEDULED_TASK_COMPLETED);
      break;
    case target::TaskState::Failed:
      wire->set_state(v1::SCHEDULED_TASK_FAILED);
      break;
    case target::TaskState::Cancelled:
      wire->set_state(v1::SCHEDULED_TASK_CANCELLED);
      break;
    }
    if (!time_to_wire(task.run_at, wire->mutable_next_run_at())) {
      return reply.reject(target::ErrorCode::Internal);
    }
    wire->set_version(task.version.value);
    if (!task.occurrence.value.empty()) {
      wire->set_occurrence_id(task.occurrence.value);
    }
    return reply.write();
  }
  bool handle_task(StreamState& state, const v1::HostRequest& host, PluginHostReply& reply) {
    if (!options_.tasks) {
      return reply.reject(target::ErrorCode::CapabilityNotSupported);
    }
    if (!bridge_uuid(host.request_id()) || !bridge_uuid(host.trace_id())) {
      return reply.reject(target::ErrorCode::InvalidArgument);
    }
    const bool create = host.operation_case() == v1::HostRequest::kTaskCreate;
    const bool read = host.operation_case() == v1::HostRequest::kTaskGet;
    const auto operation =
        create ? target::PluginTaskOperation::Create
               : (read ? target::PluginTaskOperation::Read : target::PluginTaskOperation::Cancel);
    const auto installed = application_.task_profile(state.authorization.fence.instance, operation);
    if (const auto* rejected = std::get_if<target::Failure>(&installed)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto& profile =
        std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(installed);
    if (!host.idempotency_key().empty() && host.idempotency_key().size() > 256) {
      return reply.reject(target::ErrorCode::InvalidArgument);
    }
    auto sampled = application_.current_time();
    if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto now = std::get<target::TimePoint>(sampled);
    target::TimePoint deadline = now + std::chrono::seconds{5};
    if (host.has_deadline()) {
      const auto decoded = codec::from_wire(host.deadline());
      const auto* value = std::get_if<target::TimePoint>(&decoded);
      if (!value || *value <= now) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
      deadline = *value;
    }
    target::SchedulerTaskContext context{state.authorization.fence.instance,
                                         *profile.installation,
                                         host.request_id(),
                                         host.trace_id(),
                                         deadline,
                                         host.idempotency_key()};
    if (create) {
      if (host.task_create().task_id().empty() || host.idempotency_key().empty() ||
          host.task_create().target_conversation().empty()) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
      auto decoded = task_creation_from_wire(host.task_create());
      if (const auto* rejected = std::get_if<target::Failure>(&decoded)) {
        return reply.reject(rejected->code);
      }
      auto input = std::get<target::TaskCreationInput>(std::move(decoded));
      return finish_task(host.operation_case(),
                         options_.tasks->create_task(context, std::move(input)), reply);
    } else if (read) {
      return finish_task(host.operation_case(),
                         options_.tasks->get_task(context, host.task_get().task_id()), reply);
    } else {
      return finish_task(host.operation_case(),
                         options_.tasks->cancel_task(context, host.task_cancel().task_id()), reply);
    }
  }
  bool handle_storage(StreamState& state, const v1::HostRequest& host, PluginHostReply& reply) {
    if (!bridge_uuid(host.request_id()) || !bridge_uuid(host.trace_id())) {
      return reply.reject(target::ErrorCode::InvalidArgument);
    }
    const auto installed = application_.storage_profile(state.authorization.fence.instance);
    if (const auto* rejected = std::get_if<target::Failure>(&installed)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto& profile =
        std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(installed);
    auto decoded = storage_operation_from_wire(host);
    if (const auto* rejected = std::get_if<target::Failure>(&decoded)) {
      return reply.reject(rejected->code);
    }
    const auto& operation = std::get<StorageWireOperation>(decoded);
    auto scope =
        storage_scope_from_wire(operation.scope, state.authorization.fence.instance, profile);
    if (const auto* rejected = std::get_if<target::Failure>(&scope)) {
      return reply.reject(rejected->code);
    }
    const auto requested = std::get<target::RequestedScopeKind>(scope);
    auto sampled = application_.current_time();
    if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto now = std::get<target::TimePoint>(sampled);
    target::TimePoint deadline = now + std::chrono::seconds{30};
    if (host.has_deadline()) {
      const auto decoded = codec::from_wire(host.deadline());
      const auto* value = std::get_if<target::TimePoint>(&decoded);
      if (!value) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
      deadline = *value;
    }
    auto admitted_context = target::plugin_storage_context(
        state.authorization, profile, requested,
        {host.request_id(), host.trace_id(), operation.method, host.idempotency_key(), deadline},
        now);
    if (const auto* rejected = std::get_if<target::Failure>(&admitted_context)) {
      return reply.reject(rejected->code);
    }
    auto context = std::get<target::StorageCommandContext>(std::move(admitted_context));
    if (operation.mutating) {
      context.fingerprint = storage_fingerprint(*operation.message);
    }
    switch (host.operation_case()) {
    case v1::HostRequest::kStorageGet:
      return handle_storage_get(context, host, reply);
    case v1::HostRequest::kStorageSet:
      return handle_storage_set(context, host, reply);
    case v1::HostRequest::kStorageDelete:
      return handle_storage_delete(context, host, reply);
    case v1::HostRequest::kStorageCompareAndSet:
      return handle_storage_cas(context, host, reply);
    default:
      return reply.reject(target::ErrorCode::CapabilityNotSupported);
    }
  }
  bool handle_storage_get(const target::StorageCommandContext& context, const v1::HostRequest& host,
                          PluginHostReply& reply) {
    auto found = options_.storage->get_storage({context, host.storage_get().key()});
    if (const auto* rejected = std::get_if<target::Failure>(&found)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto& entry = std::get<target::StorageEntryResult>(found);
    auto* wire = reply.body()->mutable_storage_get();
    wire->set_value(entry.value.bytes.data(), entry.value.bytes.size());
    wire->set_version(entry.value.version.value);
    if (entry.value.expires_at &&
        !time_to_wire(*entry.value.expires_at, wire->mutable_expires_at())) {
      return reply.reject(target::ErrorCode::Internal);
    }
    return reply.write();
  }
  bool handle_storage_set(const target::StorageCommandContext& context, const v1::HostRequest& host,
                          PluginHostReply& reply) {
    std::optional<target::TimePoint> expires;
    if (host.storage_set().has_expires_at()) {
      const auto decoded = codec::from_wire(host.storage_set().expires_at());
      const auto* value = std::get_if<target::TimePoint>(&decoded);
      if (!value) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
      expires = *value;
    }
    std::optional<target::CompareCondition> condition;
    if (host.storage_set().has_condition()) {
      condition = storage_condition_from_wire(host.storage_set().condition());
      if (!condition) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
    }
    const auto& bytes = host.storage_set().value();
    std::vector<std::byte> value(bytes.size());
    std::memcpy(value.data(), bytes.data(), bytes.size());
    auto written = options_.storage->set_storage(
        {context, host.storage_set().key(), std::move(value), expires, condition});
    if (const auto* rejected = std::get_if<target::Failure>(&written)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto& entry = std::get<target::StorageEntryResult>(written);
    auto* wire = reply.body()->mutable_storage_set();
    wire->set_version(entry.value.version.value);
    if (entry.value.expires_at &&
        !time_to_wire(*entry.value.expires_at, wire->mutable_expires_at())) {
      return reply.reject(target::ErrorCode::Internal);
    }
    return reply.write();
  }
  bool handle_storage_delete(const target::StorageCommandContext& context,
                             const v1::HostRequest& host, PluginHostReply& reply) {
    std::optional<target::CompareCondition> condition;
    if (host.storage_delete().has_condition()) {
      condition = storage_condition_from_wire(host.storage_delete().condition());
      if (!condition) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
    }
    auto removed =
        options_.storage->delete_storage({context, host.storage_delete().key(), condition});
    if (const auto* rejected = std::get_if<target::Failure>(&removed)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto& result = std::get<target::StorageDeleteResult>(removed);
    auto* wire = reply.body()->mutable_storage_delete();
    wire->set_deleted(result.deleted);
    if (result.version) {
      wire->set_version(result.version->value);
    }
    return reply.write();
  }
  bool handle_storage_cas(const target::StorageCommandContext& context, const v1::HostRequest& host,
                          PluginHostReply& reply) {
    std::optional<target::TimePoint> expires;
    if (host.storage_compare_and_set().has_expires_at()) {
      const auto decoded = codec::from_wire(host.storage_compare_and_set().expires_at());
      const auto* value = std::get_if<target::TimePoint>(&decoded);
      if (!value) {
        return reply.reject(target::ErrorCode::InvalidArgument);
      }
      expires = *value;
    }
    if (!host.storage_compare_and_set().has_condition()) {
      return reply.reject(target::ErrorCode::InvalidArgument);
    }
    auto condition = storage_condition_from_wire(host.storage_compare_and_set().condition());
    if (!condition) {
      return reply.reject(target::ErrorCode::InvalidArgument);
    }
    const auto& bytes = host.storage_compare_and_set().value();
    std::vector<std::byte> value(bytes.size());
    std::memcpy(value.data(), bytes.data(), bytes.size());
    auto written = options_.storage->compare_and_set_storage(
        {context, host.storage_compare_and_set().key(), std::move(value), *condition, expires});
    if (const auto* rejected = std::get_if<target::Failure>(&written)) {
      return reply.reject(rejected->code, rejected->recovery == target::Recovery::Backoff);
    }
    const auto& entry = std::get<target::StorageEntryResult>(written);
    if (!storage_entry_to_wire(entry.value, host.storage_compare_and_set().key(),
                               reply.body()->mutable_storage_compare_and_set()->mutable_entry())) {
      return reply.reject(target::ErrorCode::Internal);
    }
    return reply.write();
  }
  bool handle_model(StreamState& state, const v1::HostRequest& host, PluginHostReply& reply) {
    const auto admitted = target::admit_plugin_model(state.permissions);
    auto deadline = codec::from_wire(host.deadline());
    if (host.operation_case() != v1::HostRequest::kModelChat) {
      *reply.body()->mutable_error() =
          wire_error(target::ErrorCode::CapabilityNotSupported, host.request_id(), host.trace_id());
    } else if (!options_.model) {
      *reply.body()->mutable_error() =
          wire_error(target::ErrorCode::CapabilityNotSupported, host.request_id(), host.trace_id());
    } else if (const auto* rejected = std::get_if<target::Failure>(&admitted)) {
      *reply.body()->mutable_error() =
          wire_error(rejected->code, host.request_id(), host.trace_id());
    } else if (const auto* parsed = std::get_if<target::TimePoint>(&deadline); parsed == nullptr) {
      *reply.body()->mutable_error() =
          wire_error(target::ErrorCode::InvalidArgument, host.request_id(), host.trace_id());
    } else {
      const auto& chat = host.model_chat();
      target::RuntimeModelChatRequest command;
      command.instance = state.authorization.fence.instance;
      command.request_id = host.request_id();
      command.logical_call_id = chat.logical_call_id();
      command.model = chat.model();
      command.messages.reserve(static_cast<std::size_t>(chat.messages_size()));
      for (const auto& message : chat.messages()) {
        command.messages.push_back({message.role(), message.text()});
      }
      const auto installed_profile =
          application_.plugin_profile(state.authorization.fence.instance);
      if (const auto* rejected = std::get_if<target::Failure>(&installed_profile)) {
        *reply.body()->mutable_error() =
            wire_error(rejected->code, host.request_id(), host.trace_id());
      } else {
        const auto& profile =
            std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(installed_profile);
        auto outcome =
            target::submit_plugin_model(*options_.model, state.authorization.fence.instance,
                                        state.permissions, profile, std::move(command), *parsed);
        if (const auto* failure = std::get_if<target::Failure>(&outcome)) {
          const bool retryable = failure->recovery == target::Recovery::QuerySameIdentity ||
                                 failure->code == target::ErrorCode::RateLimited;
          *reply.body()->mutable_error() =
              wire_error(failure->code, host.request_id(), host.trace_id(), retryable);
        } else {
          const auto& value = std::get<target::RuntimeModelChatReply>(outcome);
          auto* result = reply.body()->mutable_model_chat();
          result->set_text(value.text);
          result->set_input_tokens(value.input_tokens);
          result->set_output_tokens(value.output_tokens);
        }
      }
    }
    return reply.write();
  }
  bool reject_stream_start(PluginHostReply& reply, target::ErrorCode code, bool retryable = false) {
    return reply.reject(code, retryable);
  }
  bool handle_model_stream_start(StreamState& state, const v1::HostRequest& host,
                                 PluginHostReply& reply) {
    if (!state.streams || !options_.model) {
      return reject_stream_start(reply, target::ErrorCode::CapabilityNotSupported);
    }
    const auto parsed_deadline = codec::from_wire(host.deadline());
    const auto& start = host.model_stream_start();
    if (!target::valid_model_identifier(host.request_id()) ||
        !target::valid_model_identifier(host.trace_id()) ||
        !target::valid_model_identifier(start.logical_call_id()) || start.model().empty() ||
        start.model().size() > 256 || start.messages_size() < 1 ||
        start.messages_size() > static_cast<int>(target::kModelMaximumMessages) ||
        !std::holds_alternative<target::TimePoint>(parsed_deadline)) {
      return reject_stream_start(reply, target::ErrorCode::InvalidArgument);
    }
    std::size_t input_bytes = 0;
    for (const auto& message : start.messages()) {
      if (message.text().size() > target::kModelMaximumRequestBytes - input_bytes ||
          !target::valid_model_message_role(message.role()) ||
          message.text().find('\0') != std::string::npos) {
        return reject_stream_start(reply, target::ErrorCode::InvalidArgument);
      }
      input_bytes += message.text().size();
    }
    const auto profile = application_.plugin_profile(state.authorization.fence.instance);
    if (const auto* rejected = std::get_if<target::Failure>(&profile)) {
      return reject_stream_start(reply, rejected->code);
    }
    const auto& installed =
        std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(profile);
    if (!installed.installation) {
      return reject_stream_start(reply, target::ErrorCode::PermissionDenied);
    }
    const auto admitted = target::admit_plugin_model_stream(state.authorization.fence.instance,
                                                            installed, state.permissions);
    if (const auto* rejected = std::get_if<target::Failure>(&admitted)) {
      return reject_stream_start(reply, rejected->code,
                                 rejected->recovery == target::Recovery::Backoff ||
                                     rejected->code == target::ErrorCode::RateLimited);
    }
    const auto started =
        state.streams->start(host, start, state.authorization.fence,
                             std::get<target::TimePoint>(parsed_deadline), *installed.installation);
    if (const auto* rejected = std::get_if<target::Failure>(&started)) {
      return reject_stream_start(reply, rejected->code,
                                 rejected->recovery == target::Recovery::Backoff ||
                                     rejected->code == target::ErrorCode::RateLimited);
    }
    // A successful start has no HostResponse: the attempt lifecycle is only model_stream_event.
    return true;
  }
  bool handle_model_stream_cancel(StreamState& state, const v1::HostRequest& host,
                                  PluginHostReply& reply) {
    if (!state.streams) {
      return reject_stream_start(reply, target::ErrorCode::CapabilityNotSupported);
    }
    const auto& cancel = host.model_stream_cancel();
    const auto parsed_deadline = codec::from_wire(host.deadline());
    if (!target::valid_model_identifier(host.request_id()) ||
        !target::valid_model_identifier(cancel.start_request_id()) ||
        !target::valid_model_identifier(cancel.logical_call_id()) ||
        !std::holds_alternative<target::TimePoint>(parsed_deadline)) {
      return reject_stream_start(reply, target::ErrorCode::InvalidArgument);
    }
    const auto disposition =
        state.streams->cancel(cancel.start_request_id(), cancel.logical_call_id());
    auto* receipt = reply.body()->mutable_model_stream_cancel();
    receipt->set_start_request_id(cancel.start_request_id());
    receipt->set_logical_call_id(cancel.logical_call_id());
    receipt->set_disposition(disposition);
    return reply.write();
  }
  bool handle_host(StreamState& state, const v1::HostRequest& host,
                   const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    PluginHostReply reply{state.authorization.fence, host, stream_guard};
    if (const auto rejected = host_request_shape_error(host)) {
      return reply.reject(*rejected);
    }
    switch (host.operation_case()) {
    case v1::HostRequest::kTaskCreate:
    case v1::HostRequest::kTaskGet:
    case v1::HostRequest::kTaskCancel:
      return handle_task(state, host, reply);
    case v1::HostRequest::kModelChat:
      return handle_model(state, host, reply);
    case v1::HostRequest::kModelStreamStart:
      return handle_model_stream_start(state, host, reply);
    case v1::HostRequest::kModelStreamCancel:
      return handle_model_stream_cancel(state, host, reply);
    default:
      if (options_.storage) {
        return handle_storage(state, host, reply);
      }
      return reply.reject(target::ErrorCode::CapabilityNotSupported);
    }
  }

  PluginFrameResult dispatch_frame(StreamState& state, const v1::PluginToCore& frame,
                                   const std::shared_ptr<PluginStreamGuard>& stream_guard) {
    if (state.streams) {
      state.streams->reap();
    }
    switch (frame.body_case()) {
    case v1::PluginToCore::kHeartbeat:
      return handle_heartbeat(state, frame, stream_guard);
    case v1::PluginToCore::kRefreshAuthorization:
      return handle_refresh(state, frame, stream_guard);
    case v1::PluginToCore::kEventAck:
      return handle_ack(state, frame, stream_guard);
    case v1::PluginToCore::kRenewDelivery:
      return handle_renew(state, frame, stream_guard);
    case v1::PluginToCore::kRegister:
      return {false, {grpc::StatusCode::INVALID_ARGUMENT, "duplicate registration frame"}};
    case v1::PluginToCore::kAction:
      return {handle_action_submit(state, frame, stream_guard), grpc::Status::OK};
    case v1::PluginToCore::kActionQuery:
      return {handle_action_query(state, frame, stream_guard), grpc::Status::OK};
    case v1::PluginToCore::kActionCancel:
      return {handle_action_cancel(state, frame, stream_guard), grpc::Status::OK};
    case v1::PluginToCore::kHostRequest:
      return {handle_host(state, frame.host_request(), stream_guard), grpc::Status::OK};
    default:
      v1::CoreToPlugin response;
      fence_to_wire(state.authorization.fence, response.mutable_fence());
      *response.mutable_error() = wire_error(target::ErrorCode::CapabilityNotSupported, {}, {});
      return {stream_guard->write(response), grpc::Status::OK};
    }
  }

  target::RuntimeBridgeApplication& application_;
  PluginBridgeOptions options_;
  // Runtime process-wide active Model stream cap shared by every plugin connection.
  std::atomic<std::size_t> plugin_streams_{0};
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
make_plugin_bridge(target::RuntimeBridgeApplication& application, PluginBridgeOptions options) {
  try {
    return std::unique_ptr<v1::PluginBridge::Service>(
        std::make_unique<PluginBridgeImpl>(application, std::move(options)));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
