#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

inline constexpr auto kAccessTokenLifetime = std::chrono::minutes{15};
inline constexpr auto kRefreshTokenLifetime = std::chrono::hours{24 * 30};
inline constexpr auto kSessionSnapshotLifetime = std::chrono::minutes{10};
// Existing public API global ceiling; per-subject limits do not replace this bound.
inline constexpr std::size_t kMaximumUnexpiredSessions = 4096;
inline constexpr auto kSecretReceiptRetention = std::chrono::hours{24};
inline constexpr auto kDelegationBodyLifetime = std::chrono::seconds{56};
inline constexpr auto kMaximumAuthorizationAge = std::chrono::seconds{60};
inline constexpr auto kMaximumKnownClockSkew = std::chrono::seconds{2};
inline constexpr auto kMaximumSignatureLeeway = std::chrono::seconds{2};
inline constexpr std::uint32_t kMaximumBusinessHops = 2;
inline constexpr std::size_t kMaximumDelegationBindings = 128;
inline constexpr std::size_t kMaximumPeerChainBytes = 65536;
inline constexpr std::size_t kMaximumPeerChainCertificates = 8;
struct CredentialTag;
struct CryptoKeyTag;
using CredentialId = Identifier<CredentialTag>;
using CryptoKeyRef = Identifier<CryptoKeyTag>;
struct CredentialVersionRef {
  CredentialId id;
  ResourceVersion version;
  bool operator==(const CredentialVersionRef&) const = default;
};
// Implemented by mature crypto adapter, with secure allocation/erasure on destruction. No default
// vector/string backing or stream formatting: Core does not implement a wiping/password algorithm.
class SecretBuffer {
public:
  virtual ~SecretBuffer() = default;
  virtual std::span<const std::byte> bytes() const noexcept = 0;
};
using OwnedSecret = std::unique_ptr<SecretBuffer>;
using SecretView = std::span<const std::byte>;
enum class TokenPurpose { Access, Refresh, WorkloadBootstrap };
struct TokenVerifier {
  TokenPurpose purpose;
  CryptoKeyRef key;
  Sha256Digest tag;
};
struct RequestCommitment {
  CryptoKeyRef key;
  Sha256Digest tag;
  bool operator==(const RequestCommitment&) const = default;
};
struct PasswordVerifier {
  // Encoded verification material only; never a plaintext password or raw password SHA.
  std::string encoded;
  ResourceVersion hash_policy_version;
};
struct WorkloadBootstrapVerifier {
  TokenVerifier verifier;
  ResourceRef binding;
  std::vector<Service> audiences;
  TimePoint expires_at;
};
struct CertificateBinding {
  SubjectId subject;
  Sha256Digest fingerprint;
  std::string registered_san;
  std::optional<Service> platform_service;
  TimePoint not_before;
  TimePoint not_after;
};
enum class CredentialKind { Unspecified, Password, WorkloadBootstrap, CertificateBinding };
// A registered stub has a kind but NO secret/verifier. No fabricated empty password hash.
struct PendingCredentialMaterial {
  CredentialKind kind = CredentialKind::Unspecified;
};
enum class CredentialState { Pending, Active, Revoked };
using CredentialMaterial = std::variant<PendingCredentialMaterial, PasswordVerifier,
                                        WorkloadBootstrapVerifier, CertificateBinding>;
