#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/domain/target/runtime_records.hpp"
#include "kiarabot/core/domain/target/runtime_rules.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/runtime_secrets.hpp"
#include "kiarabot/core/ports/target/service_readiness.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

inline constexpr std::uint32_t kDefaultPageSize = 20;
inline constexpr std::uint32_t kMaximumPageSize = 100;
inline constexpr auto kSnapshotLifetime = std::chrono::minutes{10};

struct PageRequest {
  std::uint32_t page_size = kDefaultPageSize;
  std::string page_token;
};
struct PageInfo {
  std::uint32_t page_size = 0;
  std::string next_page_token;
};
template <typename Value> struct ListResult {
  std::vector<Value> items;
  PageInfo page;
  SnapshotWindow snapshot;
};
struct OperationAccepted {
  OperationRef operation;
};
struct AccountMutation {
  BotAccount account;
  std::optional<OperationAccepted> operation;
};
struct BindingMutation {
  AdapterBindingRecord binding;
  std::optional<OperationAccepted> operation;
  // One-time P06 external-bridge bootstrap credential for the adapter instance. Plaintext exists
  // only in this response; Runtime stores only its SHA-256 digest. Empty when no new credential was
  // issued (already active) or for managed plugin instances.
  std::optional<std::string> bootstrap_credential;
};
struct AdapterRegistrationResult {
  AdapterRecord adapter;
  std::vector<Capability> effective_capabilities;
  AdapterFence fence;
  std::uint32_t heartbeat_interval_seconds = 30;
};
struct HeartbeatResult {
  AdapterFence fence;
  TimePoint received_at;
};
struct InstanceMutation {
  PluginInstance instance;
  std::optional<OperationAccepted> operation;
};
struct InstanceCreation {
  PluginInstance instance;
  std::optional<std::string> bootstrap_credential;
};
struct ConfigMutation {
  InstanceConfiguration config;
  std::optional<OperationAccepted> operation;
};
struct ManifestValidation {
  bool valid = false;
  std::vector<Failure> violations;
};
// Neutral typed resource state projection; the boundary renders the wire VersionedResourceState.
struct ResourceState {
  ResourceRef resource;
  ResourceVersion version;
  LifecycleState lifecycle = LifecycleState::Active;
  DesiredState desired = DesiredState::Running;
  std::optional<Fence> fence;
  // Adapter resources carry a connection-only fence (no trusted process execution in P05).
  std::optional<AdapterFence> adapter_fence;
  bool registered = false;
  ResourceVersion desired_config_version;
  ResourceVersion applied_config_version;
  ExecutionOutcome process_outcome = ExecutionOutcome::NotStarted;
  TimePoint observed_at;
  Service source_service = Service::Runtime;
};

