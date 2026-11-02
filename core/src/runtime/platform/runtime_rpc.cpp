#include "kiarabot/core/runtime/platform/runtime_rpc.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include "kiarabot/core/runtime/platform/runtime_fence_codec.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/common.pb.h"
#include "kiarabot/v1/events.pb.h"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace codec = target_codec;
namespace t = target;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose,
                   t::Acceptance acceptance = t::Acceptance::NotAccepted) {
  return {code, acceptance, recovery, std::nullopt, std::nullopt};
}
t::Failure internal_failure() {
  return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
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
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return {};
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result += '-';
    }
    result += kDigits[bytes[index] >> 4U];
    result += kDigits[bytes[index] & 0x0fU];
  }
  return result;
}
bool safe_key(std::string_view value) {
  return value.size() >= 16 && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return character >= 33 && character < 127; });
}
// Canonical typed request fingerprint: full payload including idempotency-relevant fields but
// excluding transport identifiers and deadline. Protobuf reflection clears only RequestContext.
t::Sha256Digest fingerprint(const google::protobuf::Message& request) {
  google::protobuf::Message* copy = request.New();
  copy->CopyFrom(request);
  const auto* descriptor = copy->GetDescriptor();
  const auto* reflection = copy->GetReflection();
  const auto* field = descriptor->FindFieldByName("context");
  if (field != nullptr) {
    reflection->ClearField(copy, field);
  }
  const auto serialized = copy->SerializeAsString();
  delete copy;
  t::Sha256Digest digest;
  SHA256(reinterpret_cast<const unsigned char*>(serialized.data()), serialized.size(),
         reinterpret_cast<unsigned char*>(digest.bytes.data()));
  return digest;
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* result = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *result;
    return true;
  }
  return false;
}
grpc::Status error_status(const t::Failure& input, const std::string& request,
                          const std::string& trace) {
  v1::RpcFailure result;
  const bool retryable =
      input.recovery == t::Recovery::Backoff || input.recovery == t::Recovery::QuerySameIdentity;
  const auto error = codec::make_error(input.code, request, trace, retryable);
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
  default:
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
  default:
    context->set_recovery(v1::MANUAL_REVIEW);
    break;
  }
  if (input.retry_after && input.retry_after->count() > 0) {
    context->set_retry_after_ms(
        static_cast<std::uint32_t>(std::min<std::int64_t>(input.retry_after->count(), UINT32_MAX)));
  }
  if (input.operation) {
    auto* operation = context->mutable_operation();
    operation->set_owner_service("Runtime");
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
std::string service_text(t::Service service) {
  switch (service) {
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
  return {};
}
v1::LifecycleState lifecycle(t::LifecycleState value) {
  switch (value) {
  case t::LifecycleState::Active:
    return v1::LIFECYCLE_ACTIVE;
  case t::LifecycleState::Disabled:
    return v1::LIFECYCLE_DISABLED;
  case t::LifecycleState::Deleting:
    return v1::LIFECYCLE_DELETING;
  case t::LifecycleState::Deleted:
    return v1::LIFECYCLE_DELETED;
  case t::LifecycleState::Restoring:
    return v1::LIFECYCLE_RESTORING;
  case t::LifecycleState::Purged:
    return v1::LIFECYCLE_PURGED;
  }
  return v1::LIFECYCLE_UNSPECIFIED;
}
v1::DesiredState desired(t::DesiredState value) {
  return value == t::DesiredState::Running ? v1::DESIRED_RUNNING : v1::DESIRED_STOPPED;
}
v1::ExecutionOutcome execution(t::ExecutionOutcome value) {
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
v1::ConfigApplyState apply_state(t::ConfigApplyState value) {
  switch (value) {
  case t::ConfigApplyState::Pending:
    return v1::CONFIG_APPLY_PENDING;
  case t::ConfigApplyState::Applied:
    return v1::CONFIG_APPLY_APPLIED;
  case t::ConfigApplyState::Rejected:
    return v1::CONFIG_APPLY_REJECTED;
  case t::ConfigApplyState::Unavailable:
    return v1::CONFIG_APPLY_UNAVAILABLE;
  }
  return v1::CONFIG_APPLY_STATE_UNSPECIFIED;
}
bool fence(const t::Fence& value, v1::Fence* output) {
  output->set_instance_id(value.instance.value);
  output->set_runtime_epoch(value.runtime_epoch.value);
  output->set_execution_generation(value.execution_generation.value);
  output->set_connection_generation(value.connection_generation.value);
  return true;
}
bool adapter_fence(const t::AdapterFence& value, v1::Fence* output) {
  const auto encoded = runtime_fence_codec::to_wire(value);
  if (const auto* wire = std::get_if<v1::Fence>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
bool resource(const t::ResourceRef& value, v1::ResourceRef* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<v1::ResourceRef>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
bool component(const t::Capability& value, v1::Capability* output) {
  if (value.name.empty() || value.version.empty()) {
    return false;
  }
  output->set_name(value.name);
  output->set_version(value.version);
  output->set_supported(true);
  return true;
}
bool account(const t::BotAccount& value, v1::AccountRecord* output) {
  output->set_account_id(value.id.value);
  output->set_platform(value.platform);
  output->set_platform_account_id(value.platform_account_id);
  output->set_display_name(value.display_name);
  output->set_version(value.version.value);
  output->set_state(lifecycle(value.state));
  return true;
}
bool installation(const t::PluginInstallation& value, v1::InstallationRecord* output) {
  output->set_installation_id(value.id.value);
  output->set_plugin_type_id(value.plugin_type.value);
  if (!value.manifest.empty()) {
    auto* manifest = output->mutable_manifest();
    if (!manifest->ParseFromString(std::string(reinterpret_cast<const char*>(value.manifest.data()),
                                               value.manifest.size()))) {
      return false;
    }
  }
  output->set_version(value.version.value);
  output->set_state(lifecycle(value.state));
  return true;
}
bool instance_state(const t::PluginInstance& value, v1::VersionedResourceState* output,
                    t::TimePoint observed) {
  if (!resource({t::ResourceKind::Instance, {value.id.value}}, output->mutable_resource())) {
    return false;
  }
  output->set_version(value.version.value);
  output->set_lifecycle(lifecycle(value.state));
  output->set_desired(desired(value.desired));
  if (value.current) {
    if (!fence(*value.current, output->mutable_fence())) {
      return false;
    }
    output->set_registered(true);
  }
  output->set_desired_config_version(value.desired_version.value);
  output->set_applied_config_version(value.applied_config_version.value);
  output->set_process_outcome(v1::EXECUTION_NOT_STARTED);
  if (!timestamp(observed, output->mutable_observed_at())) {
    return false;
  }
  output->set_source_service(service_text(t::Service::Runtime));
  return true;
}
bool instance(const t::PluginInstance& value, v1::InstanceRecord* output, t::TimePoint observed) {
  output->set_instance_id(value.id.value);
  output->set_installation_id(value.installation.value);
  if (value.host) {
    output->set_host_id(value.host->value);
  }
  // P05 models an externally-managed instance as one without a Runtime-assigned managed host.
  output->set_externally_managed(!value.host.has_value());
  return instance_state(value, output->mutable_state(), observed);
}
bool state(const t::ResourceState& value, v1::VersionedResourceState* output) {
  if (!resource(value.resource, output->mutable_resource())) {
    return false;
  }
  output->set_version(value.version.value);
  output->set_lifecycle(lifecycle(value.lifecycle));
  output->set_desired(desired(value.desired));
  if (value.fence && !fence(*value.fence, output->mutable_fence())) {
    return false;
  }
  if (value.adapter_fence && !adapter_fence(*value.adapter_fence, output->mutable_fence())) {
    return false;
  }
  output->set_registered(value.registered);
  output->set_desired_config_version(value.desired_config_version.value);
  output->set_applied_config_version(value.applied_config_version.value);
  output->set_process_outcome(execution(value.process_outcome));
  if (!timestamp(value.observed_at, output->mutable_observed_at())) {
    return false;
  }
  output->set_source_service(service_text(value.source_service));
  return true;
}
bool adapter(const t::AdapterRecord& value, v1::AdapterRecord* output, t::TimePoint observed,
             t::TimePoint clock) {
  if (!resource({t::ResourceKind::Adapter, value.adapter}, output->mutable_resource())) {
    return false;
  }
  output->set_version(value.version.value);
  auto* metadata = output->mutable_last_registration();
  metadata->set_implementation(value.registration.implementation);
  metadata->set_implementation_version(value.registration.implementation_version);
  metadata->set_protocol_version(value.registration.protocol_version);
  for (const auto& capability : value.registration.declared_capabilities) {
    if (!component(capability, metadata->add_declared_capabilities())) {
      return false;
    }
  }
  for (const auto& capability : value.registration.effective_capabilities) {
    if (!component(capability, metadata->add_effective_capabilities())) {
      return false;
    }
  }
  output->set_online(value.online_until > clock);
  if (!timestamp(value.last_registered_at, output->mutable_last_registered_at())) {
    return false;
  }
  if (!timestamp(observed, output->mutable_observed_at())) {
    return false;
  }
  if (!adapter_fence(value.current, output->mutable_current_fence())) {
    return false;
  }
  output->set_lifecycle(lifecycle(value.state));
  return true;
}
bool config(const t::InstanceConfiguration& value, v1::InstanceConfiguration* output,
            t::TimePoint observed) {
  if (!resource({t::ResourceKind::Instance, {value.instance.value}}, output->mutable_instance())) {
    return false;
  }
  output->set_desired_version(value.desired_version.value);
  output->set_applied_version(value.applied_version.value);
  output->set_apply_state(apply_state(value.apply_state));
  output->set_delivery_id(value.delivery_id);
  if (value.fence && !fence(*value.fence, output->mutable_fence())) {
    return false;
  }
  if (!value.values.empty()) {
    auto* values = output->mutable_values();
    if (!values->ParseFromString(
            std::string(reinterpret_cast<const char*>(value.values.data()), value.values.size()))) {
      return false;
    }
  }
  if (!timestamp(observed, output->mutable_observed_at())) {
    return false;
  }
  return true;
}
bool operation(const t::Operation& value, v1::Operation* output, t::TimePoint observed) {
  auto* ref = output->mutable_ref();
  ref->set_owner_service(service_text(value.ref.owner));
  ref->set_operation_id(value.ref.id.value);
  if (!resource(value.ref.resource, ref->mutable_resource())) {
    return false;
  }
  output->set_version(value.version.value);
  output->set_state(operation_state(value.state));
  output->set_execution_outcome(execution(value.execution_outcome));
  if (value.blocked_reason) {
    const auto encoded = codec::to_wire(*value.blocked_reason);
    (void)encoded;
  }
  if (value.next_attempt && !timestamp(*value.next_attempt, output->mutable_next_attempt_at())) {
    return false;
  }
  output->set_stalled(false);
  return timestamp(observed, output->mutable_observed_at());
}
void snapshot(const t::SnapshotWindow& window, const t::PageInfo& page, v1::SnapshotInfo* output) {
  output->set_snapshot_id(window.snapshot_id);
  (void)timestamp(window.observed_at, output->mutable_observed_at());
  (void)timestamp(window.expires_at, output->mutable_expires_at());
  output->set_authorization_version(window.authorization_version.value);
  output->set_owner_service(service_text(t::Service::Runtime));
  output->mutable_page()->set_page_size(page.page_size);
  output->mutable_page()->set_next_page_token(page.next_page_token);
}
struct Prepared {
  t::CommandContext command;
  std::string request;
  std::string trace;
};
t::Outcome<Prepared> prepare(grpc::ServerContext& transport, const v1::RequestContext& input,
                             std::string_view method, const google::protobuf::Message& payload,
                             bool idempotent, const RuntimeRpcOptions& options) {
  std::string request = canonical_uuid(input.request_id()) ? input.request_id() : random_uuid();
  std::string trace = canonical_uuid(input.trace_id()) ? input.trace_id() : random_uuid();
  if (request.empty() || trace.empty() || !canonical_uuid(input.request_id()) ||
      !canonical_uuid(input.trace_id()) || !input.has_deadline()) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const auto decoded = codec::from_wire(input.deadline());
  const auto* requested = std::get_if<t::TimePoint>(&decoded);
  if (!requested) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
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
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  if (!options.authorize) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  auto authenticated = options.authorize(transport, input);
  if (const auto* rejected = std::get_if<t::Failure>(&authenticated)) {
    return *rejected;
  }
  auto authentication = std::move(std::get<t::AuthenticationContext>(authenticated));
  const auto checked = now();
  if (authentication.audience != t::Service::Runtime ||
      !canonical_uuid(authentication.subject.value) ||
      authentication.authorization_version.value == 0 ||
      authentication.verified_at.time_since_epoch().count() <= 0 ||
      authentication.expires_at <= authentication.verified_at ||
      checked >= authentication.expires_at) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  Prepared prepared;
  prepared.request = std::move(request);
  prepared.trace = std::move(trace);
  prepared.command.authentication = std::move(authentication);
  prepared.command.request_id = prepared.request;
  prepared.command.trace_id = prepared.trace;
  prepared.command.method = std::string(method);
  prepared.command.idempotency_key = input.idempotency_key();
  prepared.command.fingerprint = fingerprint(payload);
  prepared.command.deadline = effective;
  return prepared;
}
grpc::Status fail(const Prepared& prepared, const t::Failure& error) {
  return error_status(error, prepared.request, prepared.trace);
}
template <typename Output>
grpc::Status success(Output* context, const Prepared&, const t::Outcome<Output>& result,
                     v1::RuntimeService::Service* = nullptr) = delete;

bool action_status(target::ActionState value, v1::ActionStatus* out) {
  switch (value) {
  case target::ActionState::Accepted:
    *out = v1::ACCEPTED;
    return true;
  case target::ActionState::Succeeded:
    *out = v1::SUCCEEDED;
    return true;
  case target::ActionState::Failed:
    *out = v1::FAILED;
    return true;
  case target::ActionState::Expired:
    *out = v1::EXPIRED;
    return true;
  case target::ActionState::Cancelled:
    *out = v1::CANCELLED;
    return true;
  }
  return false;
}
bool action_outcome(target::ExecutionOutcome value, v1::ExecutionOutcome* out) {
  switch (value) {
  case target::ExecutionOutcome::NotStarted:
    *out = v1::EXECUTION_NOT_STARTED;
    return true;
  case target::ExecutionOutcome::KnownSucceeded:
    *out = v1::EXECUTION_KNOWN_SUCCEEDED;
    return true;
  case target::ExecutionOutcome::KnownFailed:
    *out = v1::EXECUTION_KNOWN_FAILED;
    return true;
  case target::ExecutionOutcome::Unknown:
    *out = v1::EXECUTION_UNKNOWN;
    return true;
  }
  return false;
}
bool action_result(const target::ActionRecord& value, v1::ActionResult* out) {
  const auto snapshot = target::action_wire_snapshot(value);
  if (!snapshot) {
    return false;
  }
  out->set_action_id(value.action_id.value);
  v1::ActionStatus status = v1::ACTION_STATUS_UNSPECIFIED;
  v1::ExecutionOutcome outcome = v1::EXECUTION_OUTCOME_UNSPECIFIED;
  if (!action_status(snapshot->first, &status) || !action_outcome(snapshot->second, &outcome)) {
    return false;
  }
  out->set_status(status);
  out->set_execution_outcome(outcome);
  if (value.platform_message_id) {
    out->set_platform_message_id(*value.platform_message_id);
  }
  if (value.error_code) {
    auto encoded = codec::to_wire(*value.error_code);
    if (const auto* code = std::get_if<v1::ErrorCode>(&encoded)) {
      out->mutable_error()->set_code(*code);
    }
  }
  if (value.dispatch_attempt_id) {
    out->set_dispatch_attempt_id(value.dispatch_attempt_id->value);
  }
  if (value.completed_at) {
    auto completed = codec::to_wire(*value.completed_at);
    if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&completed)) {
      out->mutable_completed_at()->CopyFrom(*stamp);
    }
  }
  return true;
}
bool action_metadata(const target::ActionRecord& value, v1::ActionMetadata* out) {
  out->set_action_id(value.action_id.value);
  out->set_owner_instance_id(value.owner_instance.value);
  out->set_account_id(value.account_id.value);
  v1::ActionStatus status = v1::ACTION_STATUS_UNSPECIFIED;
  v1::ExecutionOutcome outcome = v1::EXECUTION_OUTCOME_UNSPECIFIED;
  if (!action_status(value.state, &status) || !action_outcome(value.execution_outcome, &outcome)) {
    return false;
  }
  out->set_status(status);
  out->set_execution_outcome(outcome);
  auto updated = codec::to_wire(value.updated_at);
  if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&updated)) {
    out->mutable_updated_at()->CopyFrom(*stamp);
  }
  if (value.kind == target::ActionKind::SendMessage) {
    out->set_kind(v1::ACTION_KIND_SEND_MESSAGE);
  } else if (value.kind == target::ActionKind::ReactMessage) {
    out->set_kind(v1::ACTION_KIND_REACT_MESSAGE);
  } else if (value.kind == target::ActionKind::DeleteMessage) {
    out->set_kind(v1::ACTION_KIND_DELETE_MESSAGE);
  } else if (value.kind == target::ActionKind::MuteMember) {
    out->set_kind(v1::ACTION_KIND_MUTE_MEMBER);
  } else if (value.kind == target::ActionKind::KickMember) {
    out->set_kind(v1::ACTION_KIND_KICK_MEMBER);
  } else if (value.kind == target::ActionKind::ApproveRequest) {
    out->set_kind(v1::ACTION_KIND_APPROVE_REQUEST);
  }
  out->set_target_account_id(value.account_id.value);
  out->set_target_conversation_id(value.target_conversation);
  auto created = codec::to_wire(value.created_at);
  if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&created)) {
    out->mutable_created_at()->CopyFrom(*stamp);
  }
  auto deadline = codec::to_wire(value.deadline);
  if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&deadline)) {
    out->mutable_deadline()->CopyFrom(*stamp);
  }
  auto retain = codec::to_wire(value.retain_until);
  if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&retain)) {
    out->mutable_retain_until()->CopyFrom(*stamp);
  }
  out->set_idempotency_key(value.idempotency_key);
  if (value.platform_message_id) {
    out->set_platform_message_id(*value.platform_message_id);
  }
  if (value.dispatch_attempt_id) {
    out->set_dispatch_attempt_id(value.dispatch_attempt_id->value);
  }
  if (value.error_code) {
    auto encoded = codec::to_wire(*value.error_code);
    if (const auto* code = std::get_if<v1::ErrorCode>(&encoded)) {
      out->mutable_error()->set_code(*code);
    }
  }
  return true;
}

