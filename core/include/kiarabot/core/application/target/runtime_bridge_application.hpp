#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/runtime_action_application.hpp"
#include "kiarabot/core/application/target/runtime_plugin_capability.hpp"
#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/domain/target/bridge_rules.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/runtime_secrets.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// Bounded, already-converted adapter registration frame. The transport may be either the external
// AdapterBridge stream (bootstrap_authenticated path) or the internal dispatcher; the application
// performs the same validation and persistence either way.
struct BridgeAdapterRegistration {
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  ResourceId adapter;
  std::string implementation;
  std::string implementation_version;
  std::string protocol_version;
  std::optional<InstanceId> presented_instance;
  std::vector<ResourceId> declared_accounts;
  std::vector<Capability> declared_capabilities;
  // The bootstrap credential's SHA-256 is resolved by Runtime; the plaintext is never persisted.
  std::string bearer_credential;
};

struct BridgePluginManifest {
  std::string id;
  std::string name;
  std::string version;
  std::string api_version;
  std::string runtime;
  std::string entrypoint;
  std::string isolation;
  std::string config_schema;
  bool ordered = false;
  std::vector<std::string> capabilities;
  std::vector<std::string> permissions;
  std::vector<std::string> dependencies;
};

struct BridgePluginRegistration {
  std::string request_id;
  std::string instance_id;
  BridgePluginManifest manifest;
  std::vector<std::string> supported_features;
  std::optional<InstallationId> installation_id;
  std::optional<std::uint64_t> execution_generation;
  std::string bearer_credential;
};

// Decoded inbound adapter event frame. The transport converts the wire envelope once and keeps
// `serialized_envelope` byte-exact for durable storage; the application only derives the four-tuple
// conversation key, payload kind, sequence and trace correlation.
struct BridgeEventIngress {
  std::string adapter_id;
  std::string account_id;
  std::string event_id;
  std::string trace_id;
  EventPayloadKind payload = EventPayloadKind::MessageReceived;
  bool has_conversation = false;
  std::string conversation_id;
  ConversationKey::Kind conversation_type = ConversationKey::Kind::Unspecified;
  std::optional<std::uint64_t> sequence;
  TimePoint occurred_at;
  std::string serialized_envelope;
};

enum class BridgeAckStatus { Persisted, Duplicate, Rejected };
struct BridgeEventAck {
  BridgeAckStatus status = BridgeAckStatus::Rejected;
  std::optional<Failure> failure;
  // Frozen backpressure semantics: capacity/out-of-order rejections are retryable RATE_LIMITED /
  // CONFLICT with retry_after_ms and close the stream; the adapter retains the event and resends
  // after reconnecting. Non-retryable rejections finish the event.
  bool retryable = false;
  std::uint32_t retry_after_ms = 0;
  bool close_stream = false;
};

// Wire-supplied attempt identity for ACK/Renew. The transport never supplies internal execution or
// connection ids; the application resolves them from the persisted current instance fence.
struct BridgeAttemptRef {
  std::string event_id;
  AttemptId attempt;
  BridgeFence fence;
};

// Push callback used by the central dispatcher to hand one claimed delivery to its plugin stream.
// Returns a Failure when the stream could not accept the frame; the lease then expires and the
// normal sweep/retry path redelivers it.
using DeliverySink = std::function<Status(const EventRecord& event, const DeliveryAttempt& attempt,
                                          std::string_view trace_id)>;
// Bounded graceful-shutdown notice callback. Writes exactly one ShutdownNotice frame on the
// current stream; the transport remains responsible for closing the stream.
using ShutdownSink = std::function<Status()>;

struct BridgeRegistrationResult {
  InstanceId assigned_instance_id;
  BridgeAuthorization authorization;
  std::uint32_t heartbeat_interval_seconds = 30;
  std::vector<Capability> effective_capabilities;
  std::vector<std::string> granted_permissions;
  std::vector<std::string> enabled_features;
  std::uint64_t desired_config_version = 0;
  std::string required_api_version;
  std::string required_protocol_version;
};

// One decoded plugin Action submission from the authenticated bridge stream. The transport owns
// the wire ActionEnvelope parse; the bridge application resolves the owner identity, manifest
// permission/feature set and the Runtime Action application transaction.
struct BridgeActionSubmit {
  std::string request_id;
  std::string trace_id;
  ActionId action_id;
  ResourceId account_id;
  ResourceId adapter_id;
  ActionKind kind = ActionKind::SendMessage;
  std::string idempotency_key;
  std::string serialized_envelope;
  std::string target_conversation;
  std::uint8_t target_conversation_kind = 0;
  TimePoint deadline;
};