struct CredentialVersionRecord {
  CredentialVersionRef ref;
  SubjectId subject;
  CredentialMaterial material;
  CredentialState state = CredentialState::Pending;
  TimePoint created_at;
  std::optional<TimePoint> revoked_at;
  ResourceRef binding;
  // Authoritative finite expiry for all issued credentials; Pending has none. Password expiry
  // comes from trusted policy, not an empty/default time. Expired is only a current-UTC projection.
  std::optional<TimePoint> expires_at;
};
enum class CredentialStatusState { Pending, Active, Expired, Revoked };
struct CredentialStatusSnapshot {
  CredentialVersionRef ref;
  SubjectId subject;
  ResourceRef binding;
  CredentialStatusState state;
  TimePoint created_at;
  std::optional<TimePoint> expires_at;
  TimePoint checked_at;
};
struct LoginIdentity {
  std::string normalized_login;
  SubjectId subject;
  CredentialId password_credential;
  ResourceVersion version;
};
// Persisted at Login. Later delegation checks the current certificate head/principal gate of
// this original Control, even when the actual forwarding caller is Runtime. No cached cert grant.
struct SessionControlPeer {
  SubjectId subject;
  CredentialId credential;
  bool operator==(const SessionControlPeer&) const = default;
};
inline bool valid_session_control_peer(const SessionControlPeer& peer) {
  return !peer.subject.value.empty() && !peer.credential.value.empty();
}
struct SessionSecurity {
  SessionId id;
  SubjectId subject;
  CredentialVersionRef credential;
  TokenVerifier access;
  TokenVerifier refresh;
  TimePoint created_at;
  TimePoint access_expires_at;
  TimePoint refresh_expires_at;
  std::uint64_t rotation;
  ResourceVersion version;
  bool revoked = false;
  // Login persists the verified original Control; absent never means arbitrary Control.
  std::optional<SessionControlPeer> control_peer = std::nullopt;
};
struct EphemeralTokenPair {
  SessionId session;
  OwnedSecret access;
  OwnedSecret refresh;
  TimePoint access_expires_at;
  TimePoint refresh_expires_at;
  std::uint64_t rotation;
};
struct EphemeralCredentialDelivery {
  CredentialVersionRef credential;
  OwnedSecret secret;
  TimePoint expires_at;
};
using SecretReplayPayload = std::variant<EphemeralTokenPair, EphemeralCredentialDelivery>;
// Each role/permission is paired with its exact resource; never form roles x resources across rows.
struct ScopedRole {
  FixedRole role;
  ResourceRef resource;
  bool operator==(const ScopedRole&) const = default;
};
struct PermissionBinding {
  Permission permission;
  ResourceRef resource;
  bool operator==(const PermissionBinding&) const = default;
};
// G08 request basis: fixed human role XOR nonempty explicit permissions. The variant makes
// mixing both bases unrepresentable; application validates human-only role and nonempty scope.
struct RoleAssignmentSpec {
  FixedRole role;
  std::vector<ResourceRef> scope;
};
struct ExplicitAssignmentSpec {
  std::vector<Permission> permissions;
  std::vector<ResourceRef> scope;
};
struct ExactAssignmentSpec {
  SubjectId subject;
  std::variant<RoleAssignmentSpec, ExplicitAssignmentSpec> basis;
};
struct ExactAssignment {
  AssignmentId id;
  SubjectId subject;
  std::vector<ScopedRole> roles;
  std::vector<PermissionBinding> explicit_permissions;
  ResourceVersion version;
};
struct LoginReceiptScope {
  SubjectId control_peer;
  std::string normalized_login;
  std::string idempotency_key;
  bool operator==(const LoginReceiptScope&) const = default;
};
struct RefreshReceiptScope {
  SubjectId control_peer;
  SessionId session;
  std::string idempotency_key;
  bool operator==(const RefreshReceiptScope&) const = default;
};
struct CredentialDeliveryScope {
  SubjectId actor;
  CredentialId credential;
  std::string idempotency_key;
  bool operator==(const CredentialDeliveryScope&) const = default;
};
using SecretReceiptScope =
    std::variant<LoginReceiptScope, RefreshReceiptScope, CredentialDeliveryScope>;
