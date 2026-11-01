#pragma once

#include <utility>

#include "kiarabot/core/domain/target/identity_types.hpp"
#include "kiarabot/core/domain/target/persistence_rules.hpp"

namespace kiarabot::core::target {

inline bool valid_credential_kind(CredentialKind kind) {
  switch (kind) {
  case CredentialKind::Password:
  case CredentialKind::WorkloadBootstrap:
  case CredentialKind::CertificateBinding:
    return true;
  case CredentialKind::Unspecified:
    return false;
  }
  return false;
}
// Structural state/material validation only. Algorithm/hash/certificate verification belongs to
// mature crypto adapters; expiration is checked at authentication UTC, not a stored fake terminal.
inline Status validate_credential_record(const CredentialVersionRecord& record) {
  if (record.ref.id.value.empty() || record.ref.version.value == 0 ||
      record.subject.value.empty() || !valid_resource_ref(record.binding)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const auto* pending = std::get_if<PendingCredentialMaterial>(&record.material);
  switch (record.state) {
  case CredentialState::Pending:
    if (!pending || !valid_credential_kind(pending->kind) || record.revoked_at ||
        record.expires_at) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    return std::monostate{};
  case CredentialState::Active:
    if (pending || record.revoked_at || !record.expires_at ||
        *record.expires_at <= record.created_at) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    break;
  case CredentialState::Revoked:
    if (!record.revoked_at || *record.revoked_at < record.created_at) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    if (record.expires_at && *record.expires_at <= record.created_at) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    // Revoking an unissued stub is a real revocation, not an active empty verifier.
    if (pending) {
      return valid_credential_kind(pending->kind)
                 ? Status{std::monostate{}}
                 : Status{rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput)};
    }
    break;
  default:
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (const auto* password = std::get_if<PasswordVerifier>(&record.material)) {
    if (password->encoded.empty() || password->hash_policy_version.value == 0) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  } else if (const auto* bootstrap = std::get_if<WorkloadBootstrapVerifier>(&record.material)) {
    if (bootstrap->verifier.purpose != TokenPurpose::WorkloadBootstrap ||
        bootstrap->verifier.key.value.empty() || !valid_resource_ref(bootstrap->binding) ||
        bootstrap->audiences.empty() || bootstrap->expires_at <= record.created_at ||
        bootstrap->binding != record.binding ||
        (record.expires_at && *record.expires_at > bootstrap->expires_at)) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    for (const auto audience : bootstrap->audiences) {
      if (!valid_service(audience)) {
        return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
    }
  } else if (const auto* certificate = std::get_if<CertificateBinding>(&record.material)) {
    if (certificate->subject != record.subject || certificate->registered_san.empty() ||
        certificate->not_after <= certificate->not_before ||
        (record.expires_at && *record.expires_at > certificate->not_after) ||
        (certificate->platform_service && !valid_service(*certificate->platform_service))) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
  } else {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}
inline Outcome<CredentialVersionRecord> make_pending_credential(CredentialId id, SubjectId subject,
                                                                CredentialKind kind,
                                                                ResourceRef binding,
                                                                TimePoint now) {
  CredentialVersionRecord record{{std::move(id), {1}},
                                 std::move(subject),
                                 PendingCredentialMaterial{kind},
                                 CredentialState::Pending,
                                 now,
                                 std::nullopt,
                                 std::move(binding),
                                 std::nullopt};
  const auto validation = validate_credential_record(record);
  if (const auto* failure = std::get_if<Failure>(&validation)) {
    return *failure;
  }
  return record;
}
// Public status carries no material; Revoked takes precedence, Expired is not a persisted state.
inline Outcome<CredentialStatusSnapshot> credential_status(const CredentialVersionRecord& record,
                                                           TimePoint checked_at) {
  const auto validation = validate_credential_record(record);
  if (std::holds_alternative<Failure>(validation) || checked_at < record.created_at) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  CredentialStatusState state = CredentialStatusState::Pending;
  if (record.state == CredentialState::Revoked) {
    state = CredentialStatusState::Revoked;
  } else if (record.state == CredentialState::Active) {
    state = checked_at >= *record.expires_at ? CredentialStatusState::Expired
                                             : CredentialStatusState::Active;
  }
  return CredentialStatusSnapshot{record.ref,        record.subject,    record.binding, state,
                                  record.created_at, record.expires_at, checked_at};
}
// Scope is the stable uniqueness key. Parameters are authenticated request CONTENT, so changing
// expected_version/input rotation under the same key cannot escape a commitment conflict.
inline Status validate_secret_request_parameters(const SecretReceiptScope& scope,
                                                 const SecretRequestParameters& parameters) {
  if (const auto* login = std::get_if<LoginReceiptScope>(&scope)) {
    if (!login->control_peer.value.empty() && !login->normalized_login.empty() &&
        !login->idempotency_key.empty() &&
        std::holds_alternative<LoginRequestParameters>(parameters)) {
      return std::monostate{};
    }
  } else if (const auto* refresh = std::get_if<RefreshReceiptScope>(&scope)) {
    const auto* request = std::get_if<RefreshRequestParameters>(&parameters);
    if (!refresh->control_peer.value.empty() && !refresh->session.value.empty() &&
        !refresh->idempotency_key.empty() && request && request->input_rotation != 0) {
      return std::monostate{};
    }
  } else if (const auto* delivery = std::get_if<CredentialDeliveryScope>(&scope)) {
    const auto* request = std::get_if<CredentialDeliveryParameters>(&parameters);
    if (!delivery->actor.value.empty() && !delivery->credential.value.empty() &&
        !delivery->idempotency_key.empty() && request && request->expected_version.value != 0) {
      return std::monostate{};
    }
  }
  return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
inline Status validate_delegation_origin(const DelegationClaims& claims) {
  if (claims.source_credential.id.value.empty() || claims.source_credential.version.value == 0 ||
      (claims.source_session && claims.source_session->value.empty())) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}
inline Status require_preserved_origin(const DelegationClaims& parent,
                                       const DelegationClaims& child) {
  const auto parent_origin = validate_delegation_origin(parent);
  if (std::holds_alternative<Failure>(parent_origin)) {
    return parent_origin;
  }
  const auto child_origin = validate_delegation_origin(child);
  if (std::holds_alternative<Failure>(child_origin)) {
    return child_origin;
  }
  if (parent.subject != child.subject || parent.source_credential != child.source_credential ||
      parent.source_session != child.source_session) {
    return rejected(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}
inline SessionMetadata session_metadata(const SessionSecurity& session) {
  return {session.id,       session.subject, session.created_at, session.refresh_expires_at,
          session.rotation, session.version, session.revoked};
}

} // namespace kiarabot::core::target
