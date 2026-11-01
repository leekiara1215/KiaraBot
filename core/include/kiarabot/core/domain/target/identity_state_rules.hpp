#pragma once

#include <algorithm>
#include <limits>

#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace kiarabot::core::target {

enum class RefreshDecision { Rotate, ReplayOriginal };
inline Failure authentication_rejected() {
  return rejected(ErrorCode::Unauthenticated, Recovery::Authenticate);
}
inline Outcome<TimePoint> identity_deadline(TimePoint start, std::chrono::nanoseconds duration) {
  if (duration <= std::chrono::nanoseconds::zero() || start > TimePoint::max() - duration) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return start + duration;
}
inline Status require_current_credential(const Principal& subject,
                                         const CredentialVersionRecord& credential,
                                         const CredentialVersionRef& expected, TimePoint now) {
  if (subject.state != LifecycleState::Active || subject.id.value.empty() ||
      subject.version.value == 0 || credential.subject != subject.id ||
      credential.ref != expected || credential.state != CredentialState::Active ||
      now < credential.created_at) {
    return authentication_rejected();
  }
  if (std::holds_alternative<Failure>(validate_credential_record(credential))) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  if (!credential.expires_at || now >= *credential.expires_at) {
    return authentication_rejected();
  }
  if (const auto* bootstrap = std::get_if<WorkloadBootstrapVerifier>(&credential.material)) {
    if (now >= bootstrap->expires_at) {
      return authentication_rejected();
    }
  } else if (const auto* certificate = std::get_if<CertificateBinding>(&credential.material)) {
    if (now < certificate->not_before || now >= certificate->not_after) {
      return authentication_rejected();
    }
  }
  return std::monostate{};
}
// State checks only. Caller has verified the raw token using the mature crypto port; never treat
// a matching lookup row or RefreshHandle alone as proof of possession.
inline Status require_session_current(const Principal& subject,
                                      const CredentialVersionRecord& credential,
                                      const SessionSecurity& session, TokenPurpose purpose,
                                      TimePoint now) {
  const auto current = require_current_credential(subject, credential, session.credential, now);
  if (std::holds_alternative<Failure>(current)) {
    return current;
  }
  if (purpose != TokenPurpose::Access && purpose != TokenPurpose::Refresh) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (subject.kind != PrincipalKind::Human ||
      !std::holds_alternative<PasswordVerifier>(credential.material) ||
      session.subject != subject.id || session.id.value.empty() || session.revoked ||
      now < session.created_at || now >= session.refresh_expires_at ||
      (purpose == TokenPurpose::Access && now >= session.access_expires_at)) {
    return authentication_rejected();
  }
  if (session.rotation == 0 || session.version.value == 0 ||
      session.access.purpose != TokenPurpose::Access ||
      session.refresh.purpose != TokenPurpose::Refresh || session.access.key.value.empty() ||
      session.refresh.key.value.empty() || session.access_expires_at <= session.created_at ||
      session.refresh_expires_at < session.access_expires_at) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  return std::monostate{};
}
// Explicit signed provenance is re-read online, even if authorization_version is unchanged.
inline Status require_delegation_origin_current(const DelegationClaims& claims,
                                                const Principal& subject,
                                                const CredentialVersionRecord& source_credential,
                                                const SessionSecurity* source_session,
                                                TimePoint now) {
  if (std::holds_alternative<Failure>(validate_delegation_origin(claims)) ||
      claims.subject != subject.id) {
    return authentication_rejected();
  }
  const auto credential =
      require_current_credential(subject, source_credential, claims.source_credential, now);
  if (std::holds_alternative<Failure>(credential)) {
    return credential;
  }
  if (claims.source_session) {
    if (!source_session || source_session->id != *claims.source_session) {
      return authentication_rejected();
    }
    return require_session_current(subject, source_credential, *source_session,
                                   TokenPurpose::Access, now);
  }
  if (source_session || subject.kind == PrincipalKind::Human) {
    return authentication_rejected();
  }
  return std::monostate{};
}
inline bool same_token_verifier(const TokenVerifier& left, const TokenVerifier& right) {
  // Compare already authenticated metadata/index values, not raw secret equality/crypto
  // verification.
  return left.purpose == right.purpose && left.key == right.key && left.tag == right.tag;
}
inline Outcome<RefreshDecision>
check_refresh_state(const Principal& subject, const CredentialVersionRecord& credential,
                    const SessionSecurity& current, const RefreshHandle& handle,
                    const RefreshRecoveryLookup& lookup, TimePoint now) {
  const auto session =
      require_session_current(subject, credential, current, TokenPurpose::Refresh, now);
  if (const auto* failure = std::get_if<Failure>(&session)) {
    return *failure;
  }
  if (lookup.control_peer.value.empty() || lookup.idempotency_key.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const auto* active = std::get_if<CurrentRefreshHandle>(&handle);
  const auto* consumed = std::get_if<ConsumedRefreshHandle>(&handle);
  const auto& input = active ? *active : consumed->input;
  if (input.session != current.id || input.subject != subject.id ||
      input.credential != current.credential || input.input_rotation == 0 ||
      input.input_verifier.purpose != TokenPurpose::Refresh ||
      input.input_verifier.key.value.empty() || now >= input.input_expires_at) {
    return authentication_rejected();
  }
  if (active) {
    if (input.input_rotation != current.rotation ||
        input.input_expires_at != current.refresh_expires_at ||
        !same_token_verifier(input.input_verifier, current.refresh)) {
      return authentication_rejected();
    }
    if (current.rotation == std::numeric_limits<std::uint64_t>::max()) {
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    }
    return RefreshDecision::Rotate;
  }
  if (consumed->receipt_scope.control_peer != lookup.control_peer ||
      consumed->receipt_scope.idempotency_key != lookup.idempotency_key ||
      consumed->receipt_scope.session != current.id || now >= consumed->recovery_until ||
      consumed->consumed_at > now ||
      input.input_rotation == std::numeric_limits<std::uint64_t>::max() ||
      current.rotation != input.input_rotation + 1) {
    return authentication_rejected();
  }
  const auto latest = identity_deadline(consumed->consumed_at, kSecretReceiptRetention);
  if (std::holds_alternative<Failure>(latest) ||
      consumed->recovery_until > std::get<TimePoint>(latest) ||
      consumed->recovery_until > input.input_expires_at ||
      consumed->recovery_until <= consumed->consumed_at) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  return RefreshDecision::ReplayOriginal;
}
inline Status validate_secret_receipt(const EncryptedSecretReceipt& receipt) {
  const auto& binding = receipt.binding;
  const auto parameters = validate_secret_request_parameters(binding.scope, binding.parameters);
  if (std::holds_alternative<Failure>(parameters)) {
    return parameters;
  }
  const auto latest = identity_deadline(binding.created_at, kSecretReceiptRetention);
  if (std::holds_alternative<Failure>(latest) || binding.retain_until <= binding.created_at ||
      binding.retain_until > std::get<TimePoint>(latest) || binding.subject.value.empty() ||
      binding.credential.id.value.empty() || binding.credential.version.value == 0 ||
      binding.request.key.value.empty() || receipt.sealed.key.value.empty() ||
      receipt.sealed.format_version == 0 || receipt.sealed.nonce.empty() ||
      receipt.sealed.ciphertext.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (const auto* scope = std::get_if<RefreshReceiptScope>(&binding.scope)) {
    const auto& request = std::get<RefreshRequestParameters>(binding.parameters);
    if (!binding.session || *binding.session != scope->session ||
        request.input_rotation == std::numeric_limits<std::uint64_t>::max() ||
        binding.result_rotation != request.input_rotation + 1) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  } else if (std::holds_alternative<LoginReceiptScope>(binding.scope)) {
    if (!binding.session || binding.session->value.empty() || binding.result_rotation != 1) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  } else {
    const auto& scope = std::get<CredentialDeliveryScope>(binding.scope);
    const auto& request = std::get<CredentialDeliveryParameters>(binding.parameters);
    if (binding.session || binding.result_rotation != 0 ||
        scope.credential != binding.credential.id ||
        request.expected_version.value == std::numeric_limits<std::uint64_t>::max() ||
        binding.credential.version.value != request.expected_version.value + 1) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  }
  return std::monostate{};
}
// Actor authorization/password/token verification must already succeed. For credential delivery,
// subject is the TARGET credential owner; the currently authorized actor is bound by request_scope.
// Current subject/credential/session state is checked BEFORE any old success/commitment replay.
inline Status require_secret_replay(const EncryptedSecretReceipt& receipt,
                                    const SecretReceiptScope& request_scope,
                                    const SecretRequestParameters& parameters,
                                    const RequestCommitment& commitment, const Principal& subject,
                                    const CredentialVersionRecord& credential,
                                    const SessionSecurity* session, TimePoint now) {
  const auto current =
      require_current_credential(subject, credential, receipt.binding.credential, now);
  if (std::holds_alternative<Failure>(current)) {
    return current;
  }
  if (receipt.binding.subject != subject.id) {
    return authentication_rejected();
  }
  if (receipt.binding.session) {
    if (!session || session->id != *receipt.binding.session ||
        session->rotation != receipt.binding.result_rotation) {
      return authentication_rejected();
    }
    const auto valid =
        require_session_current(subject, credential, *session, TokenPurpose::Refresh, now);
    if (std::holds_alternative<Failure>(valid)) {
      return valid;
    }
  } else if (session) {
    return authentication_rejected();
  }
  if (now < receipt.binding.created_at || now >= receipt.binding.retain_until ||
      receipt.binding.scope != request_scope) {
    return authentication_rejected();
  }
  if (std::holds_alternative<Failure>(validate_secret_receipt(receipt))) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  if (receipt.binding.parameters != parameters || receipt.binding.request != commitment) {
    return rejected(ErrorCode::Conflict);
  }
  return std::monostate{};
}
inline Status validate_delegation_shape(const DelegationClaims& claims) {
  const auto origin = validate_delegation_origin(claims);
  if (std::holds_alternative<Failure>(origin)) {
    return origin;
  }
  const auto maximum = identity_deadline(claims.issued_at, kDelegationBodyLifetime);
  if (claims.issuer.empty() || !valid_service(claims.audience) || claims.subject.value.empty() ||
      claims.direct_actor.value.empty() || claims.proof_id.value.empty() ||
      claims.authorization_version.value == 0 || claims.business_hops == 0 ||
      claims.business_hops > kMaximumBusinessHops ||
      claims.actor_chain.size() != claims.business_hops ||
      claims.actor_chain.back() != claims.direct_actor || claims.permissions.empty() ||
      std::holds_alternative<Failure>(maximum) || claims.expires_at <= claims.issued_at ||
      claims.expires_at > std::get<TimePoint>(maximum) || claims.not_before >= claims.expires_at) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  for (std::size_t i = 0; i < claims.actor_chain.size(); ++i) {
    if (claims.actor_chain[i].value.empty()) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (claims.actor_chain[i] == claims.actor_chain[j]) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
  }
  for (const auto& binding : claims.permissions) {
    if (!known_permission(binding.permission) || !valid_policy_resource(binding.resource)) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  }
  return std::monostate{};
}
// Business edges: Control->Runtime hops1/[Control]; Runtime->Model hops2/[Control,Runtime].
// Identity sign/exchange RPCs and TLS handshakes do not add an actor/hop.
inline Status validate_delegation_exchange(const DelegationClaims& parent,
                                           const DelegationClaims& child,
                                           Service forwarding_service,
                                           const SubjectId& forwarding_actor) {
  if (std::holds_alternative<Failure>(validate_delegation_shape(parent)) ||
      std::holds_alternative<Failure>(validate_delegation_shape(child))) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const auto origin = require_preserved_origin(parent, child);
  if (std::holds_alternative<Failure>(origin)) {
    return origin;
  }
  if (parent.business_hops >= kMaximumBusinessHops ||
      child.business_hops != parent.business_hops + 1 || forwarding_service != parent.audience ||
      child.audience == parent.audience || child.issuer != parent.issuer ||
      child.direct_actor != forwarding_actor ||
      child.actor_chain.size() != parent.actor_chain.size() + 1 ||
      child.actor_chain.back() != forwarding_actor || child.expires_at > parent.expires_at ||
      child.issued_at < parent.issued_at || child.not_before < parent.not_before) {
    return rejected(ErrorCode::PermissionDenied);
  }
  for (std::size_t i = 0; i < parent.actor_chain.size(); ++i) {
    if (child.actor_chain[i] != parent.actor_chain[i]) {
      return rejected(ErrorCode::PermissionDenied);
    }
  }
  for (const auto& binding : child.permissions) {
    if (std::find(parent.permissions.begin(), parent.permissions.end(), binding) ==
        parent.permissions.end()) {
      return rejected(ErrorCode::PermissionDenied);
    }
  }
  return std::monostate{};
}
inline Outcome<AdministratorBootstrap>
consume_administrator_bootstrap(const AdministratorBootstrap& current, ResourceVersion expected,
                                SubjectId administrator, TimePoint now) {
  if (current.state != BootstrapState::Required) {
    return rejected(ErrorCode::Conflict);
  }
  if (administrator.value.empty() || current.administrator || current.consumed_at) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const auto next = next_version(current.version, expected);
  if (const auto* failure = std::get_if<Failure>(&next)) {
    return *failure;
  }
  return AdministratorBootstrap{BootstrapState::Consumed, std::get<ResourceVersion>(next),
                                std::move(administrator), now};
}

} // namespace kiarabot::core::target
