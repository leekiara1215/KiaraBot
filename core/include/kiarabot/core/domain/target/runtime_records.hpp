#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// Closed v1 adapter capability vocabulary (doc/01-contracts.md "权限、能力与订阅词表").
// A declared name outside this list is CAPABILITY_NOT_SUPPORTED; a declared known name is not an
// authorization grant. Effective capabilities are the negotiation result only.
inline constexpr std::string_view kRuntimeProtocolVersion = "kiarabot.v1";
inline constexpr std::size_t kMaximumCapabilityCount = 128;
inline constexpr std::size_t kMaximumAdapterAccountCount = 64;
inline constexpr std::size_t kMaximumImplementationText = 128;
inline constexpr std::size_t kMaximumPlatformText = 32;
inline constexpr std::size_t kMaximumPlatformAccountId = 128;
inline constexpr std::size_t kMaximumDisplayName = 128;

struct Capability {
  std::string name;
  std::string version;
  bool operator==(const Capability&) const = default;
};

// Immutable capability declaration/negotiation snapshot of one accepted adapter registration.
struct AdapterRegistrationMetadata {
  std::string implementation;
  std::string implementation_version;
  std::string protocol_version;
  std::vector<Capability> declared_capabilities;
  std::vector<Capability> effective_capabilities;
};

// Accepted protocol connection of one stable adapter resource. External adapters are not
// Runtime-supervised processes: registration assigns/advances ONLY the connection generation and
// the current runtime epoch. The execution id/generation describe one actual process execution
// established by a Host (P08 trusted binding), so an adapter connection must not fabricate them;
// the wire Fence therefore omits its optional execution_generation for adapter frames. The
// connection id itself is Runtime-internal and never travels on the wire.
struct AdapterFence {
  InstanceId instance;
  RuntimeEpoch runtime_epoch;
  ConnectionId connection;
  ConnectionGeneration connection_generation;
  bool operator==(const AdapterFence&) const = default;
};
inline bool valid_adapter_fence(const AdapterFence& fence) {
  return !fence.instance.value.empty() && !fence.runtime_epoch.value.empty() &&
         !fence.connection.value.empty() && fence.connection_generation.value > 0;
}

// Wire-visible adapter connection claim carried by heartbeat/restart frames: the boundary matches
// these values against the current registration and resolves the internal connection id itself.
struct AdapterFenceClaim {
  InstanceId instance;
  RuntimeEpoch runtime_epoch;
  ConnectionGeneration connection_generation;
  bool operator==(const AdapterFenceClaim&) const = default;
};

struct AdapterRecord {
  ResourceId adapter;
  AdapterRegistrationMetadata registration;
  AdapterFence current;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
  TimePoint last_registered_at;
  TimePoint last_heartbeat_at;
  TimePoint online_until;
};

// Account-to-adapter binding. The wire AdapterBinding names the account and the adapter instance,
// so the durable key is the account; an account is served by at most one adapter instance while
// an adapter serves many accounts. Unbind keeps the row as a disabled tombstone (ids are never
// reused); a later Bind creates a new version instead of reviving the old row.
struct AdapterBindingRecord {
  ResourceId account;
  InstanceId adapter_instance;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
};

// Runtime-owned registration projection of a Host. Actual process facts stay with Host; only the
// accepted registration/heartbeat projection and capability strings are stored here.
struct HostRegistration {
  ResourceId host;
  std::vector<std::string> capabilities;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
  TimePoint last_heartbeat_at;
  TimePoint observed_at;
};

enum class ConfigApplyState { Pending, Applied, Rejected, Unavailable };

// Complete config application projection for one plugin instance. `values` is the canonical
// serialized plugin configuration produced by the boundary (Core never parses plugin JSON).
// Versions are Runtime-owned: UpdateConfig increments desired_version; only a matching
// plugin.config.v1 ACK at the current fence may advance applied_version/apply_state.
struct InstanceConfiguration {
  InstanceId instance;
  ResourceVersion desired_version;
  ResourceVersion applied_version;
  ConfigApplyState apply_state = ConfigApplyState::Pending;
  std::string delivery_id;
  std::optional<Fence> fence;
  std::optional<ErrorCode> error;
  TimePoint observed_at;
  std::vector<std::byte> values;
  ResourceVersion version;
};

// Immutable ten-minute page boundary handed to a list query. The application owns the snapshot
// identity/window; a boundary only renders it. This internal value does not claim a persisted
// immutable row set: page tokens carry the window so later pages reject a different snapshot.
struct SnapshotWindow {
  std::string snapshot_id;
  ResourceVersion authorization_version;
  TimePoint observed_at;
  TimePoint expires_at;
};

} // namespace kiarabot::core::target