// P10b timer handoff conversion. The frozen wire HandoffContext omits actor/authorization_version;
// Runtime binds the actor to the request owner instance and treats the Scheduler authorization
// version as 1 (Scheduler-side evidence stays in the Scheduler journal).
bool handoff_from_wire(const v1::HandoffContext& input, t::HandoffContext* output) {
  if (input.source_service() != "Scheduler" || input.operation().owner_service() != "Scheduler" ||
      input.payload_sha256().size() != t::Sha256Digest{}.bytes.size()) {
    return false;
  }
  const auto operation_resource = codec::from_wire(input.operation().resource());
  const auto* operation_resource_value = std::get_if<t::ResourceRef>(&operation_resource);
  const auto resource = codec::from_wire(input.resource());
  const auto* resource_value = std::get_if<t::ResourceRef>(&resource);
  const auto created = codec::from_wire(input.created_at());
  const auto* created_value = std::get_if<t::TimePoint>(&created);
  const auto valid = codec::from_wire(input.valid_until());
  const auto* valid_value = std::get_if<t::TimePoint>(&valid);
  if (!operation_resource_value || !resource_value || !created_value || !valid_value ||
      !canonical_uuid(input.message_id()) || input.operation().operation_id().empty()) {
    return false;
  }
  output->source = t::Service::Scheduler;
  output->message = t::MessageId{input.message_id()};
  output->operation =
      t::OperationRef{t::Service::Scheduler, t::OperationId{input.operation().operation_id()},
                      *operation_resource_value};
  if (!input.causation_id().empty()) {
    output->causation = t::MessageId{input.causation_id()};
  }
  output->resource = *resource_value;
  output->version = t::ResourceVersion{input.resource_version()};
  output->actor = t::SubjectId{};
  output->authorization_version = t::ResourceVersion{1};
  std::memcpy(output->payload_hash.bytes.data(), input.payload_sha256().data(),
              output->payload_hash.bytes.size());
  output->created_at = *created_value;
  output->valid_until = *valid_value;
  return true;
}
bool timer_acceptance_wire(const t::TimerTriggerAcceptance& value, v1::TimerAcceptance* output) {
  output->set_occurrence_id(value.occurrence_id.value);
  output->set_event_id(value.event_id.value);
  output->set_duplicate(value.duplicate);
  const auto persisted = codec::to_wire(value.persisted_at);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&persisted)) {
    *output->mutable_persisted_at() = *wire;
    return true;
  }
  return false;
}
std::optional<std::string> timer_envelope(const v1::RuntimeAcceptTimerTriggerRequest& request,
                                          const t::HandoffContext& handoff,
                                          std::string_view trace_id) {
  v1::EventEnvelope envelope;
  envelope.set_event_id(request.occurrence_id());
  envelope.set_schema_version("v1");
  envelope.set_trace_id(std::string(trace_id));
  envelope.set_delivery_mode(v1::AT_LEAST_ONCE);
  envelope.mutable_source()->set_adapter_id("scheduler");
  const auto occurred = codec::to_wire(handoff.created_at);
  if (const auto* occurred_wire = std::get_if<google::protobuf::Timestamp>(&occurred)) {
    *envelope.mutable_occurred_at() = *occurred_wire;
  } else {
    return std::nullopt;
  }
  auto* timer = envelope.mutable_timer_triggered();
  timer->set_task_id(request.task_id());
  timer->set_owner_plugin_id(request.owner_instance_id());
  timer->set_occurrence_id(request.occurrence_id());
  timer->set_payload(request.payload());
  const auto scheduled = codec::to_wire(handoff.created_at);
  if (const auto* scheduled_wire = std::get_if<google::protobuf::Timestamp>(&scheduled)) {
    *timer->mutable_scheduled_at() = *scheduled_wire;
  } else {
    return std::nullopt;
  }
  std::string serialized;
  if (!envelope.SerializeToString(&serialized)) {
    return std::nullopt;
  }
  return serialized;
}

