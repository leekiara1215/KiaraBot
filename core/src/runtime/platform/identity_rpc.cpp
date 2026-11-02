#include "kiarabot/core/runtime/platform/identity_rpc.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/domain/target/identity_state_rules.hpp"
#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace codec = target_codec;
namespace t = target;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose,
                   t::Acceptance acceptance = t::Acceptance::NotAccepted) {
  return {code, acceptance, recovery, std::nullopt, std::nullopt};
}
t::Failure invalid_result() {
  return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
}
bool uuid(std::string_view value) {
  v1::ResourceRef reference;
  reference.set_kind(v1::RESOURCE_PRINCIPAL);
  reference.set_id(std::string(value));
  return std::holds_alternative<t::ResourceRef>(codec::from_wire(reference));
}
std::string random_id() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return {};
  }
  bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
  bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
  std::string result;
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result += '-';
    }
    result += kHex[bytes[index] >> 4U];
    result += kHex[bytes[index] & 15U];
  }
  return result;
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
t::SecretView secret(const std::string& value) {
  return std::as_bytes(std::span{value.data(), value.size()});
}
bool key_valid(std::string_view value) {
  if (value.size() > 128) {
    return false;
  }
  for (char character : value) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '.' || character == '_' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}
bool positive(t::TimePoint value) {
  return value.time_since_epoch().count() > 0;
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
  const auto error = codec::make_error(input.code, request, trace,
                                       input.recovery == t::Recovery::Backoff ||
                                           input.recovery == t::Recovery::QuerySameIdentity);
  const auto* stable = std::get_if<v1::Error>(&error);
  bool valid = stable != nullptr;
  if (stable) {
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
    valid = false;
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
  case t::Recovery::ReadCurrent:
    context->set_recovery(v1::RESYNC_SNAPSHOT);
    break;
  case t::Recovery::QuerySameIdentity:
    context->set_recovery(v1::QUERY_SAME_INTENT);
    break;
  case t::Recovery::Backoff:
    context->set_recovery(v1::RETRY_SAME_ID);
    break;
  case t::Recovery::Diagnose:
    context->set_recovery(v1::MANUAL_REVIEW);
    break;
  default:
    valid = false;
    break;
  }
  // An unknown commit may only query/retry the SAME identity or require manual diagnosis.
  if (input.acceptance == t::Acceptance::Unknown &&
      input.recovery != t::Recovery::QuerySameIdentity && input.recovery != t::Recovery::Backoff &&
      input.recovery != t::Recovery::Diagnose) {
    valid = false;
  }
  if (input.retry_after) {
    const auto count = input.retry_after->count();
    if (count < 0 || count > std::numeric_limits<std::uint32_t>::max()) {
      valid = false;
    } else {
      context->set_retry_after_ms(static_cast<std::uint32_t>(count));
    }
  }
  if (input.operation) {
    auto* operation = context->mutable_operation();
    switch (input.operation->owner) {
    case t::Service::Runtime:
      operation->set_owner_service("runtime");
      break;
    case t::Service::Scheduler:
      operation->set_owner_service("scheduler");
      break;
    case t::Service::Model:
      operation->set_owner_service("model");
      break;
    default:
      valid = false;
      break;
    }
    operation->set_operation_id(input.operation->id.value);
    const auto resource = codec::to_wire(input.operation->resource);
    if (const auto* encoded = std::get_if<v1::ResourceRef>(&resource)) {
      *operation->mutable_resource() = *encoded;
    } else {
      valid = false;
    }
  }
  if (valid) {
    const auto encoded = codec::to_grpc_failure(result);
    if (const auto* status = std::get_if<grpc::Status>(&encoded)) {
      return *status;
    }
  }
  // Malformed application output cannot make an unknown commit look not accepted.
  *result.mutable_error() =
      std::get<v1::Error>(codec::make_error(t::ErrorCode::Internal, request, trace, false));
  context->clear_operation();
  context->set_retry_after_ms(0);
  context->set_recovery(context->acceptance() == v1::ACCEPTANCE_UNKNOWN ? v1::QUERY_SAME_INTENT
                                                                        : v1::MANUAL_REVIEW);
  return std::get<grpc::Status>(codec::to_grpc_failure(result));
}
bool session(const t::SessionMetadata& input, t::TimePoint checked, v1::SessionRecord* output) {
  if (!uuid(input.id.value) || !uuid(input.subject.value) || input.version.value == 0 ||
      input.rotation == 0 || !positive(input.created_at) || input.created_at > checked ||
      input.expires_at <= input.created_at || !positive(checked)) {
    return false;
  }
  output->set_session_id(input.id.value);
  output->set_principal_id(input.subject.value);
  output->set_rotation(input.rotation);
  output->set_revoked(input.revoked);
  return timestamp(input.created_at, output->mutable_created_at()) &&
         timestamp(input.expires_at, output->mutable_expires_at());
}
t::Outcome<v1::TokenPair> token_pair(const t::IdentityTokenReply& input) {
  const auto& pair = input.tokens;
  if (!uuid(pair.session.value) || !pair.access || !pair.refresh || pair.access->bytes().empty() ||
      pair.refresh->bytes().empty() || pair.rotation == 0 || !positive(input.checked_at) ||
      !positive(pair.access_expires_at) || pair.refresh_expires_at <= input.checked_at ||
      pair.refresh_expires_at < pair.access_expires_at) {
    return invalid_result();
  }
  v1::TokenPair output;
  output.set_session_id(pair.session.value);
  output.set_rotation(pair.rotation);
  const auto assign_secret = [](const t::SecretBuffer& buffer, std::string* destination) {
    const auto bytes = buffer.bytes();
    for (const auto byte : bytes) {
      if (std::to_integer<unsigned int>(byte) < 33 || std::to_integer<unsigned int>(byte) > 126) {
        return false;
      }
    }
    destination->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
  };
  if (!assign_secret(*pair.access, output.mutable_access_token()) ||
      !assign_secret(*pair.refresh, output.mutable_refresh_token()) ||
      !timestamp(pair.access_expires_at, output.mutable_access_expires_at()) ||
      !timestamp(pair.refresh_expires_at, output.mutable_refresh_expires_at())) {
    return invalid_result();
  }
  return output;
}
t::Outcome<v1::PrincipalRecord>
principal(const t::Principal& input, const std::vector<t::ExactAssignment>& assignments,
          google::protobuf::RepeatedPtrField<v1::AssignmentRecord>* output) {
  if (!uuid(input.id.value) || !input.binding || input.version.value == 0 ||
      input.kind != t::PrincipalKind::Human || input.state != t::LifecycleState::Active) {
    return invalid_result();
  }
  v1::PrincipalRecord result;
  result.set_principal_id(input.id.value);
  result.set_principal_kind("human");
  result.set_version(input.version.value);
  result.set_state(v1::LIFECYCLE_ACTIVE);
  // The public PrincipalRecord requires the persisted binding. Never invent it from the subject.
  if (input.binding) {
    const auto encoded = codec::to_wire(*input.binding);
    if (const auto* binding = std::get_if<v1::ResourceRef>(&encoded)) {
      *result.mutable_binding() = *binding;
    } else {
      return invalid_result();
    }
  }
  std::set<v1::FixedRole> roles;
  std::set<t::Permission> permissions;
  std::set<std::string> ids;
  for (const auto& assignment : assignments) {
    if (assignment.subject != input.id || !uuid(assignment.id.value) ||
        !ids.insert(assignment.id.value).second) {
      return invalid_result();
    }
    const auto projection = t::project_assignment(assignment);
    const auto* projected = std::get_if<t::AssignmentProjection>(&projection);
    if (!projected) {
      return invalid_result();
    }
    auto* row = output->Add();
    row->set_assignment_id(projected->id.value);
    row->set_principal_id(projected->subject.value);
    row->set_version(projected->version.value);
    if (projected->role) {
      v1::FixedRole role;
      switch (*projected->role) {
      case t::FixedRole::Admin:
        role = v1::ROLE_ADMIN;
        break;
      case t::FixedRole::Operator:
        role = v1::ROLE_OPERATOR;
        break;
      case t::FixedRole::Viewer:
        role = v1::ROLE_VIEWER;
        break;
      default:
        return invalid_result();
      }
      row->set_role(role);
      roles.insert(role);
      for (const auto& [name, permission] : t::kPermissionNames) {
        static_cast<void>(name);
        if (t::default_role_allows(*projected->role, permission)) {
          permissions.insert(permission);
        }
      }
    }
    for (const auto permission : projected->permissions) {
      const auto encoded = codec::to_wire(permission);
      if (const auto* wire = std::get_if<v1::Permission>(&encoded)) {
        row->add_permissions(*wire);
        permissions.insert(permission);
      } else {
        return invalid_result();
      }
    }
    for (const auto& resource : projected->scope) {
      const auto encoded = codec::to_wire(resource);
      if (const auto* wire = std::get_if<v1::ResourceRef>(&encoded)) {
        *row->mutable_scope()->add_resources() = *wire;
      } else {
        return invalid_result();
      }
    }
  }
  for (const auto role : roles) {
    result.add_roles(role);
  }
  for (const auto permission : permissions) {
    const auto encoded = codec::to_wire(permission);
    if (const auto* wire = std::get_if<v1::Permission>(&encoded)) {
      result.add_display_permissions(*wire);
    } else {
      return invalid_result();
    }
  }
  return result;
}
bool assignment_record(const t::AssignmentProjection& input, v1::AssignmentRecord* output) {
  if (!uuid(input.id.value) || !uuid(input.subject.value) || input.version.value == 0 ||
      input.scope.empty()) {
    return false;
  }
  output->set_assignment_id(input.id.value);
  output->set_principal_id(input.subject.value);
  output->set_version(input.version.value);
  if (input.role) {
    switch (*input.role) {
    case t::FixedRole::Admin:
      output->set_role(v1::ROLE_ADMIN);
      break;
    case t::FixedRole::Operator:
      output->set_role(v1::ROLE_OPERATOR);
      break;
    case t::FixedRole::Viewer:
      output->set_role(v1::ROLE_VIEWER);
      break;
    default:
      return false;
    }
    if (!input.permissions.empty()) {
      return false;
    }
  }
  for (const auto permission : input.permissions) {
    const auto encoded = codec::to_wire(permission);
    if (const auto* wire = std::get_if<v1::Permission>(&encoded)) {
      output->add_permissions(*wire);
    } else {
      return false;
    }
  }
  for (const auto& resource : input.scope) {
    const auto encoded = codec::to_wire(resource);
    if (const auto* wire = std::get_if<v1::ResourceRef>(&encoded)) {
      *output->mutable_scope()->add_resources() = *wire;
    } else {
      return false;
    }
  }
  return true;
}
t::Outcome<std::vector<t::ResourceRef>> resource_scope(const v1::ResourceScope& input) {
  std::vector<t::ResourceRef> result;
  if (input.resources_size() > 128) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  for (const auto& resource : input.resources()) {
    auto decoded = codec::from_wire(resource);
    if (const auto* error = std::get_if<t::Failure>(&decoded)) {
      return *error;
    }
    result.push_back(std::get<t::ResourceRef>(decoded));
  }
  return result;
}
t::Outcome<t::FixedRole> decode_role(v1::FixedRole input) {
  switch (input) {
  case v1::ROLE_ADMIN:
    return t::FixedRole::Admin;
  case v1::ROLE_OPERATOR:
    return t::FixedRole::Operator;
  case v1::ROLE_VIEWER:
    return t::FixedRole::Viewer;
  default:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
}
t::Outcome<t::ExactAssignmentSpec> decode_assignment_spec(const v1::AssignmentSpec& input) {
  if (!uuid(input.principal_id())) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  t::ExactAssignmentSpec spec{t::SubjectId{input.principal_id()},
                              t::ExplicitAssignmentSpec{{}, {}}};
  auto scope = resource_scope(input.scope());
  if (const auto* error = std::get_if<t::Failure>(&scope)) {
    return *error;
  }
  auto resources = std::move(std::get<std::vector<t::ResourceRef>>(scope));
  if (input.role() != v1::FIXED_ROLE_UNSPECIFIED) {
    if (input.permissions_size() != 0) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    auto role = decode_role(input.role());
    if (const auto* error = std::get_if<t::Failure>(&role)) {
      return *error;
    }
    spec.basis = t::RoleAssignmentSpec{std::get<t::FixedRole>(role), std::move(resources)};
    return spec;
  }
  if (input.permissions_size() == 0) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  t::ExplicitAssignmentSpec explicit_spec{{}, std::move(resources)};
  for (const auto wire : input.permissions()) {
    auto decoded = codec::from_wire(static_cast<v1::Permission>(wire));
    if (const auto* error = std::get_if<t::Failure>(&decoded)) {
      return *error;
    }
    explicit_spec.permissions.push_back(std::get<t::Permission>(decoded));
  }
  spec.basis = std::move(explicit_spec);
  return spec;
}
bool cursor_scope_equal(std::span<const t::ResourceRef> left,
                        std::span<const t::ResourceRef> right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end());
}
t::Outcome<t::InvalidationCursor> decode_cursor(const v1::StreamCursor& input,
                                                std::vector<t::ResourceRef> subscribed_scope) {
  t::InvalidationCursor cursor;
  cursor.source_service = input.source_service();
  cursor.epoch = input.epoch();
  cursor.sequence = input.sequence();
  auto scope = resource_scope(input.scope());
  if (const auto* error = std::get_if<t::Failure>(&scope)) {
    return *error;
  }
  cursor.scope = std::move(std::get<std::vector<t::ResourceRef>>(scope));
  if (cursor.scope.empty()) {
    cursor.scope = std::move(subscribed_scope);
  } else if (!cursor_scope_equal(cursor.scope, subscribed_scope)) {
    // A cursor bound to another subscription scope cannot be silently widened or mixed.
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  return cursor;
}
bool encode_cursor(const t::InvalidationCursor& cursor, v1::StreamCursor* output) {
  if (cursor.scope.empty() || cursor.scope.size() > t::kMaximumInvalidationScope ||
      !t::valid_uuid_text(cursor.epoch)) {
    return false;
  }
  output->set_source_service(std::string{t::kInvalidationSourceService});
  output->set_epoch(cursor.epoch);
  output->set_sequence(cursor.sequence);
  for (const auto& resource : cursor.scope) {
    const auto encoded = codec::to_wire(resource);
    if (const auto* wire = std::get_if<v1::ResourceRef>(&encoded)) {
      *output->mutable_scope()->add_resources() = *wire;
    } else {
      return false;
    }
  }
  return true;
}
bool record_visible(const t::InvalidationRecord& record,
                    std::span<const t::ResourceRef> subscribed) {
  for (const auto& resource : record.event.scope) {
    if (std::find(subscribed.begin(), subscribed.end(), resource) != subscribed.end()) {
      return true;
    }
  }
  return false;
}
bool encode_invalidation(const t::InvalidationRecord& record,
                         std::span<const t::ResourceRef> subscribed,
                         v1::InvalidationRecord* output) {
  const auto wire_cursor = t::InvalidationCursor{std::string{t::kInvalidationSourceService},
                                                 record.epoch, record.event.scope, record.sequence};
  if (!encode_cursor(wire_cursor, output->mutable_cursor())) {
    return false;
  }
  output->set_principal_id(record.event.principal.value);
  output->set_authorization_version(record.event.authorization_version.value);
  for (const auto& resource : record.event.scope) {
    const auto encoded = codec::to_wire(resource);
    if (const auto* wire = std::get_if<v1::ResourceRef>(&encoded)) {
      *output->mutable_scope()->add_resources() = *wire;
    } else {
      return false;
    }
  }
  return record_visible(record, subscribed);
}
bool resync_cursor(const t::InvalidationWatermark& watermark, std::span<const t::ResourceRef> scope,
                   v1::StreamCursor* output) {
  return encode_cursor(
      t::InvalidationCursor{std::string{t::kInvalidationSourceService}, watermark.epoch,
                            std::vector<t::ResourceRef>(scope.begin(), scope.end()),
                            watermark.sequence},
      output);
}
// Principal/credential management projections. They never invent roles, scope, expiry or a
// secret; every field comes from a committed application result.
t::Outcome<t::PrincipalKind> principal_kind(std::string_view value) {
  if (value == "human") {
    return t::PrincipalKind::Human;
  }
  if (value == "platform_service") {
    return t::PrincipalKind::PlatformService;
  }
  if (value == "adapter") {
    return t::PrincipalKind::Adapter;
  }
  if (value == "plugin") {
    return t::PrincipalKind::Plugin;
  }
  return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
const char* principal_kind_name(t::PrincipalKind kind) {
  switch (kind) {
  case t::PrincipalKind::Human:
    return "human";
  case t::PrincipalKind::PlatformService:
    return "platform_service";
  case t::PrincipalKind::Adapter:
    return "adapter";
  case t::PrincipalKind::Plugin:
    return "plugin";
  }
  return nullptr;
}
std::optional<v1::LifecycleState> lifecycle(t::LifecycleState state) {
  switch (state) {
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
  return std::nullopt;
}
std::optional<v1::CredentialState> credential_state(t::CredentialStatusState state) {
  switch (state) {
  case t::CredentialStatusState::Pending:
    return v1::CREDENTIAL_PENDING;
  case t::CredentialStatusState::Active:
    return v1::CREDENTIAL_ACTIVE;
  case t::CredentialStatusState::Expired:
    return v1::CREDENTIAL_EXPIRED;
  case t::CredentialStatusState::Revoked:
    return v1::CREDENTIAL_REVOKED;
  }
  return std::nullopt;
}
t::Outcome<v1::PrincipalRecord> principal_status(const t::Principal& input) {
  if (!uuid(input.id.value) || input.version.value == 0 || !input.binding ||
      !t::valid_resource_ref(*input.binding) || !t::known_principal_kind(input.kind)) {
    return invalid_result();
  }
  const auto* kind = principal_kind_name(input.kind);
  const auto encoded = codec::to_wire(*input.binding);
  const auto* binding = std::get_if<v1::ResourceRef>(&encoded);
  if (!kind || !binding) {
    return invalid_result();
  }
  v1::PrincipalRecord result;
  result.set_principal_id(input.id.value);
  result.set_principal_kind(kind);
  result.set_version(input.version.value);
  *result.mutable_binding() = *binding;
  const auto wire_state = lifecycle(input.state);
  if (!wire_state) {
    return invalid_result();
  }
  result.set_state(*wire_state);
  return result;
}
t::Outcome<v1::CredentialStatus> credential_status(const t::CredentialStatusSnapshot& input) {
  if (!uuid(input.ref.id.value) || input.ref.version.value == 0 || !uuid(input.subject.value) ||
      !t::valid_resource_ref(input.binding) || !positive(input.created_at) ||
      input.checked_at < input.created_at) {
    return invalid_result();
  }
  v1::CredentialStatus result;
  const auto encoded = codec::to_wire(input.binding);
  const auto* binding = std::get_if<v1::ResourceRef>(&encoded);
  const auto wire_state = credential_state(input.state);
  if (!binding || !wire_state) {
    return invalid_result();
  }
  result.set_state(*wire_state);
  result.set_credential_id(input.ref.id.value);
  result.set_principal_id(input.subject.value);
  *result.mutable_binding() = *binding;
  result.set_version(input.ref.version.value);
  if (input.expires_at) {
    if (input.state == t::CredentialStatusState::Pending || *input.expires_at <= input.created_at ||
        !timestamp(*input.expires_at, result.mutable_expires_at())) {
      return invalid_result();
    }
  } else if (input.state == t::CredentialStatusState::Active ||
             input.state == t::CredentialStatusState::Expired) {
    return invalid_result();
  }
  if (!timestamp(input.created_at, result.mutable_created_at()) ||
      !timestamp(input.checked_at, result.mutable_checked_at())) {
    return invalid_result();
  }
  return result;
}
t::Outcome<v1::CredentialDelivery> credential_delivery(const t::CredentialDeliveryReply& input) {
  if (!uuid(input.credential.id.value) || input.credential.version.value == 0 || !input.secret ||
      input.secret->bytes().empty() || input.delivery_receipt_id.empty() ||
      input.delivery_receipt_id.size() > 256 || !positive(input.expires_at) ||
      !positive(input.checked_at)) {
    return invalid_result();
  }
  v1::CredentialDelivery result;
  result.set_credential_id(input.credential.id.value);
  result.set_version(input.credential.version.value);
  result.set_new_secret(reinterpret_cast<const char*>(input.secret->bytes().data()),
                        input.secret->bytes().size());
  result.set_delivery_receipt_id(input.delivery_receipt_id);
  if (!timestamp(input.expires_at, result.mutable_expires_at())) {
    return invalid_result();
  }
  return result;
}
// Match the mature proof encoder's budget before decoding or expanding a wire product.
template <typename Request> bool within_delegation_budget(const Request& request) {
  if (request.scope().resources_size() < 1 || request.scope().resources_size() > 128 ||
      request.permissions_size() < 1 || request.permissions_size() > 40) {
    return false;
  }
  const auto resource_count = static_cast<std::size_t>(request.scope().resources_size());
  const auto permission_count = static_cast<std::size_t>(request.permissions_size());
  return permission_count <= 128 / resource_count;
}
t::Outcome<t::Service> audience(std::string_view value) {
  if (value == "identity") {
    return t::Service::Identity;
  }
  if (value == "runtime") {
    return t::Service::Runtime;
  }
  if (value == "host") {
    return t::Service::Host;
  }
  if (value == "scheduler") {
    return t::Service::Scheduler;
  }
  if (value == "model") {
    return t::Service::Model;
  }
  if (value == "control") {
    return t::Service::Control;
  }
  return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
struct AuditCommandInput {
  t::ResourceRef scope;
  t::AuditQuery query;
};
t::Outcome<AuditCommandInput> audit_command(const v1::IdentityAuditFilter& filter) {
  if (!filter.has_scope() || filter.scope().resources_size() != 1) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  const auto decoded_scope = codec::from_wire(filter.scope().resources(0));
  const auto* scope = std::get_if<t::ResourceRef>(&decoded_scope);
  if (!scope) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  t::AuditQuery query;
  query.limit = filter.has_page() && filter.page().page_size() != 0
                    ? std::min<std::size_t>(filter.page().page_size(), 100)
                    : 20;
  if (filter.has_page() && !filter.page().page_token().empty()) {
    auto cursor = t::decode_audit_cursor(filter.page().page_token());
    if (!cursor) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    query.cursor = *cursor;
  }
  if (filter.has_occurred_from()) {
    const auto decoded = codec::from_wire(filter.occurred_from());
    if (const auto* value = std::get_if<t::TimePoint>(&decoded)) {
      query.filter.occurred_from = *value;
    } else {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
  }
  if (filter.has_occurred_until()) {
    const auto decoded = codec::from_wire(filter.occurred_until());
    if (const auto* value = std::get_if<t::TimePoint>(&decoded)) {
      query.filter.occurred_until = *value;
    } else {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
  }
  if (!filter.actor().empty()) {
    if (!uuid(filter.actor())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    query.filter.actor = t::SubjectId{filter.actor()};
  }
  if (filter.has_resource()) {
    const auto decoded = codec::from_wire(filter.resource());
    const auto* resource = std::get_if<t::ResourceRef>(&decoded);
    if (!resource) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    query.filter.resource = *resource;
  }
  return AuditCommandInput{*scope, std::move(query)};
}

t::IdentityAuditCommand audit_command_for(const t::IdentityRequestContext& context,
                                          const t::VerifiedPeer& peer,
                                          const AuditCommandInput& input) {
  return {context, peer, input.scope, input.query};
}
class IdentityRpc final : public v1::IdentityService::Service {
public:
  IdentityRpc(t::IdentitySecurityApplication& application, PeerAuthenticator authenticate,
              t::IdentityDelegationApplication* issuer, InvocationDeadline invocation_deadline,
              IdentityRpcOptions options)
      : application_(application), authenticate_(std::move(authenticate)), issuer_(issuer),
        invocation_deadline_(std::move(invocation_deadline)),
        verification_keys_(options.verification_keys), exchange_(options.exchange),
        principal_(options.principal), authorization_(options.authorization),
        assignments_(options.assignments), invalidation_(options.invalidation),
        audit_(options.audit) {}

  grpc::Status Login(grpc::ServerContext* transport, const v1::IdentityLoginRequest* request,
                     v1::IdentityLoginResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer,
            const t::IdentityRequestContext& context) -> t::Outcome<v1::IdentityLoginResponse> {
          if (request->username().empty() || request->password().empty() ||
              context.idempotency_key.empty()) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto reply = application_.login(
              t::LoginRequest{context, peer, request->username(), secret(request->password())});
          if (auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::IdentityTokenReply>(reply);
          auto tokens = token_pair(result);
          if (auto* error = std::get_if<t::Failure>(&tokens)) {
            return *error;
          }
          v1::IdentityLoginResponse output;
          *output.mutable_tokens() = std::get<v1::TokenPair>(std::move(tokens));
          if (!timestamp(result.checked_at, output.mutable_checked_at())) {
            return invalid_result();
          }
          return output;
        });
  }
  grpc::Status RefreshSession(grpc::ServerContext* transport,
                              const v1::IdentityRefreshSessionRequest* request,
                              v1::IdentityRefreshSessionResponse* response) override {
    return run(transport, request->has_context(), request->context(), response,
               [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
                   -> t::Outcome<v1::IdentityRefreshSessionResponse> {
                 if (request->refresh_token().empty() || context.idempotency_key.empty()) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 auto reply = application_.refresh_session(
                     t::RefreshSessionRequest{context, peer, secret(request->refresh_token())});
                 if (auto* error = std::get_if<t::Failure>(&reply)) {
                   return *error;
                 }
                 const auto& result = std::get<t::IdentityTokenReply>(reply);
                 auto tokens = token_pair(result);
                 if (auto* error = std::get_if<t::Failure>(&tokens)) {
                   return *error;
                 }
                 v1::IdentityRefreshSessionResponse output;
                 *output.mutable_tokens() = std::get<v1::TokenPair>(std::move(tokens));
                 if (!timestamp(result.checked_at, output.mutable_checked_at())) {
                   return invalid_result();
                 }
                 return output;
               });
  }
  grpc::Status ValidateSession(grpc::ServerContext* transport,
                               const v1::IdentityValidateSessionRequest* request,
                               v1::IdentityValidateSessionResponse* response) override {
    return run(transport, request->has_context(), request->context(), response,
               [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
                   -> t::Outcome<v1::IdentityValidateSessionResponse> {
                 if (request->access_token().empty()) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 auto reply = application_.validate_session(
                     t::AccessSessionRequest{context, peer, secret(request->access_token())});
                 if (auto* error = std::get_if<t::Failure>(&reply)) {
                   return *error;
                 }
                 const auto& result = std::get<t::ValidatedSession>(reply);
                 if (result.authorization_version.value == 0 || !positive(result.checked_at)) {
                   return invalid_result();
                 }
                 v1::IdentityValidateSessionResponse output;
                 auto projected =
                     principal(result.principal, result.assignments, output.mutable_assignments());
                 if (auto* error = std::get_if<t::Failure>(&projected)) {
                   return *error;
                 }
                 *output.mutable_principal() = std::get<v1::PrincipalRecord>(std::move(projected));
                 output.set_authorization_version(result.authorization_version.value);
                 if (!timestamp(result.checked_at, output.mutable_checked_at())) {
                   return invalid_result();
                 }
                 if (result.session.subject != result.principal.id || result.session.revoked ||
                     result.session.expires_at <= result.checked_at ||
                     !session(result.session, result.checked_at, output.mutable_session())) {
                   return invalid_result();
                 }
                 return output;
               });
  }
  grpc::Status GetPrincipal(grpc::ServerContext* transport,
                            const v1::IdentityGetPrincipalRequest* request,
                            v1::IdentityGetPrincipalResponse* response) override {
    return run(transport, request->has_context(), request->context(), response,
               [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
                   -> t::Outcome<v1::IdentityGetPrincipalResponse> {
                 if (request->access_token().empty()) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 auto reply = application_.get_principal(
                     t::AccessSessionRequest{context, peer, secret(request->access_token())});
                 if (auto* error = std::get_if<t::Failure>(&reply)) {
                   return *error;
                 }
                 const auto& result = std::get<t::CurrentPrincipal>(reply);
                 if (result.authorization_version.value == 0 || !positive(result.checked_at)) {
                   return invalid_result();
                 }
                 v1::IdentityGetPrincipalResponse output;
                 auto projected =
                     principal(result.principal, result.assignments, output.mutable_assignments());
                 if (auto* error = std::get_if<t::Failure>(&projected)) {
                   return *error;
                 }
                 *output.mutable_principal() = std::get<v1::PrincipalRecord>(std::move(projected));
                 output.set_authorization_version(result.authorization_version.value);
                 if (!timestamp(result.checked_at, output.mutable_checked_at())) {
                   return invalid_result();
                 }
                 return output;
               });
  }
  grpc::Status RevokeSession(grpc::ServerContext* transport,
                             const v1::IdentityRevokeSessionRequest* request,
                             v1::IdentityRevokeSessionResponse* response) override {
    return run(transport, request->has_context(), request->context(), response,
               [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
                   -> t::Outcome<v1::IdentityRevokeSessionResponse> {
                 if (!uuid(request->session_id()) || request->context().delegation().empty() ||
                     context.idempotency_key.empty()) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 auto reply = application_.revoke_session(t::RevokeSessionRequest{
                     context,
                     t::IdentityActorProof{peer, secret(request->context().delegation())},
                     {request->session_id()}});
                 if (auto* error = std::get_if<t::Failure>(&reply)) {
                   return *error;
                 }
                 const auto& result = std::get<t::RevokeSessionReply>(reply);
                 v1::IdentityRevokeSessionResponse output;
                 if (result.session.id.value != request->session_id() || !result.session.revoked ||
                     !session(result.session, result.checked_at, output.mutable_session()) ||
                     !timestamp(result.checked_at, output.mutable_checked_at())) {
                   return invalid_result();
                 }
                 return output;
               });
  }
  grpc::Status ListSessions(grpc::ServerContext* transport,
                            const v1::IdentityListSessionsRequest* request,
                            v1::IdentityListSessionsResponse* response) override {
    return run(transport, request->has_context(), request->context(), response,
               [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
                   -> t::Outcome<v1::IdentityListSessionsResponse> {
                 const auto& page = request->page();
                 if (!uuid(request->principal_id()) || request->context().delegation().empty() ||
                     page.page_size() > 100 || !page.next_page_token().empty() ||
                     page.has_expires_at() || page.page_token().size() > 1024 ||
                     (!page.snapshot_id().empty() && !uuid(page.snapshot_id())) ||
                     (page.page_token().empty() && !page.snapshot_id().empty())) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 auto reply = application_.list_sessions(t::ListSessionsRequest{
                     context,
                     t::IdentityActorProof{peer, secret(request->context().delegation())},
                     {request->principal_id()},
                     {page.page_size(), page.page_token(), page.snapshot_id()}});
                 if (auto* error = std::get_if<t::Failure>(&reply)) {
                   return *error;
                 }
                 const auto& result = std::get<t::SessionPageReply>(reply);
                 const auto size = page.page_size() == 0 ? 20 : page.page_size();
                 if (!positive(result.checked_at) || result.authorization_version.value == 0 ||
                     result.page_size != size || result.sessions.size() > size ||
                     !uuid(result.snapshot_id) || result.next_page_token.size() > 1024 ||
                     (!page.snapshot_id().empty() && result.snapshot_id != page.snapshot_id()) ||
                     result.expires_at <= result.checked_at ||
                     result.expires_at - result.checked_at > t::kSessionSnapshotLifetime) {
                   return invalid_result();
                 }
                 v1::IdentityListSessionsResponse output;
                 std::set<std::string> ids;
                 for (const auto& entry : result.sessions) {
                   if (entry.subject.value != request->principal_id() ||
                       !ids.insert(entry.id.value).second ||
                       !session(entry, result.checked_at, output.add_sessions())) {
                     return invalid_result();
                   }
                 }
                 output.mutable_page()->set_page_size(result.page_size);
                 output.mutable_page()->set_snapshot_id(result.snapshot_id);
                 output.mutable_page()->set_next_page_token(result.next_page_token);
                 if (!timestamp(result.expires_at, output.mutable_page()->mutable_expires_at()) ||
                     !timestamp(result.checked_at, output.mutable_checked_at())) {
                   return invalid_result();
                 }
                 return output;
               });
  }
  grpc::Status RegisterPrincipal(grpc::ServerContext* transport,
                                 const v1::IdentityRegisterPrincipalRequest* request,
                                 v1::IdentityRegisterPrincipalResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityRegisterPrincipalResponse> {
          if (!principal_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (request->principal_kind().empty() || !request->has_binding() ||
              request->allowed_services_size() == 0 || request->allowed_services_size() > 16 ||
              context.idempotency_key.empty()) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto kind = principal_kind(request->principal_kind());
          if (const auto* error = std::get_if<t::Failure>(&kind)) {
            return *error;
          }
          const auto encoded = codec::from_wire(request->binding());
          const auto* binding = std::get_if<t::ResourceRef>(&encoded);
          if (!binding) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          std::vector<t::Service> services;
          for (const auto& name : request->allowed_services()) {
            auto service = audience(name);
            if (const auto* error = std::get_if<t::Failure>(&service)) {
              return *error;
            }
            if (std::get<t::Service>(service) == t::Service::Control ||
                std::find(services.begin(), services.end(), std::get<t::Service>(service)) !=
                    services.end()) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
            services.push_back(std::get<t::Service>(service));
          }
          auto reply = principal_->register_principal(t::RegisterPrincipalCommand{
              context, peer, std::get<t::PrincipalKind>(kind), *binding, std::move(services)});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& snapshot = std::get<t::RegisteredPrincipalSnapshot>(reply);
          auto projected = principal_status(snapshot.principal);
          if (const auto* error = std::get_if<t::Failure>(&projected)) {
            return *error;
          }
          auto initial = credential_status(snapshot.initial_credential);
          if (const auto* error = std::get_if<t::Failure>(&initial)) {
            return *error;
          }
          v1::IdentityRegisterPrincipalResponse output;
          *output.mutable_principal() = std::get<v1::PrincipalRecord>(std::move(projected));
          *output.mutable_initial_credential() = std::get<v1::CredentialStatus>(std::move(initial));
          return output;
        },
        false);
  }
  grpc::Status RotateCredential(grpc::ServerContext* transport,
                                const v1::IdentityRotateCredentialRequest* request,
                                v1::IdentityRotateCredentialResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityRotateCredentialResponse> {
          if (!principal_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!uuid(request->credential_id()) || !request->has_expected_version() ||
              request->expected_version() == 0 || context.idempotency_key.empty()) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto reply = principal_->rotate_credential(t::RotateCredentialCommand{
              context, peer, {request->credential_id()}, {request->expected_version()}});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          auto delivery = credential_delivery(std::get<t::CredentialDeliveryReply>(reply));
          if (const auto* error = std::get_if<t::Failure>(&delivery)) {
            return *error;
          }
          v1::IdentityRotateCredentialResponse output;
          *output.mutable_delivery() = std::get<v1::CredentialDelivery>(std::move(delivery));
          return output;
        },
        false);
  }
  grpc::Status RevokeCredential(grpc::ServerContext* transport,
                                const v1::IdentityRevokeCredentialRequest* request,
                                v1::IdentityRevokeCredentialResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityRevokeCredentialResponse> {
          if (!principal_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!uuid(request->credential_id()) || !request->has_expected_version() ||
              request->expected_version() == 0 || context.idempotency_key.empty()) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto reply = principal_->revoke_credential(t::RevokeCredentialCommand{
              context, peer, {request->credential_id()}, {request->expected_version()}});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::RevokeCredentialReply>(reply);
          if (result.version.value == 0 || !positive(result.checked_at)) {
            return invalid_result();
          }
          v1::IdentityRevokeCredentialResponse output;
          output.set_version(result.version.value);
          return output;
        },
        false);
  }
  grpc::Status GetPrincipalStatus(grpc::ServerContext* transport,
                                  const v1::IdentityGetPrincipalStatusRequest* request,
                                  v1::IdentityGetPrincipalStatusResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityGetPrincipalStatusResponse> {
          if (!principal_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!uuid(request->principal_id())) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto reply = principal_->get_principal_status(
              t::GetPrincipalStatusQuery{context, peer, {request->principal_id()}});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::PrincipalStatusReply>(reply);
          if (result.principal.id.value != request->principal_id() ||
              !positive(result.checked_at)) {
            return invalid_result();
          }
          auto projected = principal_status(result.principal);
          if (const auto* error = std::get_if<t::Failure>(&projected)) {
            return *error;
          }
          v1::IdentityGetPrincipalStatusResponse output;
          *output.mutable_principal() = std::get<v1::PrincipalRecord>(std::move(projected));
          return output;
        },
        false);
  }
  grpc::Status ListAudit(grpc::ServerContext* transport,
                         const v1::IdentityListAuditRequest* request,
                         v1::IdentityListAuditResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer,
            const t::IdentityRequestContext& context) -> t::Outcome<v1::IdentityListAuditResponse> {
          if (!audit_ || !request->has_filter()) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          auto converted = audit_command(request->filter());
          if (const auto* error = std::get_if<t::Failure>(&converted)) {
            return *error;
          }
          auto result =
              audit_->list_audit(audit_command_for(context, peer, std::get<AuditCommandInput>(converted)));
          if (const auto* error = std::get_if<t::Failure>(&result)) {
            return *error;
          }
          const auto& page = std::get<t::AuditPage>(result);
          v1::IdentityListAuditResponse output;
          for (const auto& record : page.records) {
            auto* entry = output.add_entries();
            entry->set_entry_id(std::to_string(record.id));
            entry->set_actor(record.actor.value);
            auto resource = codec::to_wire(record.resource);
            if (const auto* wire = std::get_if<v1::ResourceRef>(&resource)) {
              *entry->mutable_resource() = *wire;
            } else {
              return failure(t::ErrorCode::Internal);
            }
            entry->set_request_id(record.request_id);
            entry->set_trace_id(record.trace_id);
            entry->set_method(record.method);
            if (record.rejection) {
              auto code = codec::to_wire(*record.rejection);
              if (const auto* wire = std::get_if<v1::ErrorCode>(&code)) {
                entry->set_rejection(*wire);
              } else {
                return failure(t::ErrorCode::Internal);
              }
            } else {
              entry->set_rejection(v1::ERROR_CODE_UNSPECIFIED);
            }
            entry->set_authorization_version(record.authorization_version.value);
            auto occurred = codec::to_wire(record.occurred_at);
            if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&occurred)) {
              *entry->mutable_occurred_at() = *stamp;
            } else {
              return failure(t::ErrorCode::Internal);
            }
            if (record.target_account_id) {
              entry->set_target_account_id(*record.target_account_id);
            }
            if (record.target_conversation_id) {
              entry->set_target_conversation_id(*record.target_conversation_id);
            }
          }
          output.mutable_page()->set_next_page_token(page.next_cursor);
          output.mutable_page()->set_page_size(static_cast<std::uint32_t>(page.records.size()));
          auto checked = codec::to_wire(page.checked_at);
          if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&checked)) {
            *output.mutable_checked_at() = *stamp;
          } else {
            return failure(t::ErrorCode::Internal);
          }
          return output;
        },
        false);
  }
  grpc::Status ListLogs(grpc::ServerContext* transport,
                        const v1::IdentityListLogsRequest* request,
                        v1::IdentityListLogsResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer,
            const t::IdentityRequestContext& context) -> t::Outcome<v1::IdentityListLogsResponse> {
          if (!audit_ || !request->has_filter()) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          auto converted = audit_command(request->filter());
          if (const auto* error = std::get_if<t::Failure>(&converted)) {
            return *error;
          }
          auto result =
              audit_->list_logs(audit_command_for(context, peer, std::get<AuditCommandInput>(converted)));
          if (const auto* error = std::get_if<t::Failure>(&result)) {
            return *error;
          }
          const auto& page = std::get<t::LogPage>(result);
          v1::IdentityListLogsResponse output;
          for (const auto& record : page.records) {
            auto* entry = output.add_entries();
            entry->set_entry_id(std::to_string(record.id));
            entry->set_severity(record.severity);
            entry->set_component(record.component);
            entry->set_method(record.method);
            if (record.actor) {
              entry->set_actor(record.actor->value);
            }
            if (record.resource) {
              auto resource = codec::to_wire(*record.resource);
              if (const auto* wire = std::get_if<v1::ResourceRef>(&resource)) {
                *entry->mutable_resource() = *wire;
              } else {
                return failure(t::ErrorCode::Internal);
              }
            }
            entry->set_request_id(record.request_id);
            entry->set_trace_id(record.trace_id);
            entry->set_message(record.message);
            auto occurred = codec::to_wire(record.occurred_at);
            if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&occurred)) {
              *entry->mutable_occurred_at() = *stamp;
            } else {
              return failure(t::ErrorCode::Internal);
            }
          }
          output.mutable_page()->set_next_page_token(page.next_cursor);
          output.mutable_page()->set_page_size(static_cast<std::uint32_t>(page.records.size()));
          auto checked = codec::to_wire(page.checked_at);
          if (const auto* stamp = std::get_if<google::protobuf::Timestamp>(&checked)) {
            *output.mutable_checked_at() = *stamp;
          } else {
            return failure(t::ErrorCode::Internal);
          }
          return output;
        },
        false);
  }
  grpc::Status IssueDelegation(grpc::ServerContext* transport,
                               const v1::IdentityIssueDelegationRequest* request,
                               v1::IdentityIssueDelegationResponse* response) override {
    return run(transport, request->has_context(), request->context(), response,
               [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
                   -> t::Outcome<v1::IdentityIssueDelegationResponse> {
                 if (!issuer_) {
                   return failure(t::ErrorCode::CapabilityNotSupported);
                 }
                 if (!within_delegation_budget(*request)) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 const auto target = audience(request->audience());
                 const auto* service = std::get_if<t::Service>(&target);
                 const auto bytes = secret(request->client_certificate_sha256());
                 if (!service || request->subject_proof().empty() ||
                     request->scope().resources().empty() || request->permissions().empty() ||
                     bytes.size() != peer.binding.certificate.fingerprint.bytes.size() ||
                     !std::equal(bytes.begin(), bytes.end(),
                                 peer.binding.certificate.fingerprint.bytes.begin())) {
                   return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                 }
                 // No Bearer normalization, body actor or delegation-token substitution: the Core
                 // request is explicitly an AccessSessionRequest and validates the raw current
                 // session token.
                 for (const unsigned char character : request->subject_proof()) {
                   if (character < 33 || character > 126) {
                     return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                   }
                 }
                 std::set<t::ResourceRef, t::PolicyResourceLess> resources;
                 for (const auto& reference : request->scope().resources()) {
                   const auto decoded = codec::from_wire(reference);
                   const auto* resource = std::get_if<t::ResourceRef>(&decoded);
                   if (!resource || !resources.insert(*resource).second) {
                     return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                   }
                 }
                 std::set<t::Permission> permissions;
                 for (int index = 0; index < request->permissions_size(); ++index) {
                   const auto decoded = codec::from_wire(request->permissions(index));
                   const auto* permission = std::get_if<t::Permission>(&decoded);
                   if (!permission || !permissions.insert(*permission).second) {
                     return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
                   }
                 }
                 t::IssueIdentityDelegationRequest input{
                     {context, peer, secret(request->subject_proof())}, *service, {}};
                 input.permissions.reserve(resources.size() * permissions.size());
                 for (const auto& resource : resources) {
                   for (const auto permission : permissions) {
                     input.permissions.push_back({permission, resource});
                   }
                 }
                 auto result = issuer_->issue_delegation(input);
                 if (const auto* error = std::get_if<t::Failure>(&result)) {
                   return *error;
                 }
                 const auto& reply = std::get<t::IdentityDelegationReply>(result);
                 const auto* actor = std::get_if<t::PlatformPeer>(&peer.binding.peer);
                 const auto& claims = reply.claims;
                 if (!reply.proof || reply.proof->bytes().empty() || !actor ||
                     !positive(reply.checked_at) ||
                     std::holds_alternative<t::Failure>(t::validate_delegation_shape(claims)) ||
                     claims.audience != *service ||
                     claims.client_certificate != peer.binding.certificate.fingerprint ||
                     claims.direct_actor != actor->subject ||
                     claims.actor_chain != std::vector<t::SubjectId>{actor->subject} ||
                     !uuid(claims.subject.value) || !positive(claims.issued_at) ||
                     claims.business_hops != 1 || claims.expires_at <= reply.checked_at ||
                     !uuid(claims.proof_id.value) || !claims.source_session ||
                     !uuid(claims.source_session->value) ||
                     !uuid(claims.source_credential.id.value) ||
                     claims.permissions.size() != input.permissions.size()) {
                   return invalid_result();
                 }
                 for (const auto& binding : input.permissions) {
                   if (std::find(claims.permissions.begin(), claims.permissions.end(), binding) ==
                       claims.permissions.end()) {
                     return invalid_result();
                   }
                 }
                 v1::IdentityIssueDelegationResponse output;
                 auto* proof = output.mutable_delegation();
                 const auto proof_bytes = reply.proof->bytes();
                 for (const auto byte : proof_bytes) {
                   if (std::to_integer<unsigned int>(byte) < 33 ||
                       std::to_integer<unsigned int>(byte) > 126) {
                     return invalid_result();
                   }
                 }
                 proof->set_proof(std::string(reinterpret_cast<const char*>(proof_bytes.data()),
                                              proof_bytes.size()));
                 proof->set_audience(request->audience());
                 proof->set_business_hops(claims.business_hops);
                 if (!timestamp(claims.expires_at, proof->mutable_expires_at())) {
                   return invalid_result();
                 }
                 return output;
               });
  }

  grpc::Status ExchangeDelegation(grpc::ServerContext* transport,
                                  const v1::IdentityExchangeDelegationRequest* request,
                                  v1::IdentityExchangeDelegationResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityExchangeDelegationResponse> {
          if (!exchange_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!within_delegation_budget(*request)) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          const auto target = audience(request->audience());
          const auto* service = std::get_if<t::Service>(&target);
          const auto bytes = secret(request->client_certificate_sha256());
          const auto* runtime = std::get_if<t::PlatformPeer>(&peer.binding.peer);
          if (!runtime || runtime->service != t::Service::Runtime) {
            return failure(t::ErrorCode::PermissionDenied);
          }
          if (!service || (*service != t::Service::Model && *service != t::Service::Scheduler) ||
              request->parent_proof().empty() || request->parent_proof().size() > 32768 ||
              request->scope().resources().empty() || request->permissions().empty() ||
              bytes.size() != peer.binding.certificate.fingerprint.bytes.size() ||
              !std::equal(bytes.begin(), bytes.end(),
                          peer.binding.certificate.fingerprint.bytes.begin())) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          // Preserve the raw parent; only Core verifies its previous Control binding.
          for (const unsigned char character : request->parent_proof()) {
            if (character < 33 || character > 126) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
          }
          std::set<t::ResourceRef, t::PolicyResourceLess> resources;
          for (const auto& reference : request->scope().resources()) {
            const auto decoded = codec::from_wire(reference);
            const auto* resource = std::get_if<t::ResourceRef>(&decoded);
            if (!resource || !resources.insert(*resource).second) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
          }
          std::set<t::Permission> permissions;
          for (int index = 0; index < request->permissions_size(); ++index) {
            const auto decoded = codec::from_wire(request->permissions(index));
            const auto* permission = std::get_if<t::Permission>(&decoded);
            if (!permission || !permissions.insert(*permission).second) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
          }
          t::ExchangeIdentityDelegationRequest input{
              context, peer, secret(request->parent_proof()), *service, {}};
          input.permissions.reserve(resources.size() * permissions.size());
          for (const auto& resource : resources) {
            for (const auto permission : permissions) {
              input.permissions.push_back({permission, resource});
            }
          }
          auto result = exchange_->exchange_delegation(input);
          if (const auto* error = std::get_if<t::Failure>(&result)) {
            return *error;
          }
          const auto& reply = std::get<t::IdentityDelegationReply>(result);
          const auto* actor = std::get_if<t::PlatformPeer>(&peer.binding.peer);
          const auto& claims = reply.claims;
          if (!reply.proof || reply.proof->bytes().empty() || reply.proof->bytes().size() > 32768 ||
              !actor || !positive(reply.checked_at) ||
              std::holds_alternative<t::Failure>(t::validate_delegation_shape(claims)) ||
              claims.audience != *service ||
              claims.client_certificate != peer.binding.certificate.fingerprint ||
              claims.direct_actor != actor->subject ||
              (claims.actor_chain.size() != 2 || claims.actor_chain.back() != actor->subject) ||
              !uuid(claims.subject.value) || !positive(claims.issued_at) ||
              claims.business_hops != 2 || claims.expires_at <= reply.checked_at ||
              !uuid(claims.proof_id.value) ||
              (claims.source_session && !uuid(claims.source_session->value)) ||
              !uuid(claims.source_credential.id.value) ||
              claims.permissions.size() != input.permissions.size()) {
            return invalid_result();
          }
          for (const auto& binding : input.permissions) {
            if (std::find(claims.permissions.begin(), claims.permissions.end(), binding) ==
                claims.permissions.end()) {
              return invalid_result();
            }
          }
          v1::IdentityExchangeDelegationResponse output;
          auto* proof = output.mutable_delegation();
          const auto proof_bytes = reply.proof->bytes();
          for (const auto byte : proof_bytes) {
            if (std::to_integer<unsigned int>(byte) < 33 ||
                std::to_integer<unsigned int>(byte) > 126) {
              return invalid_result();
            }
          }
          proof->set_proof(
              std::string(reinterpret_cast<const char*>(proof_bytes.data()), proof_bytes.size()));
          proof->set_audience(request->audience());
          proof->set_business_hops(claims.business_hops);
          if (!timestamp(claims.expires_at, proof->mutable_expires_at())) {
            return invalid_result();
          }
          return output;
        },
        false);
  }

  grpc::Status CheckAuthorization(grpc::ServerContext* transport,
                                  const v1::IdentityCheckAuthorizationRequest* request,
                                  v1::IdentityCheckAuthorizationResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityCheckAuthorizationResponse> {
          if (!authorization_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (request->subject_proof().empty() || request->full_method().empty() ||
              request->scope().resources_size() == 0 || request->scope().resources_size() > 128) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto scope = resource_scope(request->scope());
          if (const auto* error = std::get_if<t::Failure>(&scope)) {
            return *error;
          }
          std::optional<t::ResourceVersion> known;
          if (request->has_known_authorization_version()) {
            if (request->known_authorization_version() == 0) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
            known = t::ResourceVersion{request->known_authorization_version()};
          }
          auto reply = authorization_->check_authorization(t::CheckAuthorizationRequest{
              context, peer, secret(request->subject_proof()), request->full_method(),
              std::move(std::get<std::vector<t::ResourceRef>>(scope)), known});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::CheckAuthorizationReply>(reply);
          if (!positive(result.checked_at) || result.authorization_version.value == 0 ||
              (result.allowed && result.denial) ||
              (!result.allowed &&
               (!result.denial || result.denial->code != t::ErrorCode::PermissionDenied))) {
            return invalid_result();
          }
          v1::IdentityCheckAuthorizationResponse output;
          auto* decision = output.mutable_decision();
          decision->set_allowed(result.allowed);
          decision->set_authorization_version(result.authorization_version.value);
          if (!timestamp(result.checked_at, decision->mutable_checked_at())) {
            return invalid_result();
          }
          if (result.denial) {
            auto encoded =
                codec::make_error(result.denial->code, context.request_id, context.trace_id, false);
            if (const auto* error = std::get_if<t::Failure>(&encoded)) {
              return *error;
            }
            *decision->mutable_denial() = std::get<v1::Error>(encoded);
          }
          return output;
        },
        false);
  }
  grpc::Status ListAssignments(grpc::ServerContext* transport,
                               const v1::IdentityListAssignmentsRequest* request,
                               v1::IdentityListAssignmentsResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityListAssignmentsResponse> {
          if (!assignments_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!uuid(request->principal_id()) || request->page().page_size() > 100 ||
              request->page().has_expires_at() || !request->page().next_page_token().empty()) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto scope = resource_scope(request->scope());
          if (const auto* error = std::get_if<t::Failure>(&scope)) {
            return *error;
          }
          t::AssignmentScopeFilter filter{std::move(std::get<std::vector<t::ResourceRef>>(scope))};
          t::AssignmentPageRequest page;
          page.page_size = request->page().page_size();
          page.page_token = request->page().page_token();
          page.snapshot_id = request->page().snapshot_id();
          auto reply = assignments_->list_assignments(
              t::ListAssignmentsRequest{context, peer, t::SubjectId{request->principal_id()},
                                        std::move(filter), std::move(page)});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::ListAssignmentsReply>(reply);
          if (result.page_size == 0 || result.page_size > 100 || result.snapshot_id.empty() ||
              !positive(result.checked_at) || !positive(result.expires_at) ||
              result.expires_at <= result.checked_at || result.authorization_version.value == 0) {
            return invalid_result();
          }
          v1::IdentityListAssignmentsResponse output;
          for (const auto& assignment : result.assignments) {
            if (!assignment_record(assignment, output.add_assignments())) {
              return invalid_result();
            }
          }
          auto* out_page = output.mutable_page();
          out_page->set_page_size(result.page_size);
          out_page->set_page_token(request->page().page_token());
          out_page->set_snapshot_id(result.snapshot_id);
          out_page->set_next_page_token(result.next_page_token);
          if (!timestamp(result.expires_at, out_page->mutable_expires_at())) {
            return invalid_result();
          }
          return output;
        },
        true);
  }
  grpc::Status SetAssignment(grpc::ServerContext* transport,
                             const v1::IdentitySetAssignmentRequest* request,
                             v1::IdentitySetAssignmentResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentitySetAssignmentResponse> {
          if (!assignments_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!request->has_assignment()) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto spec = decode_assignment_spec(request->assignment());
          if (const auto* error = std::get_if<t::Failure>(&spec)) {
            return *error;
          }
          std::optional<t::AssignmentId> assignment_id;
          if (!request->assignment_id().empty()) {
            if (!uuid(request->assignment_id())) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
            assignment_id = t::AssignmentId{request->assignment_id()};
          }
          std::optional<t::ResourceVersion> expected;
          if (request->has_expected_version()) {
            if (request->expected_version() == 0) {
              return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
            }
            expected = t::ResourceVersion{request->expected_version()};
          }
          auto reply = assignments_->set_assignment(t::SetAssignmentRequest{
              context, peer, std::move(std::get<t::ExactAssignmentSpec>(spec)), assignment_id,
              expected});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::SetAssignmentReply>(reply);
          if (!positive(result.checked_at) || result.assignment.version.value == 0 ||
              !uuid(result.assignment.id.value) || !uuid(result.assignment.subject.value)) {
            return invalid_result();
          }
          const auto projected = t::project_assignment(result.assignment);
          if (const auto* error = std::get_if<t::Failure>(&projected)) {
            return invalid_result();
          }
          v1::IdentitySetAssignmentResponse output;
          if (!assignment_record(std::get<t::AssignmentProjection>(projected),
                                 output.mutable_assignment())) {
            return invalid_result();
          }
          return output;
        },
        true);
  }
  grpc::Status RevokeAssignment(grpc::ServerContext* transport,
                                const v1::IdentityRevokeAssignmentRequest* request,
                                v1::IdentityRevokeAssignmentResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityRevokeAssignmentResponse> {
          if (!assignments_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!uuid(request->assignment_id()) || !request->has_expected_version() ||
              request->expected_version() == 0) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto reply = assignments_->revoke_assignment(
              t::RevokeAssignmentRequest{context, peer, t::AssignmentId{request->assignment_id()},
                                         t::ResourceVersion{request->expected_version()}});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::RevokeAssignmentReply>(reply);
          if (result.version.value == 0 || !positive(result.checked_at)) {
            return invalid_result();
          }
          v1::IdentityRevokeAssignmentResponse output;
          output.set_version(result.version.value);
          return output;
        },
        true);
  }
  grpc::Status
  WatchInvalidations(grpc::ServerContext* transport,
                     const v1::IdentityWatchInvalidationsRequest* request,
                     grpc::ServerWriter<v1::IdentityWatchInvalidationsResponse>* writer) override {
    std::string request_id;
    std::string trace;
    try {
      auto invocation = stream_invocation(transport, request->has_context(), request->context(),
                                          &request_id, &trace);
      if (const auto* error = std::get_if<t::Failure>(&invocation)) {
        return error_status(*error, request_id, trace);
      }
      const auto& call = std::get<StreamInvocation>(invocation);
      if (!invalidation_) {
        return error_status(failure(t::ErrorCode::CapabilityNotSupported), request_id, trace);
      }
      auto scope = resource_scope(request->scope());
      if (const auto* error = std::get_if<t::Failure>(&scope)) {
        return error_status(*error, request_id, trace);
      }
      auto subscribed = std::move(std::get<std::vector<t::ResourceRef>>(scope));
      if (subscribed.empty() || subscribed.size() > t::kMaximumInvalidationScope ||
          !t::unique_invalidation_scope(subscribed)) {
        return error_status(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput),
                            request_id, trace);
      }
      auto cursor = decode_cursor(request->cursor(), subscribed);
      if (const auto* error = std::get_if<t::Failure>(&cursor)) {
        return error_status(*error, request_id, trace);
      }
      auto opened = invalidation_->open_watch(
          {call.context, call.peer, subscribed, std::get<t::InvalidationCursor>(cursor)});
      if (const auto* error = std::get_if<t::Failure>(&opened)) {
        return error_status(*error, request_id, trace);
      }
      const auto& start = std::get<t::InvalidationWatchStart>(opened);
      if (start.watermark.epoch.empty()) {
        return error_status(invalid_result(), request_id, trace);
      }
      std::uint64_t last = std::get<t::InvalidationCursor>(cursor).sequence;
      if (start.resync_required) {
        v1::IdentityWatchInvalidationsResponse signal;
        signal.set_resync_required(true);
        if (!resync_cursor(start.watermark, subscribed, signal.mutable_cursor())) {
          return error_status(invalid_result(), request_id, trace);
        }
        if (!writer->Write(signal)) {
          return grpc::Status::OK;
        }
        last = start.watermark.sequence;
      }
      const auto send = [&](const t::InvalidationRecord& record) -> int {
        if (record.sequence <= last) {
          return 1;
        }
        v1::IdentityWatchInvalidationsResponse output;
        auto* invalidation = output.mutable_invalidation();
        if (!encode_invalidation(record, subscribed, invalidation)) {
          return 0;
        }
        last = record.sequence;
        return writer->Write(output) ? 1 : -1;
      };
      for (const auto& record : start.backlog) {
        const auto status = send(record);
        if (status <= 0) {
          return status == 0 ? error_status(invalid_result(), request_id, trace) : grpc::Status::OK;
        }
      }
      constexpr std::size_t kBatch = 32;
      while (!transport->IsCancelled() && now() < call.context.deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        if (transport->IsCancelled() || now() >= call.context.deadline) {
          break;
        }
        auto polled = invalidation_->poll(
            {call.context, call.peer, subscribed, start.watermark.epoch, last, kBatch});
        if (const auto* error = std::get_if<t::Failure>(&polled)) {
          return error_status(*error, request_id, trace);
        }
        const auto& batch = std::get<t::InvalidationPollReply>(polled);
        if (batch.resync_required) {
          v1::IdentityWatchInvalidationsResponse signal;
          signal.set_resync_required(true);
          if (!resync_cursor(batch.watermark, subscribed, signal.mutable_cursor())) {
            return error_status(invalid_result(), request_id, trace);
          }
          if (!writer->Write(signal)) {
            return grpc::Status::OK;
          }
          last = batch.watermark.sequence;
          continue;
        }
        for (const auto& record : batch.records) {
          const auto status = send(record);
          if (status <= 0) {
            return status == 0 ? error_status(invalid_result(), request_id, trace)
                               : grpc::Status::OK;
          }
        }
      }
      return grpc::Status::OK;
    } catch (...) {
      try {
        if (!request_id.empty() && !trace.empty()) {
          return error_status(failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                                      t::Acceptance::Unknown),
                              request_id, trace);
        }
      } catch (...) {
      }
      return {grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }
  grpc::Status
  GetAuthorizationSnapshot(grpc::ServerContext* transport,
                           const v1::IdentityGetAuthorizationSnapshotRequest* request,
                           v1::IdentityGetAuthorizationSnapshotResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityGetAuthorizationSnapshotResponse> {
          if (!invalidation_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          auto scope = resource_scope(request->scope());
          if (const auto* error = std::get_if<t::Failure>(&scope)) {
            return *error;
          }
          auto subscribed = std::move(std::get<std::vector<t::ResourceRef>>(scope));
          if (subscribed.empty() || subscribed.size() > t::kMaximumInvalidationScope ||
              !t::unique_invalidation_scope(subscribed)) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto cursor = decode_cursor(request->after(), subscribed);
          if (const auto* error = std::get_if<t::Failure>(&cursor)) {
            return *error;
          }
          auto reply = invalidation_->snapshot(
              {context, peer, subscribed, std::move(std::get<t::InvalidationCursor>(cursor))});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& result = std::get<t::AuthorizationSnapshotReply>(reply);
          if (!positive(result.checked_at) || result.cursor.scope.size() != subscribed.size() ||
              !cursor_scope_equal(result.cursor.scope, subscribed) ||
              result.assignments.size() > 100) {
            return invalid_result();
          }
          v1::IdentityGetAuthorizationSnapshotResponse output;
          auto* snapshot = output.mutable_snapshot();
          if (!encode_cursor(result.cursor, snapshot->mutable_cursor())) {
            return invalid_result();
          }
          snapshot->set_authorization_version(result.authorization_version.value);
          for (const auto& assignment : result.assignments) {
            if (!assignment_record(assignment, snapshot->add_assignments())) {
              return invalid_result();
            }
          }
          return output;
        },
        false);
  }
  grpc::Status GetVerificationKeys(grpc::ServerContext* transport,
                                   const v1::IdentityGetVerificationKeysRequest* request,
                                   v1::IdentityGetVerificationKeysResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityGetVerificationKeysResponse> {
          if (!verification_keys_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (request->has_known_key_version() && request->known_key_version() == 0) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          std::optional<t::ResourceVersion> known;
          if (request->has_known_key_version()) {
            known = t::ResourceVersion{request->known_key_version()};
          }
          // No delegation/JWT precheck: the Core use case authenticates the CURRENT platform
          // and its explicit trusted-Catalog grant before comparing the caller's known version.
          auto queried = verification_keys_->get_verification_keys({context, peer, known});
          if (const auto* error = std::get_if<t::Failure>(&queried)) {
            return *error;
          }
          const auto& reply = std::get<t::GetVerificationKeysReply>(queried);
          if (!positive(reply.checked_at) || reply.keys.version.value == 0 ||
              reply.keys.keys.size() != 1 ||
              reply.keys.keyset_resource.kind != t::ResourceKind::Catalog ||
              !std::holds_alternative<v1::ResourceRef>(
                  codec::to_wire(reply.keys.keyset_resource))) {
            return invalid_result();
          }
          v1::IdentityGetVerificationKeysResponse output;
          output.set_version(reply.keys.version.value);
          for (const auto& public_key : reply.keys.keys) {
            if (public_key.reference.value.empty() || public_key.reference.value.size() > 1024 ||
                public_key.reference.value.find('\0') != std::string::npos ||
                public_key.version.value == 0 || public_key.valid_until == t::TimePoint::max() ||
                public_key.valid_until <= reply.checked_at) {
              return invalid_result();
            }
            auto* key = output.add_keys();
            key->set_key_id(public_key.reference.value);
            key->set_ed25519_public_key(
                std::string(reinterpret_cast<const char*>(public_key.ed25519_public_key.data()),
                            public_key.ed25519_public_key.size()));
            key->set_version(public_key.version.value);
            if (!timestamp(public_key.valid_until, key->mutable_valid_until())) {
              return invalid_result();
            }
          }
          return output;
        },
        false);
  }
  grpc::Status GetCredentialStatus(grpc::ServerContext* transport,
                                   const v1::IdentityGetCredentialStatusRequest* request,
                                   v1::IdentityGetCredentialStatusResponse* response) override {
    return run(
        transport, request->has_context(), request->context(), response,
        [&](const t::VerifiedPeer& peer, const t::IdentityRequestContext& context)
            -> t::Outcome<v1::IdentityGetCredentialStatusResponse> {
          if (!principal_) {
            return failure(t::ErrorCode::CapabilityNotSupported);
          }
          if (!uuid(request->credential_id())) {
            return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
          }
          auto reply = principal_->get_credential_status(
              t::GetCredentialStatusQuery{context, peer, {request->credential_id()}});
          if (const auto* error = std::get_if<t::Failure>(&reply)) {
            return *error;
          }
          const auto& status = std::get<t::CredentialStatusSnapshot>(reply);
          if (status.ref.id.value != request->credential_id()) {
            return invalid_result();
          }
          auto projected = credential_status(status);
          if (const auto* error = std::get_if<t::Failure>(&projected)) {
            return *error;
          }
          v1::IdentityGetCredentialStatusResponse output;
          *output.mutable_credential() = std::get<v1::CredentialStatus>(std::move(projected));
          return output;
        },
        false);
  }

