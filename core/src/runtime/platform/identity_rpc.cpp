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
#include <utility>

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
class IdentityRpc final : public v1::IdentityService::Service {
public:
  IdentityRpc(t::IdentitySecurityApplication& application, PeerAuthenticator authenticate,
              t::IdentityDelegationApplication* issuer, InvocationDeadline invocation_deadline,
              IdentityRpcOptions options)
      : application_(application), authenticate_(std::move(authenticate)), issuer_(issuer),
        invocation_deadline_(std::move(invocation_deadline)),
        verification_keys_(options.verification_keys), exchange_(options.exchange) {}

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
                                 v1::IdentityRegisterPrincipalResponse*) override {
    v1::IdentityRegisterPrincipalResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityRegisterPrincipalResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status RotateCredential(grpc::ServerContext* transport,
                                const v1::IdentityRotateCredentialRequest* request,
                                v1::IdentityRotateCredentialResponse*) override {
    v1::IdentityRotateCredentialResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityRotateCredentialResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status RevokeCredential(grpc::ServerContext* transport,
                                const v1::IdentityRevokeCredentialRequest* request,
                                v1::IdentityRevokeCredentialResponse*) override {
    v1::IdentityRevokeCredentialResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityRevokeCredentialResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status GetPrincipalStatus(grpc::ServerContext* transport,
                                  const v1::IdentityGetPrincipalStatusRequest* request,
                                  v1::IdentityGetPrincipalStatusResponse*) override {
    v1::IdentityGetPrincipalStatusResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityGetPrincipalStatusResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
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
                                  v1::IdentityCheckAuthorizationResponse*) override {
    v1::IdentityCheckAuthorizationResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityCheckAuthorizationResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status ListAssignments(grpc::ServerContext* transport,
                               const v1::IdentityListAssignmentsRequest* request,
                               v1::IdentityListAssignmentsResponse*) override {
    v1::IdentityListAssignmentsResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityListAssignmentsResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status SetAssignment(grpc::ServerContext* transport,
                             const v1::IdentitySetAssignmentRequest* request,
                             v1::IdentitySetAssignmentResponse*) override {
    v1::IdentitySetAssignmentResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentitySetAssignmentResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status RevokeAssignment(grpc::ServerContext* transport,
                                const v1::IdentityRevokeAssignmentRequest* request,
                                v1::IdentityRevokeAssignmentResponse*) override {
    v1::IdentityRevokeAssignmentResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityRevokeAssignmentResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status
  WatchInvalidations(grpc::ServerContext* transport,
                     const v1::IdentityWatchInvalidationsRequest* request,
                     grpc::ServerWriter<v1::IdentityWatchInvalidationsResponse>*) override {
    v1::IdentityWatchInvalidationsResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&,
           const t::IdentityRequestContext&) -> t::Outcome<v1::IdentityWatchInvalidationsResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }
  grpc::Status GetAuthorizationSnapshot(grpc::ServerContext* transport,
                                        const v1::IdentityGetAuthorizationSnapshotRequest* request,
                                        v1::IdentityGetAuthorizationSnapshotResponse*) override {
    v1::IdentityGetAuthorizationSnapshotResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&, const t::IdentityRequestContext&)
            -> t::Outcome<v1::IdentityGetAuthorizationSnapshotResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
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
                                   v1::IdentityGetCredentialStatusResponse*) override {
    v1::IdentityGetCredentialStatusResponse unused;
    return run(
        transport, request->has_context(), request->context(), &unused,
        [](const t::VerifiedPeer&, const t::IdentityRequestContext&)
            -> t::Outcome<v1::IdentityGetCredentialStatusResponse> {
          return failure(t::ErrorCode::CapabilityNotSupported);
        },
        false);
  }

private:
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