class RuntimeRpc final : public v1::RuntimeService::Service {
public:
  RuntimeRpc(t::RuntimeResourceApplication& application, RuntimeRpcOptions options)
      : application_(application), options_(std::move(options)) {}

  grpc::Status CreateAccount(grpc::ServerContext* transport,
                             const v1::RuntimeCreateAccountRequest* request,
                             v1::RuntimeCreateAccountResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/CreateAccount", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      std::string id = canonical_uuid(request->context().request_id())
                           ? request->context().request_id()
                           : random_uuid();
      return error_status(*rejected, id, id);
    }
    auto& context = std::get<Prepared>(prepared);
    auto created =
        application_.create_account({context.command, request->platform(),
                                     request->platform_account_id(), request->display_name()});
    if (const auto* rejected = std::get_if<t::Failure>(&created)) {
      return fail(context, *rejected);
    }
    if (!account(std::get<t::BotAccount>(created), response->mutable_account())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status UpdateAccount(grpc::ServerContext* transport,
                             const v1::RuntimeUpdateAccountRequest* request,
                             v1::RuntimeUpdateAccountResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/UpdateAccount", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    context.command.expected_version =
        request->has_expected_version()
            ? std::optional<t::ResourceVersion>{t::ResourceVersion{request->expected_version()}}
            : std::nullopt;
    auto updated = application_.update_account(
        {context.command, t::ResourceId{request->account_id()}, request->display_name()});
    if (const auto* rejected = std::get_if<t::Failure>(&updated)) {
      return fail(context, *rejected);
    }
    const auto& mutation = std::get<t::AccountMutation>(updated);
    if (!account(mutation.account, response->mutable_account())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status DisableAccount(grpc::ServerContext* transport,
                              const v1::RuntimeDisableAccountRequest* request,
                              v1::RuntimeDisableAccountResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/DisableAccount", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    context.command.expected_version =
        request->has_expected_version()
            ? std::optional<t::ResourceVersion>{t::ResourceVersion{request->expected_version()}}
            : std::nullopt;
    auto disabled =
        application_.disable_account({context.command, t::ResourceId{request->account_id()}});
    if (const auto* rejected = std::get_if<t::Failure>(&disabled)) {
      return fail(context, *rejected);
    }
    const auto& mutation = std::get<t::AccountMutation>(disabled);
    if (!account(mutation.account, response->mutable_account()) ||
        (mutation.operation &&
         !operation_ref(mutation.operation->operation, response->mutable_operation()))) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetAccount(grpc::ServerContext* transport,
                          const v1::RuntimeGetAccountRequest* request,
                          v1::RuntimeGetAccountResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/GetAccount",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = application_.get_account({context.command, wire->id});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!account(std::get<t::BotAccount>(found), response->mutable_account())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status ListAccounts(grpc::ServerContext* transport,
                            const v1::RuntimeListAccountsRequest* request,
                            v1::RuntimeListAccountsResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/ListAccounts",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto rejection = catalog(request->catalog());
    if (rejection) {
      return fail(context, *rejection);
    }
    t::PageRequest page;
    page.page_size = request->page().page_size();
    page.page_token = request->page().page_token();
    auto listed = application_.list_accounts({context.command, page});
    if (const auto* rejected = std::get_if<t::Failure>(&listed)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::ListResult<t::BotAccount>>(listed);
    for (const auto& item : result.items) {
      if (!account(item, response->add_accounts())) {
        return error_status(internal_failure(), context.request, context.trace);
      }
    }
    snapshot(result.snapshot, result.page, response->mutable_snapshot());
    return grpc::Status::OK;
  }
  grpc::Status BindAdapter(grpc::ServerContext* transport,
                           const v1::RuntimeBindAdapterRequest* request,
                           v1::RuntimeBindAdapterResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/BindAdapter", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    context.command.expected_version =
        request->has_expected_version()
            ? std::optional<t::ResourceVersion>{t::ResourceVersion{request->expected_version()}}
            : std::nullopt;
    auto bound =
        application_.bind_adapter({context.command, t::ResourceId{request->binding().account_id()},
                                   t::InstanceId{request->binding().adapter_instance_id()}});
    if (const auto* rejected = std::get_if<t::Failure>(&bound)) {
      return fail(context, *rejected);
    }
    const auto& mutation = std::get<t::BindingMutation>(bound);
    auto* wire = response->mutable_binding();
    wire->set_account_id(mutation.binding.account.value);
    wire->set_adapter_instance_id(mutation.binding.adapter_instance.value);
    wire->set_version(mutation.binding.version.value);
    if (mutation.bootstrap_credential) {
      response->set_bootstrap_credential(*mutation.bootstrap_credential);
    }
    if (mutation.operation &&
        !operation_ref(mutation.operation->operation, response->mutable_operation())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status UnbindAdapter(grpc::ServerContext* transport,
                             const v1::RuntimeUnbindAdapterRequest* request,
                             v1::RuntimeUnbindAdapterResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/UnbindAdapter", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    context.command.expected_version =
        request->has_expected_version()
            ? std::optional<t::ResourceVersion>{t::ResourceVersion{request->expected_version()}}
            : std::nullopt;
    auto bound = application_.unbind_adapter(
        {context.command, t::ResourceId{request->binding().account_id()}});
    if (const auto* rejected = std::get_if<t::Failure>(&bound)) {
      return fail(context, *rejected);
    }
    const auto& mutation = std::get<t::BindingMutation>(bound);
    auto* wire = response->mutable_binding();
    wire->set_account_id(mutation.binding.account.value);
    wire->set_adapter_instance_id(mutation.binding.adapter_instance.value);
    wire->set_version(mutation.binding.version.value);
    if (mutation.operation &&
        !operation_ref(mutation.operation->operation, response->mutable_operation())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status ValidateManifest(grpc::ServerContext* transport,
                                const v1::RuntimeValidateManifestRequest* request,
                                v1::RuntimeValidateManifestResponse* response) override {
    auto prepared =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/ValidateManifest",
                *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    std::vector<std::byte> manifest(request->manifest().ByteSizeLong());
    if (!manifest.empty() &&
        !request->manifest().SerializeToArray(reinterpret_cast<char*>(manifest.data()),
                                              static_cast<int>(manifest.size()))) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    auto validation = application_.validate_manifest({context.command, manifest});
    if (const auto* rejected = std::get_if<t::Failure>(&validation)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::ManifestValidation>(validation);
    response->set_valid(result.valid);
    for (const auto& violation : result.violations) {
      const auto error = codec::make_error(violation.code, context.request, context.trace,
                                           violation.recovery == t::Recovery::Backoff);
      if (const auto* wire = std::get_if<v1::Error>(&error)) {
        *response->add_violations() = *wire;
      }
    }
    return grpc::Status::OK;
  }
  grpc::Status CreateInstallation(grpc::ServerContext* transport,
                                  const v1::RuntimeCreateInstallationRequest* request,
                                  v1::RuntimeCreateInstallationResponse* response) override {
    auto prepared =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/CreateInstallation",
                *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    std::vector<std::byte> manifest(request->manifest().ByteSizeLong());
    if (!manifest.empty() &&
        !request->manifest().SerializeToArray(reinterpret_cast<char*>(manifest.data()),
                                              static_cast<int>(manifest.size()))) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    auto created =
        application_.create_installation({context.command, request->plugin_type_id(), manifest});
    if (const auto* rejected = std::get_if<t::Failure>(&created)) {
      return fail(context, *rejected);
    }
    if (!installation(std::get<t::PluginInstallation>(created), response->mutable_installation())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetInstallation(grpc::ServerContext* transport,
                               const v1::RuntimeGetInstallationRequest* request,
                               v1::RuntimeGetInstallationResponse* response) override {
    auto query = prepare(*transport, request->context(),
                         "kiarabot.v1.RuntimeService/GetInstallation", *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found =
        application_.get_installation({context.command, t::InstallationId{wire->id.value}});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!installation(std::get<t::PluginInstallation>(found), response->mutable_installation()) ||
        !timestamp(now(), response->mutable_observed_at())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status ListInstallations(grpc::ServerContext* transport,
                                 const v1::RuntimeListInstallationsRequest* request,
                                 v1::RuntimeListInstallationsResponse* response) override {
    auto query = prepare(*transport, request->context(),
                         "kiarabot.v1.RuntimeService/ListInstallations", *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto rejection = catalog(request->catalog());
    if (rejection) {
      return fail(context, *rejection);
    }
    t::PageRequest page;
    page.page_size = request->page().page_size();
    page.page_token = request->page().page_token();
    auto listed = application_.list_installations({context.command, page});
    if (const auto* rejected = std::get_if<t::Failure>(&listed)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::ListResult<t::PluginInstallation>>(listed);
    for (const auto& item : result.items) {
      if (!installation(item, response->add_installations())) {
        return error_status(internal_failure(), context.request, context.trace);
      }
    }
    snapshot(result.snapshot, result.page, response->mutable_snapshot());
    return grpc::Status::OK;
  }
  grpc::Status CreateInstance(grpc::ServerContext* transport,
                              const v1::RuntimeCreateInstanceRequest* request,
                              v1::RuntimeCreateInstanceResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/CreateInstance", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    // P05 does not yet emit the P06 bootstrap_credential field (pending C1 contract integration).
    std::optional<t::ResourceId> host;
    if (!request->host_id().empty()) {
      host = t::ResourceId{request->host_id()};
    }
    auto created = application_.create_instance({context.command,
                                                 t::InstallationId{request->installation_id()},
                                                 host, request->externally_managed()});
    if (const auto* rejected = std::get_if<t::Failure>(&created)) {
      return fail(context, *rejected);
    }
    const auto& creation = std::get<t::InstanceCreation>(created);
    if (!instance(creation.instance, response->mutable_instance(), now())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    if (creation.bootstrap_credential) {
      response->set_bootstrap_credential(*creation.bootstrap_credential);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetInstance(grpc::ServerContext* transport,
                           const v1::RuntimeGetInstanceRequest* request,
                           v1::RuntimeGetInstanceResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/GetInstance",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = application_.get_instance({context.command, t::InstanceId{wire->id.value}});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!instance(std::get<t::PluginInstance>(found), response->mutable_instance(), now()) ||
        !timestamp(now(), response->mutable_observed_at())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status ListInstances(grpc::ServerContext* transport,
                             const v1::RuntimeListInstancesRequest* request,
                             v1::RuntimeListInstancesResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/ListInstances",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto rejection = catalog(request->catalog());
    if (rejection) {
      return fail(context, *rejection);
    }
    t::PageRequest page;
    page.page_size = request->page().page_size();
    page.page_token = request->page().page_token();
    auto listed = application_.list_instances({context.command, page});
    if (const auto* rejected = std::get_if<t::Failure>(&listed)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::ListResult<t::PluginInstance>>(listed);
    const auto observed = now();
    for (const auto& item : result.items) {
      v1::InstanceRecord record;
      if (!instance(item, &record, observed)) {
        return error_status(internal_failure(), context.request, context.trace);
      }
      *response->add_instances() = std::move(record);
    }
    snapshot(result.snapshot, result.page, response->mutable_snapshot());
    return grpc::Status::OK;
  }
  grpc::Status GetInstanceConfig(grpc::ServerContext* transport,
                                 const v1::RuntimeGetInstanceConfigRequest* request,
                                 v1::RuntimeGetInstanceConfigResponse* response) override {
    auto query = prepare(*transport, request->context(),
                         "kiarabot.v1.RuntimeService/GetInstanceConfig", *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = application_.get_instance_config({context.command, t::InstanceId{wire->id.value}});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!config(std::get<t::InstanceConfiguration>(found), response->mutable_config(), now())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status SetDesiredState(grpc::ServerContext* transport,
                               const v1::RuntimeSetDesiredStateRequest* request,
                               v1::RuntimeSetDesiredStateResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/SetDesiredState", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    context.command.expected_version =
        request->has_expected_version()
            ? std::optional<t::ResourceVersion>{t::ResourceVersion{request->expected_version()}}
            : std::nullopt;
    const auto desired_state = request->desired_state() == v1::DESIRED_RUNNING
                                   ? t::DesiredState::Running
                                   : t::DesiredState::Stopped;
    if (request->desired_state() != v1::DESIRED_RUNNING &&
        request->desired_state() != v1::DESIRED_STOPPED) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto changed = application_.apply_desired_state(
        {context.command, t::InstanceId{request->instance_id()}, desired_state});
    if (const auto* rejected = std::get_if<t::Failure>(&changed)) {
      return fail(context, *rejected);
    }
    const auto& mutation = std::get<t::InstanceMutation>(changed);
    if (!instance(mutation.instance, response->mutable_instance(), now()) ||
        (mutation.operation &&
         !operation_ref(mutation.operation->operation, response->mutable_operation()))) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status UpdateConfig(grpc::ServerContext* transport,
                            const v1::RuntimeUpdateConfigRequest* request,
                            v1::RuntimeUpdateConfigResponse* response) override {
    auto prepared = prepare(*transport, request->context(),
                            "kiarabot.v1.RuntimeService/UpdateConfig", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    context.command.expected_version =
        request->has_expected_version()
            ? std::optional<t::ResourceVersion>{t::ResourceVersion{request->expected_version()}}
            : std::nullopt;
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire || wire->kind != t::ResourceKind::Instance) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    std::vector<std::byte> values(request->config().ByteSizeLong());
    if (!values.empty() &&
        !request->config().SerializeToArray(reinterpret_cast<char*>(values.data()),
                                            static_cast<int>(values.size()))) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    auto changed =
        application_.update_config({context.command, t::InstanceId{wire->id.value}, values});
    if (const auto* rejected = std::get_if<t::Failure>(&changed)) {
      return fail(context, *rejected);
    }
    const auto& mutation = std::get<t::ConfigMutation>(changed);
    t::ResourceState observed{
        t::ResourceRef{t::ResourceKind::Instance, {mutation.config.instance.value}},
        mutation.config.version,
        t::LifecycleState::Active,
        t::DesiredState::Running,
        mutation.config.fence,
        std::nullopt,
        mutation.config.fence.has_value(),
        mutation.config.desired_version,
        mutation.config.applied_version,
        t::ExecutionOutcome::NotStarted,
        mutation.config.observed_at,
        t::Service::Runtime};
    if (!state(observed, response->mutable_state()) ||
        (mutation.operation &&
         !operation_ref(mutation.operation->operation, response->mutable_operation()))) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetResourceState(grpc::ServerContext* transport,
                                const v1::RuntimeGetResourceStateRequest* request,
                                v1::RuntimeGetResourceStateResponse* response) override {
    auto query = prepare(*transport, request->context(),
                         "kiarabot.v1.RuntimeService/GetResourceState", *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = application_.get_resource_state({context.command, *wire});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!state(std::get<t::ResourceState>(found), response->mutable_state())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext* transport,
                            const v1::RuntimeGetOperationRequest* request,
                            v1::RuntimeGetOperationResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/GetOperation",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto& wire = request->operation();
    if (wire.owner_service() != "Runtime" || !canonical_uuid(wire.operation_id())) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    const auto reference = codec::from_wire(wire.resource());
    const auto* resource = std::get_if<t::ResourceRef>(&reference);
    if (!resource) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = application_.get_operation(
        {context.command, {{t::Service::Runtime}, {wire.operation_id()}, *resource}});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!operation(std::get<t::Operation>(found), response->mutable_operation(), now())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetAdapter(grpc::ServerContext* transport,
                          const v1::RuntimeGetAdapterRequest* request,
                          v1::RuntimeGetAdapterResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/GetAdapter",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto reference = codec::from_wire(request->resource());
    const auto* wire = std::get_if<t::ResourceRef>(&reference);
    if (!wire) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = application_.get_adapter({context.command, wire->id});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    const auto observed = now();
    if (!adapter(std::get<t::AdapterRecord>(found), response->mutable_adapter(), observed,
                 observed) ||
        !timestamp(observed, response->mutable_observed_at())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status ListAdapters(grpc::ServerContext* transport,
                            const v1::RuntimeListAdaptersRequest* request,
                            v1::RuntimeListAdaptersResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/ListAdapters",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    const auto rejection = catalog(request->catalog());
    if (rejection) {
      return fail(context, *rejection);
    }
    t::PageRequest page;
    page.page_size = request->page().page_size();
    page.page_token = request->page().page_token();
    auto listed = application_.list_adapters({context.command, page});
    if (const auto* rejected = std::get_if<t::Failure>(&listed)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::ListResult<t::AdapterRecord>>(listed);
    const auto observed = now();
    for (const auto& item : result.items) {
      v1::AdapterRecord record;
      if (!adapter(item, &record, observed, observed)) {
        return error_status(internal_failure(), context.request, context.trace);
      }
      *response->add_adapters() = std::move(record);
    }
    snapshot(result.snapshot, result.page, response->mutable_snapshot());
    return grpc::Status::OK;
  }

  grpc::Status GetStorage(grpc::ServerContext* transport,
                          const v1::RuntimeGetStorageRequest* request,
                          v1::RuntimeGetStorageResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/GetStorage",
                         *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    t::StorageCommandContext command;
    if (!storage_command(context, *request, command) || request->key().empty()) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    auto found = options_.storage->get_storage({command, request->key()});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    if (!storage_entry(std::get<t::StorageEntryResult>(found), request->key(),
                       response->mutable_entry())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status SetStorage(grpc::ServerContext* transport,
                          const v1::RuntimeSetStorageRequest* request,
                          v1::RuntimeSetStorageResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/SetStorage",
                         *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    t::StorageCommandContext command;
    if (!storage_command(context, *request, command) || request->key().empty()) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    std::optional<t::TimePoint> expires;
    if (request->has_expires_at()) {
      const auto decoded = codec::from_wire(request->expires_at());
      const auto* value = std::get_if<t::TimePoint>(&decoded);
      if (!value) {
        return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
      }
      expires = *value;
    }
    std::vector<std::byte> bytes(request->value().size());
    std::memcpy(bytes.data(), request->value().data(), request->value().size());
    // The unary internal Set has no condition field; shared writes require one and therefore stay
    // rejected (the bridge frame path carries it). Private Set must omit it by contract.
    auto written = options_.storage->set_storage(
        {command, request->key(), std::move(bytes), expires, std::nullopt});
    if (const auto* rejected = std::get_if<t::Failure>(&written)) {
      return fail(context, *rejected);
    }
    if (!storage_entry(std::get<t::StorageEntryResult>(written), request->key(),
                       response->mutable_entry())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status DeleteStorage(grpc::ServerContext* transport,
                             const v1::RuntimeDeleteStorageRequest* request,
                             v1::RuntimeDeleteStorageResponse* response) override {
    auto query = prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/DeleteStorage",
                         *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    t::StorageCommandContext command;
    if (!storage_command(context, *request, command) || request->key().empty()) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    std::optional<t::CompareCondition> condition;
    if (request->has_condition()) {
      auto decoded = storage_condition(request->condition());
      if (!decoded) {
        return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
      }
      condition = *decoded;
    }
    auto removed = options_.storage->delete_storage({command, request->key(), condition});
    if (const auto* rejected = std::get_if<t::Failure>(&removed)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::StorageDeleteResult>(removed);
    response->set_deleted(result.deleted);
    if (result.version) {
      response->set_version(result.version->value);
    }
    return grpc::Status::OK;
  }
  grpc::Status CompareAndSetStorage(grpc::ServerContext* transport,
                                    const v1::RuntimeCompareAndSetStorageRequest* request,
                                    v1::RuntimeCompareAndSetStorageResponse* response) override {
    auto query =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/CompareAndSetStorage",
                *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    t::StorageCommandContext command;
    if (!storage_command(context, *request, command) || request->key().empty() ||
        !request->has_condition()) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    auto decoded = storage_condition(request->condition());
    if (!decoded) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    std::optional<t::TimePoint> expires;
    if (request->has_expires_at()) {
      const auto typed = codec::from_wire(request->expires_at());
      const auto* value = std::get_if<t::TimePoint>(&typed);
      if (!value) {
        return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
      }
      expires = *value;
    }
    std::vector<std::byte> bytes(request->value().size());
    std::memcpy(bytes.data(), request->value().data(), request->value().size());
    auto written = options_.storage->compare_and_set_storage(
        {command, request->key(), std::move(bytes), *decoded, expires});
    if (const auto* rejected = std::get_if<t::Failure>(&written)) {
      return fail(context, *rejected);
    }
    if (!storage_entry(std::get<t::StorageEntryResult>(written), request->key(),
                       response->mutable_entry())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status CreateSharedNamespace(grpc::ServerContext* transport,
                                     const v1::RuntimeCreateSharedNamespaceRequest* request,
                                     v1::RuntimeCreateSharedNamespaceResponse* response) override {
    auto query =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/CreateSharedNamespace",
                *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    if (!canonical_uuid(request->installation_id()) ||
        (request->has_expected_version() && request->expected_version() == 0)) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    std::optional<t::ResourceVersion> expected;
    if (request->has_expected_version()) {
      expected = t::ResourceVersion{request->expected_version()};
    }
    auto created = options_.storage->create_shared_namespace(
        {context.command, {request->installation_id()}, expected});
    if (const auto* rejected = std::get_if<t::Failure>(&created)) {
      return fail(context, *rejected);
    }
    if (!namespace_record(std::get<t::NamespaceMutation>(created).namespace_record,
                          response->mutable_namespace_())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }
  grpc::Status GetSharedNamespace(grpc::ServerContext* transport,
                                  const v1::RuntimeGetSharedNamespaceRequest* request,
                                  v1::RuntimeGetSharedNamespaceResponse* response) override {
    auto query =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/GetSharedNamespace",
                *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    if (!canonical_uuid(request->namespace_id())) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    auto found =
        options_.storage->get_shared_namespace({context.command, {request->namespace_id()}});
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return fail(context, *rejected);
    }
    const auto& view = std::get<t::NamespaceView>(found);
    if (!namespace_record(view.namespace_record, response->mutable_namespace_())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    for (const auto& member : view.members) {
      auto* output = response->add_members();
      output->set_instance_id(member.instance.value);
      output->set_can_read(member.can_read);
      output->set_can_write(member.can_write);
    }
    auto* quota = response->mutable_quota();
    quota->set_namespace_keys_used(view.quota.namespace_keys_used);
    quota->set_namespace_keys_limit(t::kMaximumNamespaceKeys);
    quota->set_namespace_bytes_used(view.quota.namespace_bytes_used);
    quota->set_namespace_bytes_limit(t::kMaximumNamespaceBytes);
    quota->set_installation_keys_used(view.quota.installation_keys_used);
    quota->set_installation_keys_limit(t::kMaximumInstallationKeys);
    quota->set_installation_bytes_used(view.quota.installation_bytes_used);
    quota->set_installation_bytes_limit(t::kMaximumInstallationBytes);
    return grpc::Status::OK;
  }
  grpc::Status SetSharedMembers(grpc::ServerContext* transport,
                                const v1::RuntimeSetSharedMembersRequest* request,
                                v1::RuntimeSetSharedMembersResponse* response) override {
    auto query = prepare(*transport, request->context(),
                         "kiarabot.v1.RuntimeService/SetSharedMembers", *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    if (!canonical_uuid(request->namespace_id()) || !canonical_uuid(request->installation_id()) ||
        request->members_size() > 1024 ||
        (request->has_expected_version() && request->expected_version() == 0)) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    std::vector<t::SharedMember> members;
    for (const auto& member : request->members()) {
      if (!canonical_uuid(member.instance_id())) {
        return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
      }
      members.push_back({{member.instance_id()}, member.can_read(), member.can_write()});
    }
    std::optional<t::ResourceVersion> expected;
    if (request->has_expected_version()) {
      expected = t::ResourceVersion{request->expected_version()};
    }
    auto updated = options_.storage->set_shared_members({context.command,
                                                         {request->namespace_id()},
                                                         {request->installation_id()},
                                                         std::move(members),
                                                         expected});
    if (const auto* rejected = std::get_if<t::Failure>(&updated)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::SharedMembersMutation>(updated);
    response->set_version(result.version.value);
    operation_ref(result.operation, response->mutable_operation());
    return grpc::Status::OK;
  }
  grpc::Status
  DisableSharedNamespace(grpc::ServerContext* transport,
                         const v1::RuntimeDisableSharedNamespaceRequest* request,
                         v1::RuntimeDisableSharedNamespaceResponse* response) override {
    auto query =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/DisableSharedNamespace",
                *request, true, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&query)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(query);
    if (!options_.storage) {
      return fail(context, failure(t::ErrorCode::CapabilityNotSupported));
    }
    if (!canonical_uuid(request->namespace_id()) ||
        (request->has_expected_version() && request->expected_version() == 0)) {
      return fail(context, failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
    }
    std::optional<t::ResourceVersion> expected;
    if (request->has_expected_version()) {
      expected = t::ResourceVersion{request->expected_version()};
    }
    auto disabled = options_.storage->disable_shared_namespace(
        {context.command, {request->namespace_id()}, expected});
    if (const auto* rejected = std::get_if<t::Failure>(&disabled)) {
      return fail(context, *rejected);
    }
    const auto& result = std::get<t::SharedMembersMutation>(disabled);
    response->set_version(result.version.value);
    operation_ref(result.operation, response->mutable_operation());
    return grpc::Status::OK;
  }
  grpc::Status AcceptTimerTrigger(grpc::ServerContext* transport,
                                  const v1::RuntimeAcceptTimerTriggerRequest* request,
                                  v1::RuntimeAcceptTimerTriggerResponse* response) override {
    auto prepared =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/AcceptTimerTrigger",
                *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    t::HandoffContext handoff;
    if (!handoff_from_wire(request->handoff(), &handoff)) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    handoff.actor = t::SubjectId{request->owner_instance_id()};
    const auto kind = request->target().type() == v1::ConversationRef::PRIVATE
                          ? t::ConversationKey::Kind::Private
                      : request->target().type() == v1::ConversationRef::GROUP
                          ? t::ConversationKey::Kind::Group
                          : t::ConversationKey::Kind::Unspecified;
    auto envelope = timer_envelope(*request, handoff, context.trace);
    if (!envelope) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    t::AcceptTimerTriggerRequest application_request;
    application_request.context = context.command;
    application_request.handoff = handoff;
    application_request.occurrence_id = t::MessageId{request->occurrence_id()};
    application_request.task_id = request->task_id();
    application_request.owner_instance_id = t::InstanceId{request->owner_instance_id()};
    application_request.schedule_version = t::ResourceVersion{request->schedule_version()};
    application_request.conversation_id = request->target().id();
    application_request.conversation_kind = kind;
    application_request.payload.reserve(request->payload().size());
    for (const unsigned char byte : request->payload()) {
      application_request.payload.push_back(static_cast<std::byte>(byte));
    }
    application_request.serialized_envelope = std::move(*envelope);
    auto accepted = application_.accept_timer_trigger(application_request);
    if (const auto* rejected = std::get_if<t::Failure>(&accepted)) {
      return fail(context, *rejected);
    }
    if (!timer_acceptance_wire(std::get<t::TimerTriggerAcceptance>(accepted),
                               response->mutable_receipt())) {
      return error_status(internal_failure(), context.request, context.trace);
    }
    return grpc::Status::OK;
  }

  grpc::Status QueryActionResult(grpc::ServerContext* transport,
                                 const v1::RuntimeQueryActionResultRequest* request,
                                 v1::RuntimeQueryActionResultResponse* response) override {
    auto prepared =
        prepare(*transport, request->context(), "kiarabot.v1.RuntimeService/QueryActionResult",
                *request, false, options_);
    if (const auto* rejected = std::get_if<t::Failure>(&prepared)) {
      return error_status(*rejected, request->context().request_id(),
                          request->context().trace_id());
    }
    auto& context = std::get<Prepared>(prepared);
    if (options_.actions == nullptr) {
      return error_status(failure(t::ErrorCode::CapabilityNotSupported), context.request,
                          context.trace);
    }
    const auto& account_scope = request->account_scope();
    if (account_scope.resources_size() != 1 ||
        account_scope.resources(0).kind() != v1::RESOURCE_ACCOUNT ||
        account_scope.resources(0).id().empty()) {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    t::ActionControlResultQuery query;
    query.authentication = context.command.authentication;
    query.request_id = context.command.request_id;
    query.trace_id = context.command.trace_id;
    query.method = context.command.method;
    query.deadline = context.command.deadline;
    query.account_id = t::ResourceId{account_scope.resources(0).id()};
    if (request->selector_case() == v1::RuntimeQueryActionResultRequest::kActionId) {
      if (request->action_id().empty()) {
        return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                            context.request, context.trace);
      }
      query.action_id = t::ActionId{request->action_id()};
    } else if (request->selector_case() == v1::RuntimeQueryActionResultRequest::kIdempotencyKey) {
      if (request->idempotency_key().empty() || request->owner_instance_id().empty()) {
        return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                            context.request, context.trace);
      }
      query.idempotency_key = request->idempotency_key();
      query.owner_instance = t::InstanceId{request->owner_instance_id()};
    } else {
      return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                          context.request, context.trace);
    }
    auto found = options_.actions->query_control_action(query);
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return error_status(*rejected, context.request, context.trace);
    }
    const auto& record = std::get<t::ActionRecord>(found);
    if (!action_result(record, response->mutable_result()) ||
        !action_metadata(record, response->mutable_metadata())) {
      return error_status(failure(t::ErrorCode::Internal), context.request, context.trace);
    }
    v1::ExecutionOutcome outcome = v1::EXECUTION_OUTCOME_UNSPECIFIED;
    if (!action_outcome(record.execution_outcome, &outcome)) {
      return error_status(failure(t::ErrorCode::Internal), context.request, context.trace);
    }
    response->set_execution_outcome(outcome);
    return grpc::Status::OK;
  }

private:
  template <typename Request>
  bool storage_command(const Prepared& prepared, const Request& request,
                       t::StorageCommandContext& command) {
    const auto& fence = request.fence();
    if (!canonical_uuid(fence.instance_id())) {
      return false;
    }
    command.authentication = prepared.command.authentication;
    command.request_id = prepared.command.request_id;
    command.trace_id = prepared.command.trace_id;
    command.method = prepared.command.method;
    command.idempotency_key = prepared.command.idempotency_key;
    command.fingerprint = prepared.command.fingerprint;
    command.deadline = prepared.command.deadline;
    command.instance = {fence.instance_id()};
    if (request.has_scope()) {
      switch (request.scope().scope_case()) {
      case v1::NamespaceScope::SCOPE_NOT_SET:
        command.requested = t::RequestedScopeKind::DefaultPrivate;
        break;
      case v1::NamespaceScope::kInstancePrivate:
        if (!request.scope().instance_private()) {
          return false;
        }
        command.requested = t::RequestedScopeKind::DefaultPrivate;
        break;
      case v1::NamespaceScope::kInstallationShared:
        if (!request.scope().installation_shared()) {
          return false;
        }
        command.requested = t::RequestedScopeKind::InstallationShared;
        break;
      case v1::NamespaceScope::kInstallationSharedId:
        if (!canonical_uuid(request.scope().installation_shared_id())) {
          return false;
        }
        command.requested = t::RequestedScopeKind::InstallationSharedId;
        command.shared_namespace_id = {request.scope().installation_shared_id()};
        break;
      }
    }
    return true;
  }
  static std::optional<t::CompareCondition> storage_condition(const v1::StorageCondition& wire) {
    switch (wire.expected_case()) {
    case v1::StorageCondition::EXPECTED_NOT_SET:
      return std::nullopt;
    case v1::StorageCondition::kAbsent:
      if (!wire.absent()) {
        return std::nullopt;
      }
      return t::CompareCondition{t::ExpectedAbsent{}};
    case v1::StorageCondition::kVersion:
      if (wire.version() == 0) {
        return std::nullopt;
      }
      return t::CompareCondition{t::ResourceVersion{wire.version()}};
    }
    return std::nullopt;
  }
  static bool storage_entry(const t::StorageEntryResult& value, const std::string& key,
                            v1::StorageEntry* output) {
    if (!output) {
      return false;
    }
    output->set_key(key);
    output->set_value(value.value.bytes.data(), value.value.bytes.size());
    output->set_version(value.value.version.value);
    if (value.value.expires_at) {
      return timestamp(*value.value.expires_at, output->mutable_expires_at());
    }
    return true;
  }
  static bool namespace_record(const t::SharedNamespaceRecord& value, v1::NamespaceRecord* output) {
    if (!output) {
      return false;
    }
    output->set_namespace_id(value.namespace_id.value);
    output->set_installation_id(value.installation.value);
    output->set_version(value.version.value);
    output->set_disabled(value.disabled);
    return timestamp(value.created_at, output->mutable_created_at()) &&
           timestamp(value.updated_at, output->mutable_updated_at());
  }
  std::optional<t::Failure> catalog(const v1::ResourceRef& input) const {
    const auto decoded = codec::from_wire(input);
    if (const auto* failure = std::get_if<t::Failure>(&decoded)) {
      return *failure;
    }
    const auto* resource = std::get_if<t::ResourceRef>(&decoded);
    if (!resource || resource->kind != t::ResourceKind::Catalog || resource->id.value.empty()) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    return std::nullopt;
  }
  static bool operation_ref(const t::OperationRef& value, v1::OperationRef* output) {
    if (!output) {
      return true;
    }
    output->set_owner_service(service_text(value.owner));
    output->set_operation_id(value.id.value);
    return resource(value.resource, output->mutable_resource());
  }
  t::RuntimeResourceApplication& application_;
  RuntimeRpcOptions options_;
};
} // namespace

target::Outcome<std::unique_ptr<v1::RuntimeService::Service>>
make_runtime_rpc(target::RuntimeResourceApplication& application, RuntimeRpcOptions options) {
  if (!options.authorize) {
    return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
  }
  try {
    std::unique_ptr<v1::RuntimeService::Service> service =
        std::make_unique<RuntimeRpc>(application, std::move(options));
    return service;
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::platform
