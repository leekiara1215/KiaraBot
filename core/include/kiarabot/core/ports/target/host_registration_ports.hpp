#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/host_records.hpp"
#include "kiarabot/core/domain/target/runtime_records.hpp"

namespace kiarabot::core::target {

// P08b port contract for Runtime-side Host registration/liveness. Application rules
// (RuntimeHostApplication) live in the application layer; adapters and the UoW only see these
// transport-neutral types.
inline constexpr auto kHostOfflineAfter = std::chrono::seconds{15};
inline constexpr auto kHostHeartbeatInterval = std::chrono::seconds{5};

struct HostFence {
  ResourceId host;
  std::uint64_t connection_generation = 0;
  std::string runtime_epoch;
  bool operator==(const HostFence&) const = default;
};
// Runtime-managed projection of one HostBridge connection. `online` is a liveness observation
// only: offline never proves a managed process stopped, so no process outcome is rewritten.
struct HostConnectionState {
  HostFence fence;
  TimePoint registered_at;
  TimePoint last_heartbeat_at;
  bool online = false;
  // Last desired plugin-config version durably written for managed instances of this host.
  ResourceVersion desired_config_version;
};
struct HostCertificateBinding {
  std::string host_id;
  // Deployment-issued client certificate SHA-256 (lowercase hex). Pre-registered only.
  std::string certificate_sha256;
};
struct HostRegistrationCommand {
  // Trusted boundary result of the internal-mTLS peer certificate, never a wire field.
  std::string certificate_sha256;
  std::string host_id;
  std::vector<std::string> capabilities;
  std::string runtime_epoch;
};
struct HostRegistrationResult {
  HostRegistration registration;
  HostConnectionState connection;
};
struct HostHeartbeatCommand {
  HostFence fence;
  TimePoint sent_at;
};
struct HostHeartbeatResult {
  HostFence fence;
  TimePoint received_at;
  std::chrono::seconds heartbeat_interval = kHostHeartbeatInterval;
  bool resync_required = false;
};

// Deployment allowlist plus the authoritative host/connection rows (PG implements the latter).
class HostRegistrationPort {
public:
  virtual ~HostRegistrationPort() = default;
  virtual Outcome<std::optional<HostCertificateBinding>>
  certificate_binding(std::string_view certificate_sha256) = 0;
  virtual Outcome<std::optional<HostRegistration>> find_host(const ResourceId& host) = 0;
  virtual Status save_host(const HostRegistration& registration,
                           std::optional<ResourceVersion> expected) = 0;
  virtual Outcome<std::optional<HostConnectionState>> connection(const ResourceId& host) = 0;
  // Replaces the current connection generation for this host; the PG adapter must reject a
  // stale/unknown previous generation and allocate/increment the next generation atomically.
  virtual Outcome<HostConnectionState>
  activate_connection(const HostConnectionState& connection) = 0;
  virtual Status save_connection(const HostConnectionState& connection,
                                 std::uint64_t expected_generation) = 0;
};
class HostClockPort {
public:
  virtual ~HostClockPort() = default;
  virtual Outcome<TimePoint> now() = 0;
  // Server CSPRNG Runtime epoch; supplied once per process, never derived from time.
  virtual Outcome<std::string> runtime_epoch() = 0;
  virtual Outcome<std::uint64_t> next_connection_generation() = 0;
};

} // namespace kiarabot::core::target
