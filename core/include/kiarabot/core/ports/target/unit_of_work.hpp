#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "kiarabot/core/domain/target/invariants.hpp"
#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/core/ports/target/host_command_ports.hpp"
#include "kiarabot/core/ports/target/host_registration_ports.hpp"
#include "kiarabot/core/ports/target/namespace_ports.hpp"

namespace kiarabot::core::target {
namespace host_ports_detail {
class UnsupportedHostRegistration final : public HostRegistrationPort {
public:
  Outcome<std::optional<HostCertificateBinding>> certificate_binding(std::string_view) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<std::optional<HostRegistration>> find_host(const ResourceId&) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Status save_host(const HostRegistration&, std::optional<ResourceVersion>) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<std::optional<HostConnectionState>> connection(const ResourceId&) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<HostConnectionState> activate_connection(const HostConnectionState&) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Status save_connection(const HostConnectionState&, std::uint64_t) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
};
inline HostRegistrationPort& unsupported_host_registration() {
  static UnsupportedHostRegistration instance;
  return instance;
}
class UnsupportedHostCommandOutbox final : public HostCommandOutbox {
public:
  Outcome<PersistedHostCommand> insert(const HostCommandOutboxRecord&) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<std::vector<HostCommandOutboxRecord>> pending(const std::string&, std::uint64_t,
                                                        std::size_t) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<std::optional<HostCommandOutboxRecord>> find(const std::string&) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Status mark_dispatched(const std::string&, std::uint64_t, std::string_view, TimePoint) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Status record_receipt(const std::string&, bool, std::optional<ExecutionOutcome>,
                        std::optional<std::string>, TimePoint) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Status mark_unknown(const std::string&, TimePoint) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<bool> record_observation(const HostObservationRecord&) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
  Outcome<std::vector<HostObservationRecord>> observations(const std::string&, std::uint64_t,
                                                           std::size_t) override {
    return rejected(ErrorCode::UpstreamUnavailable);
  }
};
inline HostCommandOutbox& unsupported_host_commands() {
  static UnsupportedHostCommandOutbox instance;
  return instance;
}
} // namespace host_ports_detail

class EventRepository;
class ActionRepository;
class ActionQuotaRepository;
class ActionPolicyRepository;
class ActionEvidenceRepository;
class HostRegistrationPort;

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
// P13 owner-local operations that are not part of the P01-P12 owner contract surface. Defaults
// fail closed so every partial adapter/UoW test double keeps compiling and can never silently
// report an empty participant list, empty tombstone scan, or a fake cleanup success.
inline Failure unsupported_lifecycle_read() {
  return {ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
          std::nullopt, std::nullopt};
}
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
  // P13a owner-local participant projection for an operation row. The frozen wire Operation
  // already carries participants; storage keeps them in an owner-private relation so the generic
  // Operation repository row stays unchanged.
  virtual Outcome<std::vector<OperationParticipant>> operation_participants(const OperationId&) {
    return unsupported_lifecycle_read();
  }
  virtual Status save_operation_participants(const OperationId&,
                                             const std::vector<OperationParticipant>&) {
    return unsupported_lifecycle_read();
  }
  // P13a bounded retention passes. Each implementation adjudicates retention against actual
  // database UTC in the same conditional write; the application-owned 60s cadence is advisory.
  virtual Outcome<std::size_t> prune_outbox(TimePoint, std::size_t) {
    return unsupported_lifecycle_read();
  }
  virtual Outcome<std::size_t> prune_audit(TimePoint, std::size_t) {
    return unsupported_lifecycle_read();
  }
  virtual Outcome<std::size_t> prune_operations(TimePoint, std::size_t) {
    return unsupported_lifecycle_read();
  }
  // P13a tombstone registry for reversible deletes. A missing tombstone is absence, a storage
  // fault is Failure; the list is a bounded owner-local scan ordered by resource id.
  virtual Outcome<std::optional<LifecycleTombstone>> find_lifecycle_tombstone(const ResourceRef&) {
    return unsupported_lifecycle_read();
  }
  virtual Status save_lifecycle_tombstone(const LifecycleTombstone&) {
    return unsupported_lifecycle_read();
  }
  virtual Status delete_lifecycle_tombstone(const ResourceRef&) {
    return unsupported_lifecycle_read();
  }
  virtual Outcome<std::vector<LifecycleTombstone>> list_lifecycle_tombstones(std::size_t,
                                                                             const std::string&) {
    return unsupported_lifecycle_read();
  }
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
  // P08 Runtime-side Host registration/heartbeat port. Defaults fail closed so an adapter that
  // has not implemented P08b never reports a fake successful registration.
  virtual HostRegistrationPort& host_registration() {
    return host_ports_detail::unsupported_host_registration();
  }
  // P08b-3 durable Host command/observation outbox. Defaults fail closed so a partial adapter
  // never reports a fake successful command enqueue/delivery.
  virtual HostCommandOutbox& host_commands() {
    return host_ports_detail::unsupported_host_commands();
  }
  // P09 storage/shared-namespace groups. Phase-1 defaults fail closed so an adapter that has not
  // implemented P09 never reports a fake empty namespace; P09 phase 2 overrides them with PG
  // implementations after P07a-A lands. They borrow this same UoW, never commit.
  virtual NamespaceRepository& namespaces() {
    return namespace_ports_detail::unsupported_namespaces();
  }
  virtual SharedNamespaceRepository& shared_namespaces() {
    return namespace_ports_detail::unsupported_shared_namespaces();
  }
  virtual NamespaceQuotaRepository& namespace_quota() {
    return namespace_ports_detail::unsupported_namespace_quota();
  }
  virtual StorageReceiptRepository& storage_receipts() {
    return namespace_ports_detail::unsupported_storage_receipts();
  }
};
class SchedulerUnitOfWork : public UnitOfWork {
public:
  virtual Repository<TaskKey, ScheduledTask>& tasks() = 0;
  virtual Repository<MessageId, Occurrence>& occurrences() = 0;
  // Owner-scoped projections for typed queries and maintenance. Adapters implement these with
  // owner/due indexes; the default fails closed so a partial adapter cannot silently list all rows.
  virtual Outcome<std::vector<ScheduledTask>> list_tasks(const InstanceId& owner, std::size_t limit,
                                                         const std::string& after) {
    (void)owner;
    (void)limit;
    (void)after;
    return Failure{ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
                   std::nullopt, std::nullopt};
  }
  // Non-terminal task counts per instance and per installation for the frozen 1024/8192 limits.
  virtual Outcome<std::pair<std::size_t, std::size_t>>
  count_tasks(const InstanceId& owner, const InstallationId& installation) {
    (void)owner;
    (void)installation;
    return Failure{ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
                   std::nullopt, std::nullopt};
  }
  virtual Outcome<std::vector<ScheduledTask>> due_tasks(TimePoint run_at, std::size_t limit) {
    (void)run_at;
    (void)limit;
    return Failure{ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
                   std::nullopt, std::nullopt};
  }
  virtual Outcome<std::vector<Occurrence>>
  list_occurrences(const InstanceId& owner, std::size_t limit, const std::string& after) {
    (void)owner;
    (void)limit;
    (void)after;
    return Failure{ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
                   std::nullopt, std::nullopt};
  }
  // Accepted occurrences whose durable handoff has not been confirmed or expired yet.
  virtual Outcome<std::vector<Occurrence>> inflight_occurrences(std::size_t limit) {
    (void)limit;
    return Failure{ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
                   std::nullopt, std::nullopt};
  }
};
// P11 Model-private configuration. The configuration version is the CAS/watermark; the provider
// secret version is a separate relation and rotates without changing this row's version.
class ModelConfigurationRepository {
public:
  virtual ~ModelConfigurationRepository() = default;
  virtual Outcome<std::optional<ModelConfiguration>> find(const ResourceId& model_id) = 0;
  // Alias lookup participates in global uniqueness; an ambiguous/invalid stored alias is Internal.
  virtual Outcome<std::optional<ModelConfiguration>> find_by_alias(const std::string& alias) = 0;
  // Keyset read ordered by model_id, strictly greater than `after`; trusted limit only.
  virtual Outcome<std::vector<ModelConfiguration>> list(std::size_t limit,
                                                        const std::string& after) = 0;
  // null expected = create only; otherwise match version and store version+1.
  virtual Status save(const ModelConfiguration& value, std::optional<ResourceVersion> expected) = 0;
};