struct CreateAccountRequest {
  CommandContext context;
  std::string platform;
  std::string platform_account_id;
  std::string display_name;
};
struct UpdateAccountRequest {
  CommandContext context;
  ResourceId account;
  std::string display_name;
};
struct DisableAccountRequest {
  CommandContext context;
  ResourceId account;
};
struct AccountQuery {
  CommandContext context;
  ResourceId account;
};
struct AccountListQuery {
  CommandContext context;
  PageRequest page;
};
struct BindAdapterRequest {
  CommandContext context;
  ResourceId account;
  InstanceId adapter_instance;
};
struct UnbindAdapterRequest {
  CommandContext context;
  ResourceId account;
};
struct AdapterRegistrationCommand {
  // Direct adapter peer authentication is boundary-owned; the command carries the trusted result.
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  ResourceId adapter;
  AdapterRegistrationMetadata metadata;
  std::optional<InstanceId> presented_instance;
  std::vector<ResourceId> declared_accounts;
  // P06 external bridge registration: the bootstrap resource identity assigned by Control through
  // BindAdapter. When set, the first registration creates the AdapterRecord for exactly this
  // instance instead of allocating a fresh identity. Direct (non-bridge) callers leave it unset.
  std::optional<InstanceId> assigned_instance;
  // The connection was authenticated by the Runtime-local bootstrap/ConnectionAuthorization gate
  // rather than an Identity delegation. AdapterConnect is already proven by the bootstrap
  // handshake, so this path skips the online Identity AdapterConnect check; every later business
  // operation still uses the normal online authorization path.
  bool bootstrap_authenticated = false;
  // The Runtime process epoch sampled once by the bridge composition. Direct callers leave it
  // unset and each registration samples the id port (P05 behaviour).
  std::optional<RuntimeEpoch> runtime_epoch;
};
struct HeartbeatCommand {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  // Wire Fence values only: instance, runtime epoch and the positive connection generation. The
  // internal connection id is resolved by the application from the current authoritative record.
  AdapterFenceClaim fence;
};
struct AdapterQuery {
  CommandContext context;
  ResourceId adapter;
};
struct AdapterListQuery {
  CommandContext context;
  PageRequest page;
};
struct ValidateManifestCommand {
  CommandContext context;
  std::vector<std::byte> manifest;
};
struct CreateInstallationRequest {
  CommandContext context;
  std::string plugin_type_id;
  std::vector<std::byte> manifest;
};
struct InstallationQuery {
  CommandContext context;
  InstallationId installation;
};
struct InstallationListQuery {
  CommandContext context;
  PageRequest page;
};
struct CreateInstanceRequest {
  CommandContext context;
  InstallationId installation;
  std::optional<ResourceId> host;
  bool externally_managed = false;
};
struct InstanceQuery {
  CommandContext context;
  InstanceId instance;
};
struct InstanceListQuery {
  CommandContext context;
  PageRequest page;
};
struct InstanceConfigQuery {
  CommandContext context;
  InstanceId instance;
};
struct SetDesiredStateRequest {
  CommandContext context;
  InstanceId instance;
  DesiredState desired;
};
struct UpdateConfigRequest {
  CommandContext context;
  InstanceId instance;
  std::vector<std::byte> values;
};
struct ResourceStateQuery {
  CommandContext context;
  ResourceRef resource;
};
struct OperationQuery {
  CommandContext context;
  OperationRef operation;
};
// P13a reversible lifecycle commands. The owner marks a disabling gate, records an Operation with
// participant progress and a tombstone, and returns the accepted operation. External participants
// (Host stop/drain, Scheduler task cancellation) are advanced by lifecycle_maintenance outside
// every UoW; a missing participant port keeps the operation BLOCKED instead of faking completion.
struct BeginDeleteRequest {
  CommandContext context;
  ResourceRef resource;
};
struct RestoreResourceRequest {
  CommandContext context;
  ResourceRef resource;
};
struct LifecycleMutation {
  Operation operation;
};
enum class LifecycleParticipantDisposition { Done, Blocked, Unknown };
struct LifecycleParticipantResult {
  LifecycleParticipantDisposition disposition = LifecycleParticipantDisposition::Blocked;
  std::optional<ErrorCode> reason;
  std::optional<TimePoint> next_attempt;
};
// Ports are deliberately owner-local and network-free inside a UoW: the application samples state
// in one read UoW, calls these outside the transaction, then applies results in a second UoW.
class HostLifecycleParticipantPort {
public:
  virtual ~HostLifecycleParticipantPort() = default;
  virtual LifecycleParticipantResult stop_instance(const ResourceRef& resource) = 0;
};
class SchedulerLifecycleParticipantPort {
public:
  virtual ~SchedulerLifecycleParticipantPort() = default;
  virtual LifecycleParticipantResult cancel_tasks(const ResourceRef& resource) = 0;
};
struct LifecycleMaintenance {
  std::size_t reconciled = 0;
  std::size_t completed = 0;
  std::size_t blocked = 0;
  std::size_t purged = 0;
  std::size_t pruned = 0;
  bool cleanup_failed = false;
};
inline constexpr auto kLifecycleRestoreWindow =
    std::chrono::duration_cast<TimePoint::duration>(std::chrono::days{30});
inline constexpr auto kLifecycleTombstoneRetention =
    std::chrono::duration_cast<TimePoint::duration>(std::chrono::days{180});
inline constexpr auto kLifecycleOperationRetention =
    std::chrono::duration_cast<TimePoint::duration>(std::chrono::days{180});
inline constexpr auto kLifecycleParticipantRetry =
    std::chrono::duration_cast<TimePoint::duration>(std::chrono::seconds{5});
// P10b Runtime-side timer trigger acceptance. The Scheduler owns the occurrence identity; Runtime
// only validates the handoff and durably enqueues the TimerTriggered event exactly once.
struct AcceptTimerTriggerRequest {
  CommandContext context;
  HandoffContext handoff;
  MessageId occurrence_id;
  std::string task_id;
  InstanceId owner_instance_id;
  ResourceVersion schedule_version;
  std::string conversation_id;
  ConversationKey::Kind conversation_kind = ConversationKey::Kind::Unspecified;
  std::vector<std::byte> payload;
  // Serialized EventEnvelope carrying the TimerTriggered payload; built by the boundary so the
  // application stays independent of protobuf.
  std::string serialized_envelope;
  // Set only by the boundary after it verified an enrolled platform service peer (Scheduler) from
  // the real mTLS leaf + SAN. No delegated user check applies; handoff integrity is still enforced
  // below, and no other Runtime method accepts this flag.
  bool platform_peer_authorized = false;
};
struct TimerTriggerAcceptance {
  MessageId occurrence_id;
  MessageId event_id;
  bool duplicate = false;
  TimePoint persisted_at;
};

// Runtime resource application boundary. Every method owns its UoWs, enforces current online
// authorization before effects, commits receipts/audit/operation in the same local transaction and
// never performs network I/O inside a UoW. Boundary adapters only convert wire values and map
// stable failures.
struct HostCommandMaintenance {
  std::size_t expired = 0;
  std::size_t pruned = 0;
};

class RuntimeResourceApplication : public ServiceReadinessPort {
public:
  ~RuntimeResourceApplication() override = default;

