#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/domain/target/bridge_rules.hpp"
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

struct BridgeRegistrationResult {
  InstanceId assigned_instance_id;
  BridgeAuthorization authorization;
  std::uint32_t heartbeat_interval_seconds = 30;
  std::vector<Capability> effective_capabilities;
  std::vector<std::string> granted_permissions;
  std::vector<std::string> enabled_features;
  std::string required_api_version;
  std::string required_protocol_version;
};

// Trusted outer-composition dependencies. The bridge application owns the in-memory connection
// registry (single active connection per external peer); durable adapter/instance records stay
// behind the Runtime unit of work. No method performs network I/O inside a UoW.
struct RuntimeBridgeDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeSecretPort& secrets;
  RuntimeResourceApplication& resources;
  // Trusted build feature catalog; the registration response reports only this intersection.
  std::vector<std::string> supported_features;
};

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
  // Idempotent cleanup after the stream ends; only removes the still-current connection.
  virtual Status disconnect(BridgePeerKind kind, const InstanceId& instance,
                            ConnectionGeneration connection_generation) = 0;
  // Authoritative database UTC sample used by the transport for frame/refresh expiry checks.
  virtual Outcome<TimePoint> current_time() = 0;
  virtual bool available() const = 0;
};

Outcome<std::unique_ptr<RuntimeBridgeApplication>>
make_runtime_bridge_application(RuntimeBridgeDependencies dependencies);

} // namespace kiarabot::core::target
