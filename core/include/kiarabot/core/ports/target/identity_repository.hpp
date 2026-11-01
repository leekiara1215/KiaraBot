#pragma once

#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/identity_assignment_state.hpp"
#include "kiarabot/core/domain/target/identity_types.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// Every repository below borrows ONE Identity transaction. Empty optional is absence, not failure.
class IdentityCredentialRepository {
public:
  virtual ~IdentityCredentialRepository() = default;
  virtual Outcome<std::optional<CredentialVersionRecord>> current(const CredentialId& id) = 0;
  virtual Outcome<std::optional<CredentialVersionRecord>>
  version(const CredentialVersionRef& ref) = 0;
  // Real TLS leaf SHA-256 lookup: unique leaf fingerprint, CURRENT head only, never scan all rows
  // or obtain credential_id/subject from request body. Caller checks actual subject/service/SAN,
  // current version/Active/expiry and principal gate; this index is not authentication success.
  virtual Outcome<std::optional<CredentialVersionRecord>>
  by_certificate(const Sha256Digest& leaf_fingerprint) = 0;
  // CURRENT bootstrap material only, located by a trusted keyed verifier, never body identity.
  // Accept only WorkloadBootstrap purpose, nonempty valid key and the fixed32 tag. Raw proof and
  // HMAC/key-provider work stay outside the UoW. save owns every current locator atomically.
  // No match -> empty; bad input -> InvalidArgument; ambiguous/corrupt stored state -> Failure.
  // Caller still checks current principal/state/expiry/binding/audiences; lookup is not a grant.
  virtual Outcome<std::optional<CredentialVersionRecord>>
  by_bootstrap(const TokenVerifier& verifier) = 0;

  // Trusted signed actor lookup, NOT TLS authentication. Match platform service + subject on
  // CURRENT certificate heads only, in this UoW. Zero -> empty; multiple -> Internal, never pick
  // one or scan all principals in memory. Core checks current head/owner/state/expiry/SAN/service.
  virtual Outcome<std::optional<CredentialVersionRecord>> by_platform(const PlatformPeer& peer) = 0;
  virtual Outcome<std::optional<LoginIdentity>> login(const std::string& normalized_login) = 0;
  virtual Status save_login(const LoginIdentity& login,
                            std::optional<ResourceVersion> expected) = 0;
  // Append version + advance current head atomically. Revoke also advances head; old material is
  // historical evidence, NEVER valid for authentication after current head changes/revokes.
  virtual Status save(const CredentialVersionRecord& record,
                      std::optional<ResourceVersion> expected) = 0;
};
class IdentitySessionSecurityRepository {
public:
  virtual ~IdentitySessionSecurityRepository() = default;
  virtual Outcome<std::optional<SessionSecurity>> find(const SessionId& id) = 0;
  virtual Outcome<std::optional<SessionSecurity>> by_access(const TokenVerifier& verifier) = 0;
  virtual Outcome<std::optional<SessionSecurity>> by_refresh(const TokenVerifier& verifier) = 0;
  // Current by_refresh alone is insufficient after a successful rotation. Resolve either current
  // verifier or an input consumed by this EXACT control-peer/idempotency key, within recovery_until
  // and original input expiry (max24h, never extended by lookup). Another key never resolves
  // history. Missing/expired -> empty optional; this is only a handle, NOT a validated
  // session/token grant.
  virtual Outcome<std::optional<RefreshHandle>> resolve_refresh(const TokenVerifier& input,
                                                                const RefreshRecoveryLookup& lookup,
                                                                TimePoint now) = 0;
  // Persist old verifier -> original scope/input rotation in the SAME transaction as rotation,
  // metadata and encrypted success receipt. Unique input verifier; never store raw refresh token.
  virtual Status record_refresh_consumption(const ConsumedRefreshHandle& consumed) = 0;
  // One consistent bounded metadata read for the application's immutable snapshot. max_records
  // is trusted TOTAL snapshot capacity, not client page size. Overflow -> RateLimited, never
  // truncate/fake an empty page. Dependency failure -> Failure. Query returns no verifier material.
  // Application checks CURRENT authorization on every page, binds owner/scope/cursor to snapshot,
  // caps retention at 10min, and invalidates snapshots on restart; no live offset pagination.
  virtual Outcome<std::vector<SessionMetadata>> list_metadata(const SubjectId& subject,
                                                              std::size_t max_records) = 0;
  // Authority for the existing global 4096-session ceiling: count all non-revoked sessions with
  // refresh_expires_at strictly after actual DB UTC, in THIS UoW. No caller-provided now, cached
  // count, unauthorized list or assumed zero on dependency failure. Application checks this only
  // for a new session, after original-receipt replay, and inserts in the same short transaction.
  virtual Outcome<std::size_t> count_unexpired() = 0;
  // Null expected creates; update matches stored version AND rotation, rejects overflow.
  // created_at is immutable; changing it on update -> Conflict, not a regenerated query timestamp.
  // New sessions MUST contain a valid original control_peer. Refresh/revoke preserve it exactly;
  // missing create binding or changed update binding rejects, never a wildcard/default caller.
  // save atomically creates/updates BOTH the security row and inherited sessions() metadata,
  // with the SAME transaction, version, rotation and revoked flag; base expires_at equals
  // refresh_expires_at. Caller MUST NOT separately save sessions() for this mutation: a second
  // create/CAS duplicates the repository-owned write and conflicts. Failure leaves neither row
  // partially written; unknown commit still uses the enclosing UoW recovery contract.