struct BridgeActionReport {
  ActionId action_id;
  std::optional<AttemptId> dispatch_attempt_id;
  ActionTerminalResult result;
  std::string trace_id;
};

struct BridgeEvidenceReport {
  ActionId action_id;
  AttemptId dispatch_attempt_id;
  ExecutionOutcome execution_outcome = ExecutionOutcome::Unknown;
  std::optional<ErrorCode> error_code;
  std::optional<std::string> platform_reference;
  TimePoint observed_at;
  std::string trace_id;
};

// Push callback used by the central maintenance dispatcher to hand one claimed Action frame to the
// adapter stream. The claim has already been committed as Dispatched; a failed write can only make
// the action resolve UNKNOWN/EXPIRED, never re-dispatch it.
using ActionDispatchSink = std::function<Status(const ActionDispatchFrame&)>;

// Trusted outer-composition dependencies. The bridge application owns the in-memory connection
// registry (single active connection per external peer); durable adapter/instance records stay
// behind the Runtime unit of work. No method performs network I/O inside a UoW.
struct RuntimeBridgeDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeSecretPort& secrets;
  RuntimeResourceApplication& resources;
  // Optional P07 Action application; both the plugin Action surface and adapter dispatch/evidence
  // are only available when the composition supplies it.
  RuntimeActionApplication* actions = nullptr;
  // Trusted build feature catalog; the registration response reports only this intersection.
  std::vector<std::string> supported_features;
};

// Ends the durable AdapterRecord presence projection for one bridge connection through the owner
// adapter repository. Returns true when the projection was written (online_until := now, version
// bumped) and false for an idempotent no-op (absent/deleted/owner-mismatched record, a newer
// connection generation, or an already-offline row). Storage faults are Failure; the helper never
// creates, revives or retargets a row, so a stale or repeated teardown cannot resurrect presence.
Outcome<bool> end_adapter_presence(AdapterRegistryRepository& adapters, const InstanceId& instance,
                                   ConnectionGeneration connection_generation, TimePoint now);

class RuntimeBridgeApplication {
public:
  virtual ~RuntimeBridgeApplication() = default;