struct LoginRequestParameters {
  bool operator==(const LoginRequestParameters&) const = default;
};
struct RefreshRequestParameters {
  std::uint64_t input_rotation;
  bool operator==(const RefreshRequestParameters&) const = default;
};
struct CredentialDeliveryParameters {
  ResourceVersion expected_version;
  bool operator==(const CredentialDeliveryParameters&) const = default;
};
using SecretRequestParameters =
    std::variant<LoginRequestParameters, RefreshRequestParameters, CredentialDeliveryParameters>;
struct RefreshRecoveryLookup {
  SubjectId control_peer;
  std::string idempotency_key;
};
// Resolution is NOT authentication. Consumed input locates only its original, still-bounded
// recovery attempt; application must recheck current result rotation/credential/session before
// open.
struct CurrentRefreshHandle {
  SessionId session;
  SubjectId subject;
  CredentialVersionRef credential;
  TokenVerifier input_verifier;
  std::uint64_t input_rotation;
  TimePoint input_expires_at;
};
struct ConsumedRefreshHandle {
  CurrentRefreshHandle input;
  RefreshReceiptScope receipt_scope;
  TimePoint consumed_at;
  TimePoint recovery_until;
};
using RefreshHandle = std::variant<CurrentRefreshHandle, ConsumedRefreshHandle>;
struct SecretReceiptBinding {
  SecretReceiptScope scope;
  SubjectId subject;
  std::optional<SessionId> session;
  CredentialVersionRef credential;
  SecretRequestParameters parameters;
  std::uint64_t result_rotation;
  RequestCommitment request;
  TimePoint created_at;
  TimePoint retain_until;
  bool operator==(const SecretReceiptBinding&) const = default;
};
struct SealedSecret {
  CryptoKeyRef key;
  std::uint32_t format_version;
  std::vector<std::byte> nonce;
  std::vector<std::byte> ciphertext;
};
struct EncryptedSecretReceipt {
  SecretReceiptBinding binding;
  SealedSecret sealed;
};
struct SecretReceiptCapacity {
  // Required trusted application policy, not a request parameter. No valid-receipt eviction.
  std::size_t max_records;
};
struct SecretReceiptWrite {
  EncryptedSecretReceipt receipt;
  bool duplicate;
};
struct RegisteredPeer {
  DirectPeer peer;
  CertificateBinding certificate;
  CredentialVersionRef credential;
};
// Produced only by boundary verification against registration/current credential, not by parsing
// caller-submitted JSON. Valid certificate chain alone is not business authorization.
struct VerifiedPeer {
  RegisteredPeer binding;
  TimePoint verified_at;
};
struct SecurityClockSample {
  TimePoint utc;
  std::chrono::steady_clock::time_point monotonic;
  std::chrono::milliseconds maximum_known_skew;
  bool trusted;
};
struct DelegationClaims {
  std::string issuer;
  Service audience;
  SubjectId subject;
  SubjectId direct_actor;
  std::vector<SubjectId> actor_chain;
  std::vector<PermissionBinding> permissions;
  MessageId proof_id;
  Sha256Digest client_certificate;
  ResourceVersion authorization_version;
  TimePoint issued_at;
  TimePoint not_before;
  TimePoint expires_at;
  std::uint32_t business_hops;
  // Signed provenance. Exchange preserves both fields exactly; authversion alone cannot prove
  // that the original login session/current credential has not been revoked or rotated.
  CredentialVersionRef source_credential;
  std::optional<SessionId> source_session;
};
// Signature and authenticated recipient audience only. This is NOT evidence that the business
// recipient observed the signed direct actor's TLS connection; its owner must use ordinary verify.
struct AuthorityVerifiedDelegation {
  DelegationClaims claims;
};
struct VerifiedDelegation {
  DelegationClaims claims;
  CryptoKeyRef signing_key;
};
enum class BootstrapState { Required, Consumed };
struct AdministratorBootstrap {
  BootstrapState state = BootstrapState::Required;
  ResourceVersion version;
  std::optional<SubjectId> administrator;
  std::optional<TimePoint> consumed_at;
};

} // namespace kiarabot::core::target
