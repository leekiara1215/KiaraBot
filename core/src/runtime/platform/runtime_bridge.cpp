#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"

#include <algorithm>
#include <cstdio>
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

#include <openssl/sha.h>

#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/ports/target/runtime_model.hpp"
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
struct StreamState {
  target::BridgePeerKind kind = target::BridgePeerKind::Adapter;
  target::BridgeAuthorization authorization;
  std::string adapter_id;
  target::TimePoint sampled_at;
  bool sampled = false;
  // Negotiated registration grants; plugin host frames are only served when the exact permission
  // was granted at registration. Empty for adapters.
  std::vector<std::string> permissions;
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
    } exit_guard{application_,       guard_,          stream_instance, stream_generation,
                 &shutdown_attached, &action_attached};
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
    {
      std::weak_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> weak = guard_;
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
        if (!write(*stream, response)) {
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
        if (!write(*stream, response)) {
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
  bool write(grpc::ServerReaderWriter<v1::CoreToAdapter, v1::AdapterToCore>&,
             const v1::CoreToAdapter& frame) {
    return guard_ && guard_->write(frame);
  }
  target::RuntimeBridgeApplication& application_;
  std::shared_ptr<StreamGuard<v1::CoreToAdapter, v1::AdapterToCore>> guard_;
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

class PluginBridgeImpl final : public v1::PluginBridge::Service {
public:
  PluginBridgeImpl(target::RuntimeBridgeApplication& application, PluginBridgeOptions options)
      : application_(application), options_(std::move(options)) {}

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
    StreamState state{target::BridgePeerKind::Plugin, result.authorization, {}, {}, false,
                      result.granted_permissions};
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
      } else if (body == v1::PluginToCore::kAction) {
        const auto& request = frame.action();
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* action_response = response.mutable_action_response();
        action_response->set_request_id(request.request_id());
        std::string serialized;
        const bool serialized_ok = request.action().SerializeToString(&serialized);
        target::BridgeActionSubmit submit;
        if (!decode_plugin_action(request.action(), serialized, &submit)) {
          *action_response->mutable_error() =
              wire_error(target::ErrorCode::InvalidArgument, request.request_id(),
                         request.action().trace_id(), false);
          if (!write(*stream, response)) {
            break;
          }
          continue;
        }
        auto submitted =
            application_.submit_plugin_action(state.authorization.fence.instance, submit);
        if (const auto* rejected = std::get_if<target::Failure>(&submitted)) {
          *action_response->mutable_error() =
              wire_error(rejected->code, request.request_id(), request.action().trace_id(),
                         rejected->recovery == target::Recovery::Backoff);
        } else {
          (void)action_result_to_wire(std::get<target::ActionRecord>(submitted),
                                      action_response->mutable_result());
        }
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kActionQuery) {
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
          *action_response->mutable_error() = wire_error(
              target::ErrorCode::InvalidArgument, query.request_id(), query.trace_id(), false);
          if (!write(*stream, response)) {
            break;
          }
          continue;
        }
        auto found = application_.query_plugin_action(
            state.authorization.fence.instance, std::move(action_id), std::move(idempotency_key));
        if (const auto* rejected = std::get_if<target::Failure>(&found)) {
          *action_response->mutable_error() =
              wire_error(rejected->code, query.request_id(), query.trace_id(), false);
        } else {
          (void)action_result_to_wire(std::get<target::ActionRecord>(found),
                                      action_response->mutable_result());
        }
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kActionCancel) {
        const auto& cancel = frame.action_cancel();
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* action_response = response.mutable_action_response();
        action_response->set_request_id(cancel.request_id());
        if (cancel.action_id().empty()) {
          *action_response->mutable_error() = wire_error(
              target::ErrorCode::InvalidArgument, cancel.request_id(), cancel.trace_id(), false);
          if (!write(*stream, response)) {
            break;
          }
          continue;
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
        if (!write(*stream, response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kHostRequest) {
        const auto& host = frame.host_request();
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* host_response = response.mutable_host_response();
        host_response->set_request_id(host.request_id());
        const auto reject = [&](target::ErrorCode code, bool retryable = false) {
          *host_response->mutable_error() =
              wire_error(code, host.request_id(), host.trace_id(), retryable);
          return write(*stream, response);
        };
        const bool task_operation = host.operation_case() == v1::HostRequest::kTaskCreate ||
                                    host.operation_case() == v1::HostRequest::kTaskGet ||
                                    host.operation_case() == v1::HostRequest::kTaskCancel;
        if (task_operation) {
          if (!options_.tasks) {
            if (!reject(target::ErrorCode::CapabilityNotSupported)) {
              break;
            }
            continue;
          }
          if (!bridge_uuid(host.request_id()) || !bridge_uuid(host.trace_id())) {
            if (!reject(target::ErrorCode::InvalidArgument)) {
              break;
            }
            continue;
          }
          const auto installed = application_.plugin_profile(state.authorization.fence.instance);
          if (const auto* rejected = std::get_if<target::Failure>(&installed)) {
            if (!reject(rejected->code, rejected->recovery == target::Recovery::Backoff)) {
              break;
            }
            continue;
          }
          const auto& profile =
              std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(installed);
          if (!profile.installation) {
            if (!reject(target::ErrorCode::PermissionDenied)) {
              break;
            }
            continue;
          }
          if (std::find(profile.features.begin(), profile.features.end(), "host.tasks.v1") ==
              profile.features.end()) {
            if (!reject(target::ErrorCode::CapabilityNotSupported)) {
              break;
            }
            continue;
          }
          const auto has_permission = [&](std::string_view permission) {
            return std::find(profile.permissions.begin(), profile.permissions.end(), permission) !=
                   profile.permissions.end();
          };
          const bool create = host.operation_case() == v1::HostRequest::kTaskCreate;
          const bool read = host.operation_case() == v1::HostRequest::kTaskGet;
          if ((create && !has_permission("task.schedule")) ||
              (!create && read && !has_permission("task.read")) ||
              (!create && !read && !has_permission("task.schedule"))) {
            if (!reject(target::ErrorCode::PermissionDenied)) {
              break;
            }
            continue;
          }
          if (create && std::find(profile.subscriptions.begin(), profile.subscriptions.end(),
                                  "event.timer.triggered") == profile.subscriptions.end()) {
            if (!reject(target::ErrorCode::PermissionDenied)) {
              break;
            }
            continue;
          }
          if (!host.idempotency_key().empty() && host.idempotency_key().size() > 256) {
            if (!reject(target::ErrorCode::InvalidArgument)) {
              break;
            }
            continue;
          }
          auto sampled = application_.current_time();
          if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
            if (!reject(rejected->code, rejected->recovery == target::Recovery::Backoff)) {
              break;
            }
            continue;
          }
          const auto now = std::get<target::TimePoint>(sampled);
          target::TimePoint deadline = now + std::chrono::seconds{5};
          if (host.has_deadline()) {
            const auto decoded = codec::from_wire(host.deadline());
            const auto* value = std::get_if<target::TimePoint>(&decoded);
            if (!value || *value <= now) {
              if (!reject(target::ErrorCode::InvalidArgument)) {
                break;
              }
              continue;
            }
            deadline = *value;
          }
          target::SchedulerTaskContext context{state.authorization.fence.instance,
                                               *profile.installation,
                                               host.request_id(),
                                               host.trace_id(),
                                               deadline,
                                               host.idempotency_key()};
          const auto finish = [&](const auto& result) {
            if (const auto* rejected = std::get_if<target::Failure>(&result)) {
              const bool retryable = rejected->recovery == target::Recovery::Backoff ||
                                     rejected->recovery == target::Recovery::QuerySameIdentity;
              return reject(rejected->code, retryable);
            }
            const auto& task = std::get<target::ScheduledTask>(result);
            v1::TaskResponse* wire = nullptr;
            switch (host.operation_case()) {
            case v1::HostRequest::kTaskCreate:
              wire = host_response->mutable_task_create();
              break;
            case v1::HostRequest::kTaskGet:
              wire = host_response->mutable_task_get();
              break;
            case v1::HostRequest::kTaskCancel:
              wire = host_response->mutable_task_cancel();
              break;
            default:
              return reject(target::ErrorCode::Internal);
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
              return reject(target::ErrorCode::Internal);
            }
            wire->set_version(task.version.value);
            if (!task.occurrence.value.empty()) {
              wire->set_occurrence_id(task.occurrence.value);
            }
            return write(*stream, response);
          };
          if (create) {
            if (host.task_create().task_id().empty() || host.idempotency_key().empty() ||
                host.task_create().target_conversation().empty()) {
              if (!reject(target::ErrorCode::InvalidArgument)) {
                break;
              }
              continue;
            }
            target::TaskCreationInput input;
            input.task_id = host.task_create().task_id();
            input.target = target::ConversationKey{{},
                                                   {},
                                                   host.task_create().target_conversation(),
                                                   target::ConversationKey::Kind::Group};
            const auto& payload = host.task_create().payload();
            input.payload.resize(static_cast<std::size_t>(payload.size()));
            std::memcpy(input.payload.data(), payload.data(), input.payload.size());
            if (host.task_create().when_case() == v1::TaskCreateRequest::kRunAt) {
              const auto decoded = codec::from_wire(host.task_create().run_at());
              const auto* value = std::get_if<target::TimePoint>(&decoded);
              if (!value) {
                if (!reject(target::ErrorCode::InvalidArgument)) {
                  break;
                }
                continue;
              }
              input.run_at = *value;
            } else if (host.task_create().when_case() == v1::TaskCreateRequest::kLocalTime) {
              const auto& local = host.task_create().local_time();
              input.local_time = target::LocalCivilTime{
                  local.year(),   local.month(),  local.day(),      local.hour(),
                  local.minute(), local.second(), local.time_zone()};
            } else {
              if (!reject(target::ErrorCode::InvalidArgument)) {
                break;
              }
              continue;
            }
            if (!finish(options_.tasks->create_task(context, std::move(input)))) {
              break;
            }
          } else if (read) {
            if (!finish(options_.tasks->get_task(context, host.task_get().task_id()))) {
              break;
            }
          } else {
            if (!finish(options_.tasks->cancel_task(context, host.task_cancel().task_id()))) {
              break;
            }
          }
          continue;
        }
        if (options_.storage && host.operation_case() != v1::HostRequest::kModelChat) {
          if (!bridge_uuid(host.request_id()) || !bridge_uuid(host.trace_id())) {
            if (!reject(target::ErrorCode::InvalidArgument)) {
              break;
            }
            continue;
          }
          const auto installed = application_.plugin_profile(state.authorization.fence.instance);
          if (const auto* rejected = std::get_if<target::Failure>(&installed)) {
            if (!reject(rejected->code, rejected->recovery == target::Recovery::Backoff)) {
              break;
            }
            continue;
          }
          const auto& profile =
              std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(installed);
          if (std::find(profile.permissions.begin(), profile.permissions.end(),
                        "storage.namespace") == profile.permissions.end()) {
            if (!reject(target::ErrorCode::PermissionDenied)) {
              break;
            }
            continue;
          }
          const v1::NamespaceScope* wire_scope = nullptr;
          const google::protobuf::Message* operation = nullptr;
          target::RequestedScopeKind requested = target::RequestedScopeKind::DefaultPrivate;
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
            wire_scope =
                host.storage_delete().has_scope() ? &host.storage_delete().scope() : nullptr;
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
            if (!reject(target::ErrorCode::CapabilityNotSupported)) {
              break;
            }
            continue;
          }
          target::InstallationId shared_installation;
          if (wire_scope != nullptr) {
            switch (wire_scope->scope_case()) {
            case v1::NamespaceScope::SCOPE_NOT_SET:
            case v1::NamespaceScope::kInstancePrivate:
              if (wire_scope->scope_case() == v1::NamespaceScope::kInstancePrivate &&
                  !wire_scope->instance_private()) {
                if (!reject(target::ErrorCode::InvalidArgument)) {
                  break;
                }
                continue;
              }
              break;
            case v1::NamespaceScope::kInstallationShared:
              if (!wire_scope->installation_shared() || !profile.installation) {
                if (!reject(profile.installation ? target::ErrorCode::InvalidArgument
                                                 : target::ErrorCode::PermissionDenied)) {
                  break;
                }
                continue;
              }
              requested = target::RequestedScopeKind::InstallationShared;
              shared_installation = *profile.installation;
              break;
            case v1::NamespaceScope::kInstallationSharedId:
              // Plugin frames never self-report a namespace id; it is derived from the registered
              // installation. Only Runtime internal dispatcher frames may use the explicit id.
              if (!reject(target::ErrorCode::CapabilityNotSupported)) {
                break;
              }
              continue;
            }
          }
          const target::ResourceRef authorization_resource =
              requested == target::RequestedScopeKind::InstallationShared
                  ? target::ResourceRef{target::ResourceKind::Installation,
                                        {shared_installation.value}}
                  : target::ResourceRef{target::ResourceKind::Instance,
                                        {state.authorization.fence.instance.value}};
          auto sampled = application_.current_time();
          if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
            if (!reject(rejected->code, rejected->recovery == target::Recovery::Backoff)) {
              break;
            }
            continue;
          }
          const auto now = std::get<target::TimePoint>(sampled);
          target::TimePoint deadline = now + std::chrono::seconds{30};
          if (host.has_deadline()) {
            const auto decoded = codec::from_wire(host.deadline());
            const auto* value = std::get_if<target::TimePoint>(&decoded);
            if (!value) {
              if (!reject(target::ErrorCode::InvalidArgument)) {
                break;
              }
              continue;
            }
            deadline = *value;
          }
          target::StorageCommandContext context;
          context.authentication.subject = {state.authorization.fence.instance.value};
          context.authentication.direct_peer =
              target::PluginPeer{state.authorization.fence.instance,
                                 target::SubjectId{state.authorization.fence.instance.value}};
          context.authentication.audience = target::Service::Runtime;
          context.authentication.actors = {context.authentication.subject};
          context.authentication.scopes = {{authorization_resource, "storage.namespace"}};
          context.authentication.authorization_version = {
              state.authorization.authorization_version};
          context.authentication.verified_at = state.authorization.issued_at;
          context.authentication.expires_at = state.authorization.expires_at;
          context.request_id = host.request_id();
          context.trace_id = host.trace_id();
          context.method = method;
          context.idempotency_key = host.idempotency_key();
          context.deadline = deadline;
          context.instance = state.authorization.fence.instance;
          context.requested = requested;
          context.bridge_authorization =
              target::RuntimeAuthorizationDecision{true,
                                                   context.authentication.subject,
                                                   target::Permission::StorageNamespace,
                                                   {authorization_resource},
                                                   {state.authorization.authorization_version},
                                                   now,
                                                   std::nullopt};
          if (mutating) {
            context.fingerprint = storage_fingerprint(*operation);
          }
          const auto reject_failure = [&](const target::Failure& failure) {
            return reject(failure.code, failure.recovery == target::Recovery::Backoff);
          };
          if (host.operation_case() == v1::HostRequest::kStorageGet) {
            auto found = options_.storage->get_storage({context, host.storage_get().key()});
            if (const auto* rejected = std::get_if<target::Failure>(&found)) {
              if (!reject_failure(*rejected)) {
                break;
              }
              continue;
            }
            const auto& entry = std::get<target::StorageEntryResult>(found);
            auto* wire = host_response->mutable_storage_get();
            wire->set_value(entry.value.bytes.data(), entry.value.bytes.size());
            wire->set_version(entry.value.version.value);
            if (entry.value.expires_at &&
                !time_to_wire(*entry.value.expires_at, wire->mutable_expires_at())) {
              if (!reject(target::ErrorCode::Internal)) {
                break;
              }
              continue;
            }
          } else if (host.operation_case() == v1::HostRequest::kStorageSet) {
            std::optional<target::TimePoint> expires;
            if (host.storage_set().has_expires_at()) {
              const auto decoded = codec::from_wire(host.storage_set().expires_at());
              const auto* value = std::get_if<target::TimePoint>(&decoded);
              if (!value) {
                if (!reject(target::ErrorCode::InvalidArgument)) {
                  break;
                }
                continue;
              }
              expires = *value;
            }
            std::optional<target::CompareCondition> condition;
            if (host.storage_set().has_condition()) {
              condition = storage_condition_from_wire(host.storage_set().condition());
              if (!condition) {
                if (!reject(target::ErrorCode::InvalidArgument)) {
                  break;
                }
                continue;
              }
            }
            const auto& bytes = host.storage_set().value();
            std::vector<std::byte> value(bytes.size());
            std::memcpy(value.data(), bytes.data(), bytes.size());
            auto written = options_.storage->set_storage(
                {context, host.storage_set().key(), std::move(value), expires, condition});
            if (const auto* rejected = std::get_if<target::Failure>(&written)) {
              if (!reject_failure(*rejected)) {
                break;
              }
              continue;
            }
            const auto& entry = std::get<target::StorageEntryResult>(written);
            auto* wire = host_response->mutable_storage_set();
            wire->set_version(entry.value.version.value);
            if (entry.value.expires_at &&
                !time_to_wire(*entry.value.expires_at, wire->mutable_expires_at())) {
              if (!reject(target::ErrorCode::Internal)) {
                break;
              }
              continue;
            }
          } else if (host.operation_case() == v1::HostRequest::kStorageDelete) {
            std::optional<target::CompareCondition> condition;
            if (host.storage_delete().has_condition()) {
              condition = storage_condition_from_wire(host.storage_delete().condition());
              if (!condition) {
                if (!reject(target::ErrorCode::InvalidArgument)) {
                  break;
                }
                continue;
              }
            }
            auto removed =
                options_.storage->delete_storage({context, host.storage_delete().key(), condition});
            if (const auto* rejected = std::get_if<target::Failure>(&removed)) {
              if (!reject_failure(*rejected)) {
                break;
              }
              continue;
            }
            const auto& result = std::get<target::StorageDeleteResult>(removed);
            auto* wire = host_response->mutable_storage_delete();
            wire->set_deleted(result.deleted);
            if (result.version) {
              wire->set_version(result.version->value);
            }
          } else {
            std::optional<target::TimePoint> expires;
            if (host.storage_compare_and_set().has_expires_at()) {
              const auto decoded = codec::from_wire(host.storage_compare_and_set().expires_at());
              const auto* value = std::get_if<target::TimePoint>(&decoded);
              if (!value) {
                if (!reject(target::ErrorCode::InvalidArgument)) {
                  break;
                }
                continue;
              }
              expires = *value;
            }
            if (!host.storage_compare_and_set().has_condition()) {
              if (!reject(target::ErrorCode::InvalidArgument)) {
                break;
              }
              continue;
            }
            auto condition =
                storage_condition_from_wire(host.storage_compare_and_set().condition());
            if (!condition) {
              if (!reject(target::ErrorCode::InvalidArgument)) {
                break;
              }
              continue;
            }
            const auto& bytes = host.storage_compare_and_set().value();
            std::vector<std::byte> value(bytes.size());
            std::memcpy(value.data(), bytes.data(), bytes.size());
            auto written = options_.storage->compare_and_set_storage(
                {context, host.storage_compare_and_set().key(), std::move(value), *condition,
                 expires});
            if (const auto* rejected = std::get_if<target::Failure>(&written)) {
              if (!reject_failure(*rejected)) {
                break;
              }
              continue;
            }
            const auto& entry = std::get<target::StorageEntryResult>(written);
            if (!storage_entry_to_wire(
                    entry.value, host.storage_compare_and_set().key(),
                    host_response->mutable_storage_compare_and_set()->mutable_entry())) {
              if (!reject(target::ErrorCode::Internal)) {
                break;
              }
              continue;
            }
          }
          if (!write(*stream, response)) {
            break;
          }
          continue;
        }
        const auto grant = std::find(state.permissions.begin(), state.permissions.end(),
                                     "model.chat") != state.permissions.end();
        auto deadline = codec::from_wire(host.deadline());
        if (host.operation_case() != v1::HostRequest::kModelChat) {
          *host_response->mutable_error() = wire_error(target::ErrorCode::CapabilityNotSupported,
                                                       host.request_id(), host.trace_id());
        } else if (!options_.model) {
          *host_response->mutable_error() = wire_error(target::ErrorCode::CapabilityNotSupported,
                                                       host.request_id(), host.trace_id());
        } else if (!grant) {
          *host_response->mutable_error() =
              wire_error(target::ErrorCode::PermissionDenied, host.request_id(), host.trace_id());
        } else if (const auto* parsed = std::get_if<target::TimePoint>(&deadline);
                   parsed == nullptr) {
          *host_response->mutable_error() =
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
            *host_response->mutable_error() =
                wire_error(rejected->code, host.request_id(), host.trace_id());
          } else {
            const auto& profile =
                std::get<target::RuntimeBridgeApplication::BridgePluginProfile>(installed_profile);
            if (!profile.installation) {
              *host_response->mutable_error() = wire_error(
                  target::ErrorCode::PermissionDenied, host.request_id(), host.trace_id());
            } else if (command.logical_call_id.empty() || command.model.empty() ||
                       command.messages.empty()) {
              *host_response->mutable_error() =
                  wire_error(target::ErrorCode::InvalidArgument, host.request_id(),
                             host.trace_id());
            } else {
              command.installation = *profile.installation;
              auto reply = options_.model->chat(command, *parsed);
              if (const auto* failure = std::get_if<target::Failure>(&reply)) {
                const bool retryable = failure->recovery == target::Recovery::QuerySameIdentity ||
                                       failure->code == target::ErrorCode::RateLimited;
                *host_response->mutable_error() =
                    wire_error(failure->code, host.request_id(), host.trace_id(), retryable);
              } else {
                const auto& value = std::get<target::RuntimeModelChatReply>(reply);
                auto* result = host_response->mutable_model_chat();
                result->set_text(value.text);
                result->set_input_tokens(value.input_tokens);
                result->set_output_tokens(value.output_tokens);
              }
            }
          }
        }
        if (!write(*stream, response)) {
          break;
        }
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
  PluginBridgeOptions options_;
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
make_plugin_bridge(target::RuntimeBridgeApplication& application, PluginBridgeOptions options) {
  try {
    return std::unique_ptr<v1::PluginBridge::Service>(
        std::make_unique<PluginBridgeImpl>(application, std::move(options)));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