  virtual Outcome<BotAccount> create_account(const CreateAccountRequest& request) = 0;
  virtual Outcome<AccountMutation> update_account(const UpdateAccountRequest& request) = 0;
  virtual Outcome<AccountMutation> disable_account(const DisableAccountRequest& request) = 0;
  virtual Outcome<BotAccount> get_account(const AccountQuery& request) = 0;
  virtual Outcome<ListResult<BotAccount>> list_accounts(const AccountListQuery& request) = 0;

  virtual Outcome<BindingMutation> bind_adapter(const BindAdapterRequest& request) = 0;
  virtual Outcome<BindingMutation> unbind_adapter(const UnbindAdapterRequest& request) = 0;

  virtual Outcome<AdapterRegistrationResult>
  register_adapter(const AdapterRegistrationCommand& request) = 0;
  virtual Outcome<HeartbeatResult> heartbeat(const HeartbeatCommand& request) = 0;
  virtual Outcome<AdapterRecord> get_adapter(const AdapterQuery& request) = 0;
  virtual Outcome<ListResult<AdapterRecord>> list_adapters(const AdapterListQuery& request) = 0;

  virtual Outcome<ManifestValidation> validate_manifest(const ValidateManifestCommand& request) = 0;
  virtual Outcome<PluginInstallation>
  create_installation(const CreateInstallationRequest& request) = 0;
  virtual Outcome<PluginInstallation> get_installation(const InstallationQuery& request) = 0;
  virtual Outcome<ListResult<PluginInstallation>>
  list_installations(const InstallationListQuery& request) = 0;

  virtual Outcome<InstanceCreation> create_instance(const CreateInstanceRequest& request) = 0;
  virtual Outcome<PluginInstance> get_instance(const InstanceQuery& request) = 0;
  virtual Outcome<ListResult<PluginInstance>> list_instances(const InstanceListQuery& request) = 0;
  virtual Outcome<InstanceConfiguration>
  get_instance_config(const InstanceConfigQuery& request) = 0;
  virtual Outcome<InstanceMutation> apply_desired_state(const SetDesiredStateRequest& request) = 0;
  virtual Outcome<ConfigMutation> update_config(const UpdateConfigRequest& request) = 0;

  virtual Outcome<ResourceState> get_resource_state(const ResourceStateQuery& request) = 0;
  virtual Outcome<TimerTriggerAcceptance>
  accept_timer_trigger(const AcceptTimerTriggerRequest& request) = 0;
  virtual Outcome<Operation> get_operation(const OperationQuery& request) = 0;

  // P13a lifecycle coordination. BeginDelete/Restore are owner-local durable commands; the
  // maintenance pass is called from the Runtime once-per-second dependency hook and performs one
  // bounded participant reconciliation plus the 60s retention batch.
  virtual Outcome<Operation> begin_delete(const BeginDeleteRequest& request) = 0;
  virtual Outcome<Operation> restore_resource(const RestoreResourceRequest& request) = 0;
  virtual Outcome<LifecycleMaintenance> lifecycle_maintenance() = 0;

  // P08b-3a trusted HostBridge persistence (called only by the Runtime transport adapter after
  // the internal-mTLS fence/allowlist checks). These are not public RPC methods: they read/write
  // the Host outbox/observation projections in short owner UoWs.
  virtual Outcome<std::vector<HostCommandOutboxRecord>>
  host_pending_commands(const std::string& host_id, std::uint64_t connection_generation,
                        std::size_t limit) = 0;
  virtual Status host_command_dispatched(const std::string& command_id,
                                         std::uint64_t connection_generation,
                                         std::string_view runtime_epoch) = 0;
  // Receipt is accepted only for the connection generation the command was dispatched on; a
  // repeated identical receipt is idempotent, a changed receipt is Conflict.
  virtual Status host_command_receipt(const std::string& command_id,
                                      std::uint64_t connection_generation, bool persisted,
                                      std::optional<ExecutionOutcome> outcome,
                                      std::optional<std::string> process_marker) = 0;
  virtual Outcome<bool> host_observation(const HostObservationRecord& observation) = 0;
  virtual Outcome<HostCommandMaintenance> host_command_maintenance() = 0;
};

struct RuntimeResourceDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeSecretPort& secrets;
  // Fail-closed composition adapter until the Identity current-authorization client is wired.
  RuntimeAuthorizationPort& authorization;
  // Trusted capability catalog this build actually supports; never request-controlled.
  std::vector<std::string> supported_capabilities;
  // P13a participant clients are wired by the composition when P08b-3/P10b clients exist. A null
  // pointer is fail-closed (participant BLOCKED/retryable), never a fake successful stop/cancel.
  HostLifecycleParticipantPort* host_lifecycle = nullptr;
  SchedulerLifecycleParticipantPort* scheduler_lifecycle = nullptr;
};

// Borrowed dependencies outlive every call; each call owns separate short UoWs.
Outcome<std::unique_ptr<RuntimeResourceApplication>>
make_runtime_resource_application(RuntimeResourceDependencies dependencies);

} // namespace kiarabot::core::target
