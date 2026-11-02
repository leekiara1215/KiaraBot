#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "kiarabot/core/domain/target/persistence.hpp"

namespace kiarabot::core::target {

class EventRepository;
class ActionRepository;
class ActionQuotaRepository;
class ActionPolicyRepository;
class ActionEvidenceRepository;

// Repositories borrow their UoW lifetime, never commit and use its authoritative database UTC.
// Empty optional means absent; faults are Failure (Unavailable/Conflict/Internal), never absence.
// Expected-version/unique-key failures: Conflict; capacity: RateLimited, not accepted.
// All capacity arguments come from trusted application configuration, NEVER request-controlled
// limits. All now arguments are application samples from UoW::current_time(); implementations
// still adjudicate expiry/claim/retention against actual DB UTC in the same conditional write.
template <typename Key, typename Value> class Repository {
public:
  virtual ~Repository() = default;
  virtual Outcome<std::optional<Value>> find(const Key& key) = 0;
  // null expected = create only. Updates match current version and store the next owner version.
  virtual Status save(const Value& value, std::optional<ResourceVersion> expected) = 0;
};
class ReceiptRepository {
public:
  virtual ~ReceiptRepository() = default;
  virtual Outcome<std::optional<CommandReceipt>> find(const ReceiptKey& key) = 0;
  // Insert UNIQUE subject/method/resource kind+id/key. Same fingerprint returns ORIGINAL snapshot,
  // no second charge/audit/retention extension; different fingerprint Conflict. Authenticate
  // replay. Ordinary success retention >=7d. Reject new receipts at capacity, never evict valid
  // records.
  virtual Outcome<ReceiptWrite> insert(const CommandReceipt& receipt, ReceiptCapacity capacity) = 0;
  virtual Outcome<std::size_t> prune_expired(TimePoint now, std::size_t limit) = 0;
};
class JournalRepository {
public:
  virtual ~JournalRepository() = default;
  virtual Status append_audit(const AuditEntry& entry) = 0;
  // source+message unique. Identical immutable envelope/hash -> no-op, changed content -> Conflict.
  // Reject expired new messages; capacity never drops already accepted obligations.
  virtual Status enqueue(const OutboxEntry& entry, JournalCapacity capacity, TimePoint now) = 0;
  // source is this UoW owner. 1..100 records; 0<lease<=30s. Secure unique tokens are persisted in
  // this transaction. Only due/current claims; attempts>=100 -> NeedsAttention, never auto-delete.
  virtual Outcome<std::vector<OutboxClaim>> claim(Service source, std::size_t limit, TimePoint now,
                                                  std::chrono::seconds lease) = 0;
  // Match full key, active token and now<lease_until. Duplicate same-token confirmation is no-op;
  // stale/expired/unrelated claims Conflict. Network acknowledgement happens outside the UoW.
  virtual Status confirm(const HandoffKey& key, const ClaimToken& claim_token, TimePoint now) = 0;
  virtual Outcome<std::optional<InboxRecord>> received(const HandoffKey& key) = 0;
  // UNIQUE source+message plus hash/envelope equality; same content returns ORIGINAL result.
  // Received timestamp is DB UTC; retain >=max(first receive+7d, valid_until+7d).
  // Expired new messages reject even after dedup cleanup; duplicates never extend retention.
  virtual Outcome<InboxWrite> record_received(const InboxRecord& receipt, JournalCapacity capacity,
                                              TimePoint now) = 0;
  virtual Outcome<std::size_t> prune_received(TimePoint now, std::size_t limit) = 0;
};
enum class CommitResult { Committed, RolledBack, Unknown };
class UnitOfWork {
public:
  virtual ~UnitOfWork() = default;
  virtual Service owner() const noexcept = 0;
  // Sample authoritative database UTC at this call, NOT transaction-start CURRENT_TIMESTAMP
  // frozen by the database, and never system_clock substituted on failure. Explicit dependency
  // failure uses Outcome. This is a sample for application timestamps, not a lease authority:
  // temporal writes recheck actual database UTC atomically with their predicates. Short local
  // transactions have a 2s budget (lock wait 250ms); do not reuse samples across UoWs/network I/O.
  virtual Outcome<TimePoint> current_time() = 0;
  virtual ReceiptRepository& receipts() = 0;
  virtual JournalRepository& journal() = 0;
  virtual Repository<OperationId, Operation>& operations() = 0;
  // Terminal, releases resources. Unknown MUST NOT be treated as RolledBack or automatically
  // retried: query original receipt/handoff identity in a fresh UoW. No external calls in a UoW.
  virtual CommitResult commit() = 0;
  // Only active uncommitted transactions; cannot undo Committed or Unknown.
  virtual void rollback() noexcept = 0;
  // Destruction rolls back only active/uncommitted work; single-owner, not thread safe.
};
class IdentityUnitOfWork : public UnitOfWork {
public:
  virtual Repository<SubjectId, Principal>& principals() = 0;
  virtual Repository<SessionId, LoginSession>& sessions() = 0;
  virtual Repository<AssignmentId, ResourceGrant>& grants() = 0;
  virtual Repository<SubjectId, AuthorizationState>& authorizations() = 0;
};
// One-time P06 external-bridge bootstrap credentials. Runtime persists only the SHA-256 digest of
// the opaque credential; plaintext never appears in storage, receipts or audit. Activation is
// per-resource (adapter instance or externally-managed plugin instance).
struct BridgeCredentialRecord {
  std::string id;
  std::string resource_id;
  Sha256Digest digest;
  TimePoint created_at;
  bool revoked = false;
};
class BridgeCredentialRepository {
public:
  virtual ~BridgeCredentialRepository() = default;
  // True when a non-revoked credential already exists for the resource; callers then do not issue a
  // second plaintext (it would be unrecoverable anyway).
  virtual Outcome<bool> active(const std::string& resource_id) = 0;
  // Resolve an active bootstrap credential from its SHA-256 digest for the external bridge
  // handshake. The plaintext token never reaches storage; an absent/revoked digest is an empty
  // optional, a storage fault is a Failure.
  virtual Outcome<std::optional<BridgeCredentialRecord>> by_digest(const Sha256Digest& digest) = 0;
  virtual Status insert(const BridgeCredentialRecord& record) = 0;
  virtual Status revoke(const std::string& resource_id, TimePoint now) = 0;
};
// Adapter instances are looked up by their stable adapter resource id or by the assigned instance
// identity that the wire registration/heartbeat frames carry. Both reads belong to the same owner
// UoW; the registry never decides authorization.
class AdapterRegistryRepository {
public:
  virtual ~AdapterRegistryRepository() = default;
  virtual Outcome<std::optional<AdapterRecord>> find(const ResourceId& adapter) = 0;
  virtual Outcome<std::optional<AdapterRecord>> by_instance(const InstanceId& instance) = 0;
  virtual Status save(const AdapterRecord& value, std::optional<ResourceVersion> expected) = 0;
};
class RuntimeUnitOfWork : public UnitOfWork {
public:
  virtual Repository<ResourceId, BotAccount>& accounts() = 0;
  // Keyed by the bound account; one account is served by at most one adapter instance and an
  // adapter serves many accounts.
  virtual Repository<ResourceId, AdapterBindingRecord>& adapter_bindings() = 0;
  virtual AdapterRegistryRepository& adapters() = 0;
  virtual BridgeCredentialRepository& bridge_credentials() = 0;
  // Count active account bindings that still reference this adapter instance; used to revoke the
  // adapter bootstrap credential only when the last binding is removed.
  virtual Outcome<std::size_t> active_bindings(const InstanceId& adapter_instance) = 0;
  virtual Repository<ResourceId, HostRegistration>& hosts() = 0;
  virtual Repository<InstallationId, PluginInstallation>& installations() = 0;
  virtual Repository<InstanceId, PluginInstance>& instances() = 0;
  virtual Repository<InstanceId, InstanceConfiguration>& instance_configs() = 0;
  // Bounded owner-local projections for typed List queries, ordered by the stable id and capped by
  // a trusted application limit (1..100). The application filters current authorization; the
  // repository never decides visibility. Empty page is a successful empty result, faults are
  // Failure, never a fake empty page.
  // Keyset read: ascending stable id strictly greater than `after` (empty = from the beginning),
  // bounded by a trusted application limit. This is a current-authority read, not a frozen page
  // snapshot; the application owns opaque cursor tokens and per-item authorization filtering.
  virtual Outcome<std::vector<BotAccount>> list_accounts(std::size_t limit,
                                                         const std::string& after) = 0;
  virtual Outcome<std::vector<AdapterRecord>> list_adapters(std::size_t limit,
                                                            const std::string& after) = 0;
  virtual Outcome<std::vector<HostRegistration>> list_hosts(std::size_t limit,
                                                            const std::string& after) = 0;
  virtual Outcome<std::vector<PluginInstallation>> list_installations(std::size_t limit,
                                                                      const std::string& after) = 0;
  virtual Outcome<std::vector<PluginInstance>> list_instances(std::size_t limit,
                                                              const std::string& after) = 0;
  // Event inbox, watermark and per-instance delivery ledger share this UoW so acceptance,
  // claim, ACK and retry state commit atomically (P06). Injected by the PG adapter.
  virtual EventRepository& events() = 0;
  // P07 authoritative Action records and their quota/policy/evidence relations. All four
  // participate in this same Runtime UoW; dispatch frames are written after commit, never here.
  virtual ActionRepository& actions() = 0;
  virtual ActionQuotaRepository& action_quota() = 0;
  virtual ActionPolicyRepository& action_policies() = 0;
  virtual ActionEvidenceRepository& action_evidence() = 0;
};
class SchedulerUnitOfWork : public UnitOfWork {
public:
  virtual Repository<TaskKey, ScheduledTask>& tasks() = 0;
  virtual Repository<MessageId, Occurrence>& occurrences() = 0;
};
class ModelUnitOfWork : public UnitOfWork {
public:
  virtual Repository<ModelCallKey, ModelIntent>& model_intents() = 0;
};
template <typename Work> class LocalUnitOfWorkFactory {
public:
  virtual ~LocalUnitOfWorkFactory() = default;
  virtual Outcome<std::unique_ptr<Work>> begin() = 0;
};
using IdentityUnitOfWorkFactory = LocalUnitOfWorkFactory<IdentityUnitOfWork>;
using RuntimeUnitOfWorkFactory = LocalUnitOfWorkFactory<RuntimeUnitOfWork>;
using SchedulerUnitOfWorkFactory = LocalUnitOfWorkFactory<SchedulerUnitOfWork>;
using ModelUnitOfWorkFactory = LocalUnitOfWorkFactory<ModelUnitOfWork>;

} // namespace kiarabot::core::target