private:
  struct StreamInvocation {
    t::VerifiedPeer peer;
    t::IdentityRequestContext context;
  };
  // Streaming-read invocation gate: same real TLS/current-registration/deadline checks as unary
  // run(), but any authenticated platform service may subscribe (authorization is checked by the
  // Core use case per requested scope). Never constructs a peer from request fields.
  t::Outcome<StreamInvocation> stream_invocation(grpc::ServerContext* transport, bool has_context,
                                                 const v1::RequestContext& incoming,
                                                 std::string* request_id,
                                                 std::string* trace) noexcept {
    try {
      const bool identifiers_valid = uuid(incoming.request_id()) && uuid(incoming.trace_id());
      *request_id = uuid(incoming.request_id()) ? incoming.request_id() : random_id();
      *trace = uuid(incoming.trace_id()) ? incoming.trace_id() : random_id();
      if (request_id->empty() || trace->empty()) {
        return failure(t::ErrorCode::Internal);
      }
      if (!has_context || !identifiers_valid || !incoming.has_deadline() ||
          !key_valid(incoming.idempotency_key())) {
        return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
      }
      const auto deadline = codec::from_wire(incoming.deadline());
      const auto* requested = std::get_if<t::TimePoint>(&deadline);
      if (!requested) {
        return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
      }
      const auto started = now();
      auto effective = std::min(*requested, started + std::chrono::seconds{5});
      const auto transport_limit = transport->deadline();
      if (transport_limit == std::chrono::system_clock::time_point::max()) {
        effective = std::min(effective, started + std::chrono::seconds{2});
      } else if (transport_limit <= std::chrono::system_clock::now()) {
        return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
      } else if (transport_limit < std::chrono::system_clock::now() + std::chrono::seconds{5}) {
        const auto converted = codec::from_wire(std::get<google::protobuf::Timestamp>(
            codec::to_wire(t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                transport_limit.time_since_epoch())})));
        if (const auto* limit = std::get_if<t::TimePoint>(&converted)) {
          effective = std::min(effective, *limit);
        } else {
          return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
        }
      }
      if (invocation_deadline_) {
        auto own_limit = invocation_deadline_();
        if (const auto* error = std::get_if<t::Failure>(&own_limit)) {
          return *error;
        }
        const auto limit = std::get<t::TimePoint>(own_limit);
        if (!positive(limit)) {
          return failure(t::ErrorCode::UpstreamUnavailable);
        }
        effective = std::min(effective, limit);
      }
      if (effective <= now() || transport->IsCancelled()) {
        return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
      }
      const auto auth = transport->auth_context();
      if (!auth) {
        return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
      }
      const auto evidence = extract_peer_certificate(*auth);
      const auto* certificate = std::get_if<PeerCertificateEvidence>(&evidence);
      if (!certificate) {
        return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
      }
      auto authenticated = authenticate_(*transport);
      if (const auto* error = std::get_if<t::Failure>(&authenticated)) {
        return *error;
      }
      const auto& peer = std::get<t::VerifiedPeer>(authenticated);
      const auto* platform = std::get_if<t::PlatformPeer>(&peer.binding.peer);
      if (!platform || !uuid(platform->subject.value) ||
          peer.binding.certificate.subject != platform->subject ||
          peer.binding.certificate.platform_service != platform->service ||
          peer.binding.certificate.fingerprint != certificate->leaf_fingerprint ||
          peer.binding.credential.version.value == 0 || !uuid(peer.binding.credential.id.value) ||
          !positive(peer.verified_at)) {
        return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
      }
      if (now() >= effective || transport->IsCancelled()) {
        return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
      }
      return StreamInvocation{
          peer,
          t::IdentityRequestContext{*request_id, *trace, incoming.idempotency_key(), effective}};
    } catch (...) {
      return failure(t::ErrorCode::Internal);
    }
  }

  template <typename Output, typename Invoke>
  grpc::Status run(grpc::ServerContext* transport, bool has_context,
                   const v1::RequestContext& incoming, Output* response, Invoke invoke,
                   bool control_only = true) noexcept {
    std::string request;
    std::string trace;
    bool invoked = false;
    try {
      const bool identifiers_valid = uuid(incoming.request_id()) && uuid(incoming.trace_id());
      request = uuid(incoming.request_id()) ? incoming.request_id() : random_id();
      trace = uuid(incoming.trace_id()) ? incoming.trace_id() : random_id();
      if (request.empty() || trace.empty()) {
        return {grpc::StatusCode::INTERNAL, "INTERNAL"};
      }
      const auto fail = [&](const t::Failure& error) {
        return error_status(error, request, trace);
      };
      if (!has_context || !identifiers_valid || !incoming.has_deadline() ||
          !key_valid(incoming.idempotency_key())) {
        return fail(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
      }
      const auto deadline = codec::from_wire(incoming.deadline());
      const auto* requested = std::get_if<t::TimePoint>(&deadline);
      if (!requested) {
        return fail(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
      }
      const auto started = now();
      auto effective = std::min(*requested, started + std::chrono::seconds{5});
      const auto transport_limit = transport->deadline();
      if (transport_limit == std::chrono::system_clock::time_point::max()) {
        effective = std::min(effective, started + std::chrono::seconds{2});
      } else if (transport_limit <= std::chrono::system_clock::now()) {
        return fail(failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity));
      } else if (transport_limit < std::chrono::system_clock::now() + std::chrono::seconds{5}) {
        const auto converted = codec::from_wire(std::get<google::protobuf::Timestamp>(
            codec::to_wire(t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                transport_limit.time_since_epoch())})));
        if (const auto* limit = std::get_if<t::TimePoint>(&converted)) {
          effective = std::min(effective, *limit);
        } else {
          return fail(failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput));
        }
      }
      if (effective <= now() || transport->IsCancelled()) {
        return fail(failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity));
      }
      if (invocation_deadline_) {
        auto own_limit = invocation_deadline_();
        if (const auto* error = std::get_if<t::Failure>(&own_limit)) {
          return fail(*error);
        }
        const auto limit = std::get<t::TimePoint>(own_limit);
        if (!positive(limit)) {
          return fail(failure(t::ErrorCode::UpstreamUnavailable));
        }
        effective = std::min(effective, limit);
      }
      if (effective <= now() || transport->IsCancelled()) {
        return fail(failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity));
      }
      const auto auth = transport->auth_context();
      if (!auth) {
        return fail(failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate));
      }
      const auto evidence = extract_peer_certificate(*auth);
      const auto* certificate = std::get_if<PeerCertificateEvidence>(&evidence);
      if (!certificate) {
        return fail(failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate));
      }
      auto authenticated = authenticate_(*transport);
      if (const auto* error = std::get_if<t::Failure>(&authenticated)) {
        return fail(*error);
      }
      const auto& peer = std::get<t::VerifiedPeer>(authenticated);
      const auto* platform = std::get_if<t::PlatformPeer>(&peer.binding.peer);
      if (!platform || (control_only && platform->service != t::Service::Control) ||
          !uuid(platform->subject.value) || peer.binding.certificate.subject != platform->subject ||
          peer.binding.certificate.platform_service != platform->service ||
          peer.binding.certificate.fingerprint != certificate->leaf_fingerprint ||
          peer.binding.credential.version.value == 0 || !uuid(peer.binding.credential.id.value) ||
          !positive(peer.verified_at)) {
        return fail(failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate));
      }
      if (now() >= effective || transport->IsCancelled()) {
        return fail(failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity));
      }
      t::IdentityRequestContext context{request, trace, incoming.idempotency_key(), effective};
      invoked = true;
      auto result = invoke(peer, context);
      if (const auto* error = std::get_if<t::Failure>(&result)) {
        return fail(*error);
      }
      response->Swap(&std::get<Output>(result));
      return grpc::Status::OK;
    } catch (...) {
      try {
        if (!request.empty() && !trace.empty()) {
          return error_status(
              failure(t::ErrorCode::Internal,
                      invoked ? t::Recovery::QuerySameIdentity : t::Recovery::Diagnose,
                      invoked ? t::Acceptance::Unknown : t::Acceptance::NotAccepted),
              request, trace);
        }
      } catch (...) {
      }
      return {grpc::StatusCode::INTERNAL, "INTERNAL"};
    }
  }
  t::IdentitySecurityApplication& application_;
  PeerAuthenticator authenticate_;
  t::IdentityDelegationApplication* issuer_;
  InvocationDeadline invocation_deadline_;
  t::IdentityVerificationKeysApplication* verification_keys_;
  t::IdentityExchangeApplication* exchange_;
  t::IdentityPrincipalApplication* principal_;
  t::IdentityAuthorizationApplication* authorization_;
  t::IdentityAssignmentApplication* assignments_;
  t::IdentityInvalidationApplication* invalidation_;
  t::IdentityAuditApplication* audit_;
};
} // namespace