  // External AdapterBridge registration: bootstrap -> assigned instance -> durable AdapterRecord
  // with the bootstrap resource identity, then a fresh ConnectionAuthorization. A new connection
  // replaces the previous one for the same adapter instance.
  virtual Outcome<BridgeRegistrationResult>
  register_adapter(const BridgeAdapterRegistration& request) = 0;
  // External PluginBridge registration for an externally-managed instance. The first registration
  // allocates execution_generation 1; reconnects keep the external execution identity and advance
  // only connection_generation (P06 B1).
  virtual Outcome<BridgeRegistrationResult>
  register_plugin(const BridgePluginRegistration& request) = 0;
  // Same-stream single-flight proof/refresh rotation for the currently registered connection. The
  // caller passes the session material it holds; a stale stream or mismatched material fails
  // Unauthenticated and never rotates.
  virtual Outcome<BridgeAuthorization> refresh(BridgePeerKind kind, const InstanceId& instance,
                                               const BridgeAuthorization& current,
                                               const BridgeRefreshRequest& request) = 0;
  // Per-frame authorization: complete current Fence, opaque proof and unexpired material. The
  // frame must belong to the currently registered connection (older connections are rejected).
  virtual Status authorize_frame(BridgePeerKind kind, const BridgeAuthorization& current,
                                 const BridgeFence& fence, const std::array<std::byte, 32>& proof,
                                 TimePoint now) = 0;
  // Idempotent cleanup after the stream ends; only removes the still-current connection. An
  // Adapter stream end additionally ends the durable presence projection immediately (the online
  // window is only a crash lease) through the Runtime owner UoW; a projection write fault is
  // reported to the transport but never fatal for the disconnecting thread.
  virtual Status disconnect(BridgePeerKind kind, const InstanceId& instance,
                            ConnectionGeneration connection_generation) = 0;
  // Adapter event frame -> durable inbox acceptance. Fan-out targets are the currently connected
  // plugin sessions whose manifest declares the matching event capability; the ordered flag is the
  // subscription snapshot at acceptance time.
  virtual Outcome<BridgeEventAck> ingest_event(const InstanceId& adapter_instance,
                                               const BridgeEventIngress& ingress) = 0;
  // Register/remove the push sink of one accepted plugin connection. The central dispatcher
  // claims due deliveries for the persisted instance fence and pushes them through this sink.
  virtual Status attach_plugin_delivery(const InstanceId& instance,
                                        ConnectionGeneration connection_generation,
                                        const DeliverySink& sink) = 0;
  virtual void detach_plugin_delivery(const InstanceId& instance,
                                      ConnectionGeneration connection_generation) = 0;
  // Server-owned registration facts, keyed by the authenticated instance. Absent for stale or
  // unregistered instances. The caller must first verify the current connection's fence/proof.
  // Capability entry points fetch a fresh snapshot for each ingress and apply business admission
  // before downstream I/O; an admitted snapshot is not a reusable connection authorization.
  using BridgePluginProfile = PluginCapabilityProfile;
  virtual Outcome<BridgePluginProfile> plugin_profile(const InstanceId& instance) = 0;
  Outcome<BridgePluginProfile> task_profile(const InstanceId& instance,
                                            PluginTaskOperation operation);
  Outcome<BridgePluginProfile> storage_profile(const InstanceId& instance);
  // P07 plugin Action surface (action.result.v1): submit/query/cancel scoped to the authenticated
  // plugin instance. The bridge resolves manifest permissions/features from the registered profile.
  virtual Outcome<ActionRecord> submit_plugin_action(const InstanceId& instance,
                                                     const BridgeActionSubmit& request) = 0;
  virtual Outcome<ActionRecord> query_plugin_action(const InstanceId& instance,
                                                    std::optional<ActionId> action_id,
                                                    std::optional<std::string> idempotency_key) = 0;
  virtual Outcome<ActionRecord> cancel_plugin_action(const InstanceId& instance,
                                                     const ActionId& action_id) = 0;
  // P07 adapter result/evidence reports. The caller must be the adapter instance recorded on the
  // Action dispatch; report receipts are returned for the ActionReceipt frame.
  virtual Outcome<ActionReportReceipt> record_adapter_result(const InstanceId& adapter_instance,
                                                             const BridgeActionReport& report) = 0;
  virtual Outcome<ActionReportReceipt>
  record_adapter_evidence(const InstanceId& adapter_instance,
                          const BridgeEvidenceReport& report) = 0;
  // Register/remove the adapter Action push sink for one accepted connection generation. The
  // maintenance hook claims due Actions and pushes them through this sink outside the transaction.
  virtual Status attach_adapter_action_sink(const InstanceId& instance,
                                            ConnectionGeneration connection_generation,
                                            const ActionDispatchSink& sink) = 0;
  virtual void detach_adapter_action_sink(const InstanceId& instance,
                                          ConnectionGeneration connection_generation) = 0;
  // Bounded periodic work run by the composition maintenance hook: expiry sweep plus one claim
  // round for every connected plugin sink and adapter Action sink, then push outside transactions.
  virtual Status maintenance() = 0;
  // Graceful shutdown: send one bounded ShutdownNotice to every accepted peer. Returns after the
  // notices were attempted; in-flight work is bounded by the transport drain budget.
  virtual Status attach_shutdown_sink(BridgePeerKind kind, const InstanceId& instance,
                                      ConnectionGeneration connection_generation,
                                      const ShutdownSink& sink) = 0;
  virtual void detach_shutdown_sink(BridgePeerKind kind, const InstanceId& instance,
                                    ConnectionGeneration connection_generation) = 0;
  virtual Status shutdown_notice() = 0;
  // Plugin ACK/Renew for one claimed attempt. Unknown/stale attempt ids map to DELIVERY_STALE and
  // never confirm another attempt.
  virtual Outcome<DeliveryReceipt>
  acknowledge_delivery(const InstanceId& instance, const BridgeAttemptRef& supplied, bool accepted,
                       std::optional<Failure> failure, bool retryable) = 0;
  virtual Outcome<DeliveryReceipt> renew_delivery(const InstanceId& instance,
                                                  const BridgeAttemptRef& supplied) = 0;
  // Authoritative database UTC sample used by the transport for frame/refresh expiry checks.
  virtual Outcome<TimePoint> current_time() = 0;
  virtual bool available() const = 0;
};

Outcome<std::unique_ptr<RuntimeBridgeApplication>>
make_runtime_bridge_application(RuntimeBridgeDependencies dependencies);

} // namespace kiarabot::core::target
