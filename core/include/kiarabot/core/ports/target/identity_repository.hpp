#pragma once

#include <optional>
#include <string>
#include <vector>

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
  // Null expected creates; update matches stored version AND rotation, rejects overflow.
  // created_at is immutable; changing it on update -> Conflict, not a regenerated query timestamp.
  // Base sessions() metadata shares this transaction/version; expires_at = refresh_expires_at.
  virtual Status save(const SessionSecurity& record, std::optional<ResourceVersion> expected,
                      std::optional<std::uint64_t> expected_rotation) = 0;
};
class IdentityExactAssignmentRepository {
public:
  virtual ~IdentityExactAssignmentRepository() = default;
  // Typed view of the same Identity grant authority/versions, not a second grant database.
  // P04 authorization reads this exact projection; inherited grants() is metadata, not a bypass.
  // Return same-subject exact tuples, not independently flattened role/scope projections.
  virtual Outcome<std::vector<ExactAssignment>> for_subject(const SubjectId& subject) = 0;
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