  virtual Status save(const SessionSecurity& record, std::optional<ResourceVersion> expected,
                      std::optional<std::uint64_t> expected_rotation) = 0;
};
class IdentityExactAssignmentRepository {
public:
  virtual ~IdentityExactAssignmentRepository() = default;
  // Typed view of the SAME authoritative grant rows/versions, not a second grant database.
  // Return Active same-subject exact tuples only, never revoked evidence or flattened scope.
  // Inherited grants() metadata also filters revoked rows and cannot bypass this authority.
  virtual Outcome<std::vector<ExactAssignment>> for_subject(const SubjectId& subject) = 0;
  // Current administrative evidence includes Revoked IDs, preserving their permanent owner,
  // final version and former exact scope. Empty optional is absence; faults remain Failure.
  virtual Outcome<std::optional<AssignmentStateRecord>> current(const AssignmentId& id) = 0;
  // Null expected creates Active version1 only. Updates match expected and advance exactly one
  // version with the SAME owner. Active->Revoked advances version and records finite DB UTC
  // revoked_at atomically; Revoked IDs never restore, reuse or erase owner/scope evidence.
  // This write atomically synchronizes inherited grant metadata in the SAME physical UoW.
  // Legacy base-grant writes cannot revive or replace a revoked ID.
  virtual Status save_state(const AssignmentStateRecord& record,
                            std::optional<ResourceVersion> expected) = 0;
  // Existing compatibility entry: exactly save_state(Active, no revoked_at), with identical
  // create/update owner/version CAS and atomic base synchronization. Never revives Revoked.
  virtual Status save(const ExactAssignment& assignment,
                      std::optional<ResourceVersion> expected) = 0;
};
class IdentitySecretReceiptRepository {
public:
  virtual ~IdentitySecretReceiptRepository() = default;
  virtual Outcome<std::optional<EncryptedSecretReceipt>> find(const SecretReceiptScope& scope) = 0;
  // Unique scope, keyed request commitment: same request returns ORIGINAL encrypted result,
  // changed request Conflict. No valid-receipt eviction at capacity. Retention <=24h, NEVER 7d.
  // Authentication/current session/credential/rotation checks precede decryption, including replay.
  virtual Outcome<SecretReceiptWrite> insert(const EncryptedSecretReceipt& receipt,
                                             SecretReceiptCapacity capacity) = 0;
  virtual Outcome<std::size_t> prune_expired(TimePoint now, std::size_t limit) = 0;
};
class IdentityBootstrapRepository {
public:
  virtual ~IdentityBootstrapRepository() = default;
  // A missing marker in an initialized schema is Internal, never implicitly Required.
  virtual Outcome<AdministratorBootstrap> state() = 0;
  // Conditional Required->Consumed, same transaction as initial administrator/credential/exact
  // grants/authorization version/audit. Offline trusted command only; restart never reopens it.
  virtual Status consume(const AdministratorBootstrap& consumed, ResourceVersion expected) = 0;
};
// Additive extension: P03 IdentityUnitOfWork pure-virtual set/ABI is unchanged. Derived repos and
// ALL inherited repos must use ONE physical transaction, never a second connection/UoW or cast.
class IdentitySecurityUnitOfWork : public IdentityUnitOfWork {
public:
  Service owner() const noexcept override final {
    return Service::Identity;
  }
  virtual IdentityCredentialRepository& credentials() = 0;
  virtual IdentitySessionSecurityRepository& session_security() = 0;
  virtual IdentityExactAssignmentRepository& exact_assignments() = 0;
  virtual IdentitySecretReceiptRepository& secret_receipts() = 0;
  virtual IdentityBootstrapRepository& bootstrap() = 0;
};
using IdentitySecurityUnitOfWorkFactory = LocalUnitOfWorkFactory<IdentitySecurityUnitOfWork>;

} // namespace kiarabot::core::target