// Sealed provider credential versions. Old versions are retained for in-flight calls; plaintext is
// never stored, returned by configuration queries or written to audit/logs.
class ProviderSecretRepository {
public:
  virtual ~ProviderSecretRepository() = default;
  virtual Outcome<std::optional<ProviderSecretRecord>> current(const ResourceId& model_id) = 0;
  virtual Outcome<std::optional<ProviderSecretRecord>> find(const ResourceId& model_id,
                                                            const ResourceVersion& version) = 0;
  // UNIQUE(model_id, secret_version); a concurrent rotation of the same version is Conflict.
  virtual Status insert(const ProviderSecretRecord& value) = 0;
};

// Durable Model call log. The (owner, logical_call_id) key plus request_hash is the invocation
// dedupe authority: a stored row means the provider has already been (or may have been) invoked.
class ModelCallRepository {
public:
  virtual ~ModelCallRepository() = default;
  virtual Outcome<std::optional<ModelIntent>> find(const ModelCallKey& key) = 0;
  // Control-side lookup by logical_call_id only. Multiple owners with the same call id is
  // ambiguous -> Conflict (never an arbitrary row).
  virtual Outcome<std::optional<ModelIntent>>
  find_by_call_id(const std::string& logical_call_id) = 0;
  // Bounded maintenance projection of unresolved UNKNOWN rows whose 30-day window elapsed.
  virtual Outcome<std::vector<ModelIntent>> list_unresolved_unknown(TimePoint now,
                                                                    std::size_t limit) = 0;
  virtual Status insert(const ModelIntent& value) = 0;
  // Current-version CAS; the stored owner version advances by one.
  virtual Status save(const ModelIntent& value, std::optional<ResourceVersion> expected) = 0;
  // Delete terminal rows older than their retention horizon; unresolved UNKNOWN is never pruned.
  virtual Outcome<std::size_t> prune_terminal(TimePoint now, std::size_t limit) = 0;
};

class ModelUnitOfWork : public UnitOfWork {
public:
  // P03 declaration-level intent relation kept source-compatible for the existing typed checks.
  virtual Repository<ModelCallKey, ModelIntent>& model_intents() = 0;
  virtual ModelConfigurationRepository& model_configs() = 0;
  virtual ProviderSecretRepository& provider_secrets() = 0;
  virtual ModelCallRepository& model_calls() = 0;
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
