#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/ports/target/identity_crypto.hpp"
#include "kiarabot/core/ports/target/identity_repository.hpp"
#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::target {

struct IdentityRequestContext {
  std::string request_id;
  std::string trace_id;
  std::string idempotency_key;
  TimePoint deadline;
};
// VerifiedPeer is constructed from actual TLS verification, never request JSON. Application still
// checks registered peer subject/current credential. Secret spans borrow request memory until
// return.
struct LoginRequest {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  std::string username;
  SecretView password;
};
struct RefreshSessionRequest {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  SecretView refresh_token;
};
struct AccessSessionRequest {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  SecretView access_token;
};
struct IdentityActorProof {
  VerifiedPeer direct_peer;
  // Fixed certificate-bound Identity-audience delegation, not an arbitrary self-reported actor.
  // Core verifies it using DelegationCryptoPort, then checks the original source
  // session/credential.
  SecretView delegation;
};
struct RevokeSessionRequest {
  IdentityRequestContext context;
  IdentityActorProof actor;
  SessionId session;
};
struct SessionPageRequest {
  std::uint32_t page_size = 0; // 0 ->20; maximum100. Output-only wire fields are not accepted here.
  std::string page_token;
  std::string snapshot_id;
};
struct ListSessionsRequest {
  IdentityRequestContext context;
  IdentityActorProof actor;
  SubjectId subject;
  SessionPageRequest page;
};
struct IdentityTokenReply {
  EphemeralTokenPair tokens;
  TimePoint checked_at; // Authoritative DB UTC, never Control's wall clock or JWT iat.
};
struct ValidatedSession {
  Principal principal;
  SessionMetadata session;
  std::vector<ExactAssignment> assignments;
  ResourceVersion authorization_version;
  TimePoint checked_at;
};
struct CurrentPrincipal {
  Principal principal;
  std::vector<ExactAssignment> assignments;
  ResourceVersion authorization_version;
  TimePoint checked_at;
};
struct RevokeSessionReply {
  SessionMetadata session;
  TimePoint checked_at;
};
struct SessionPageReply {
  std::vector<SessionMetadata> sessions;
  std::uint32_t page_size;
  std::string snapshot_id;
  std::string next_page_token;
  TimePoint expires_at;
  ResourceVersion authorization_version;
  TimePoint checked_at;
};
struct IdentitySecurityOptions {
  // Trusted composition/configuration only. Empty keys/zero capacities/invalid lifetime reject
  // construction, never substitute unlimited budgets, a weak random source or fake readiness.
  CryptoKeyRef token_key;
  std::vector<CryptoKeyRef> token_verification_keys;
  CryptoKeyRef request_commitment_key;
  CryptoKeyRef receipt_encryption_key;
  PasswordVerifier dummy_password_verifier;
  std::chrono::nanoseconds password_credential_lifetime{};
  SecretReceiptCapacity secret_receipts{0};
  std::size_t maximum_sessions_per_subject = 0;
  std::size_t maximum_snapshots = 0;
  std::size_t maximum_snapshot_records = 0;
};
struct IdentitySecurityDependencies {
  // Borrowed for the entire application lifetime; factory never constructs a second transaction
  // to retrofit missing repos. Expensive crypto/key access is outside ALL database UoWs.
  IdentitySecurityUnitOfWorkFactory& transactions;
  PasswordCryptoPort& passwords;
  TokenCryptoPort& tokens;
  SecretReceiptCryptoPort& secret_receipts;
  RandomPort& random;
  SecurityClockPort& clock;
  DelegationCryptoPort& delegations;
};
// Public application boundary for RPC/Control adapters. No protobuf/HTTP/JWT/SQL objects enter.
// Methods may run concurrently; implementations isolate their UoWs and protect bounded snapshots.
// All methods enforce peer identity, current origin state, deadline and stable failure semantics.
// Expected errors use Outcome, and unexpected exceptions cannot escape this boundary.
class IdentitySecurityApplication : public ServiceReadinessPort {
public:
  ~IdentitySecurityApplication() override = default;
  Service service() const noexcept override final {
    return Service::Identity;
  }
  // Reads/crypto preparation -> release UoW -> crypto -> short conditional final UoW -> commit.
  // Successful writes respond ONLY after Committed; Unknown -> original-key recovery, no retry
  // with a new key or response containing uncommitted secrets. Fresh state precedes receipt replay.
  virtual Outcome<IdentityTokenReply> login(const LoginRequest& request) = 0;
  virtual Outcome<IdentityTokenReply> refresh_session(const RefreshSessionRequest& request) = 0;
  // Always read current authoritative Identity state; no success from a cached proof/old receipt.
  virtual Outcome<ValidatedSession> validate_session(const AccessSessionRequest& request) = 0;
  virtual Outcome<CurrentPrincipal> get_principal(const AccessSessionRequest& request) = 0;
  virtual Outcome<RevokeSessionReply> revoke_session(const RevokeSessionRequest& request) = 0;
  // Current authorization on every page; immutable <=10min snapshot, size/scope/auth-version bound,
  // restart invalidation, no fake empty page on DB/authorization/capacity failure.
  virtual Outcome<SessionPageReply> list_sessions(const ListSessionsRequest& request) = 0;
};
// Additive constructor interface; does not replace P02 make_identity_application or enable ready.
// Implementation follows in its own capability, using the SAME injected ports as production.
Outcome<std::unique_ptr<IdentitySecurityApplication>>
make_identity_security_application(IdentitySecurityDependencies dependencies,
                                   IdentitySecurityOptions options);

} // namespace kiarabot::core::target
