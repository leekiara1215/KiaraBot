#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/domain/target/runtime_records.hpp"
#include "kiarabot/core/domain/target/runtime_rules.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/runtime_bootstrap_deliveries.hpp"
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
  // BOOT-02 no-secret delivery metadata for the explicit fixed recipient. Empty on the legacy
  // omitted-recipient path, on an idempotent replay whose receipt does not store delivery state,
  // and when the active credential already exists (an explicit reissue is then required).
  std::optional<BootstrapDeliveryRecord> delivery;
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
  std::optional<ResourceState> committed_state{};
};
struct InstanceCreation {
  PluginInstance instance;
  std::optional<std::string> bootstrap_credential;
  // BOOT-02 no-secret delivery metadata; see BindingMutation::delivery for the empty cases.
  std::optional<BootstrapDeliveryRecord> delivery;
};
struct ConfigMutation {
  InstanceConfiguration config;
  std::optional<OperationAccepted> operation;
  std::optional<ResourceState> committed_state{};
};
struct ManifestValidation {
  bool valid = false;
  std::vector<Failure> violations;
};

struct CreateAccountRequest {
  CommandContext context;
  std::string platform;
  std::string platform_account_id;
  std::string display_name;
  ResourceRef catalog{ResourceKind::Catalog, {}};
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
  ResourceRef catalog{ResourceKind::Catalog, {}};
};
struct BindAdapterRequest {
  CommandContext context;
  ResourceId account;
  InstanceId adapter_instance;
  // Empty preserves the existing create/bind path with no delivery intent or deployment effect.
  // Non-empty explicitly opts in to the fixed offline recipient and is validated before mutation.
  std::string bootstrap_recipient_id;
};
struct UnbindAdapterRequest {
  CommandContext context;
  ResourceId account;
  std::optional<InstanceId> adapter_instance = std::nullopt;
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
  ResourceRef catalog{ResourceKind::Catalog, {}};
};
struct ValidateManifestCommand {
  CommandContext context;
  std::vector<std::byte> manifest;
  ResourceRef catalog{ResourceKind::Catalog, {}};
};
struct CreateInstallationRequest {
  CommandContext context;
  std::string plugin_type_id;
  std::vector<std::byte> manifest;
  ResourceRef catalog{ResourceKind::Catalog, {}};
};
struct InstallationQuery {
  CommandContext context;
  InstallationId installation;
};
struct InstallationListQuery {
  CommandContext context;
  PageRequest page;
  ResourceRef catalog{ResourceKind::Catalog, {}};
};
struct CreateInstanceRequest {
  CommandContext context;
  InstallationId installation;
  std::optional<ResourceId> host;
  bool externally_managed = false;
  // Empty preserves the existing create/bind path with no delivery intent or deployment effect.
  // Non-empty explicitly opts in to the fixed offline recipient and is validated before mutation.
  std::string bootstrap_recipient_id;
};
struct InstanceQuery {
  CommandContext context;
  InstanceId instance;
};
struct InstanceListQuery {
  CommandContext context;
  PageRequest page;
  ResourceRef catalog{ResourceKind::Catalog, {}};
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
// P13d structured observability line for the once-per-second Runtime dependency hook. It uses the
// stable key=value shape from doc/08 and contains counts only (never secret/resource content);
// blocked/cleanup_failed are the two signals an operator must alert on. Pipeline errors that make
// the whole round fail are surfaced through readiness instead of this line.
inline std::string format_lifecycle_maintenance_log(const LifecycleMaintenance& maintenance) {
  const bool degraded = maintenance.cleanup_failed || maintenance.blocked > 0;
  return "operation=lifecycle_maintenance outcome=" + std::string{degraded ? "degraded" : "ok"} +
         " reconciled=" + std::to_string(maintenance.reconciled) +
         " completed=" + std::to_string(maintenance.completed) +
         " blocked=" + std::to_string(maintenance.blocked) +
         " purged=" + std::to_string(maintenance.purged) +
         " pruned=" + std::to_string(maintenance.pruned) +
         " cleanup_failed=" + (maintenance.cleanup_failed ? "true" : "false");
}
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

} // namespace kiarabot::core::target