target::Outcome<PeerAuthenticator>
make_identity_peer_authenticator(target::IdentityAdminApplication& application,
                                 target::SecurityClockPort& clock) {
  try {
    PeerAuthenticator result =
        [&application,
         &clock](grpc::ServerContext& context) -> target::Outcome<target::VerifiedPeer> {
      try {
        const auto started = std::chrono::steady_clock::now();
        const auto expired = [&] {
          return context.IsCancelled() || context.deadline() <= std::chrono::system_clock::now() ||
                 std::chrono::steady_clock::now() - started >= std::chrono::seconds{2};
        };
        if (expired()) {
          return failure(target::ErrorCode::Timeout, target::Recovery::QuerySameIdentity);
        }
        const auto auth = context.auth_context();
        if (!auth) {
          return failure(target::ErrorCode::Unauthenticated, target::Recovery::Authenticate);
        }
        auto extracted = extract_peer_certificate(*auth);
        if (const auto* error = std::get_if<target::Failure>(&extracted)) {
          return *error;
        }
        const auto& evidence = std::get<PeerCertificateEvidence>(extracted);
        auto deadline = now() + std::chrono::seconds{2};
        if (context.deadline() < std::chrono::system_clock::now() + std::chrono::seconds{2}) {
          deadline = std::min(
              deadline, target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                            context.deadline().time_since_epoch())});
        }
        auto verified = application.authenticate_peer({evidence.leaf_fingerprint, deadline},
                                                      evidence.der_chain);
        if (const auto* error = std::get_if<target::Failure>(&verified)) {
          return *error;
        }
        const auto& peer = std::get<target::VerifiedPeer>(verified);
        if (peer.binding.certificate.fingerprint != evidence.leaf_fingerprint ||
            !positive(peer.verified_at)) {
          return failure(target::ErrorCode::Unauthenticated, target::Recovery::Authenticate);
        }
        if (expired()) {
          return failure(target::ErrorCode::Timeout, target::Recovery::QuerySameIdentity);
        }
        auto sample = clock.sample();
        if (const auto* error = std::get_if<target::Failure>(&sample)) {
          return *error;
        }
        const auto& instant = std::get<target::SecurityClockSample>(sample);
        if (!instant.trusted || instant.maximum_known_skew < std::chrono::milliseconds::zero() ||
            instant.maximum_known_skew > target::kMaximumKnownClockSkew || !positive(instant.utc) ||
            std::chrono::abs(instant.utc - peer.verified_at) > target::kMaximumKnownClockSkew) {
          return failure(target::ErrorCode::UpstreamUnavailable);
        }
        if (expired()) {
          return failure(target::ErrorCode::Timeout, target::Recovery::QuerySameIdentity);
        }
        return peer;
      } catch (...) {
        return failure(target::ErrorCode::Internal);
      }
    };
    return result;
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}

target::Outcome<std::unique_ptr<v1::IdentityService::Service>>
make_identity_rpc(target::IdentitySecurityApplication& application, PeerAuthenticator authenticate,
                  target::IdentityDelegationApplication* issuer,
                  InvocationDeadline invocation_deadline, IdentityRpcOptions options) {
  try {
    if (!authenticate) {
      return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
    }
    std::unique_ptr<v1::IdentityService::Service> result = std::make_unique<IdentityRpc>(
        application, std::move(authenticate), issuer, std::move(invocation_deadline), options);
    return result;
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::platform
