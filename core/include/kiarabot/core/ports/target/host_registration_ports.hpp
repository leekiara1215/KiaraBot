#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
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
// HOST-03 durable Runtime-owned dispatch gate. One row per Host; monotonic gate_version is the
// only authority for new Host command dispatch. Admission is a duplicate of the Host intent so a
// lost Host ACK can be replayed without inventing state, never a process-stop proof.
struct HostAdmissionGateRecord {
  ResourceId host;
  OperationRef operation;
  std::uint64_t maintenance_version = 0;
  std::uint64_t gate_version = 0;
  bool admission_open = false;
  TimePoint updated_at;
  bool operator==(const HostAdmissionGateRecord&) const = default;
};

// Runtime-owned persistence for the admission gate, borrowed by one owner UoW. Expected version
// CAS is the concurrency authority; equal intent replays must return the original row.
class HostAdmissionGateStore {
public:
  virtual ~HostAdmissionGateStore() = default;
  virtual Outcome<std::optional<HostAdmissionGateRecord>> current(const ResourceId& host) = 0;
  virtual Outcome<HostAdmissionGateRecord>
  save(const HostAdmissionGateRecord& value,
       std::optional<std::uint64_t> expected_gate_version) = 0;
};

// Read-only live view of the current accepted HostBridge connection. Runtime coordination calls
// validate the presented fence against this registry; it is a liveness/identity observation only
// and never rewrites process outcomes or business state.
class HostFenceRegistry {
public:
  void record(const HostFence& fence) {
    std::lock_guard lock(mutex_);
    fences_[fence.host.value] = fence;
  }
  void erase(const HostFence& fence) {
    std::lock_guard lock(mutex_);
    const auto found = fences_.find(fence.host.value);
    if (found != fences_.end() && found->second == fence) {
      fences_.erase(found);
    }
  }
  Outcome<std::optional<HostFence>> current(const ResourceId& host) {
    std::lock_guard lock(mutex_);
    const auto found = fences_.find(host.value);
    if (found == fences_.end()) {
      return std::optional<HostFence>{};
    }
    return std::optional<HostFence>{found->second};
  }

private:
  std::mutex mutex_;
  std::map<std::string, HostFence> fences_;
};

class HostClockPort {
public:
  virtual ~HostClockPort() = default;
  virtual Outcome<TimePoint> now() = 0;
  // Server CSPRNG Runtime epoch; supplied once per process, never derived from time.
  virtual Outcome<std::string> runtime_epoch() = 0;
  virtual Outcome<std::uint64_t> next_connection_generation() = 0;
};

// Process-wide HostClockPort for owner coordination paths that need trusted local time only.
class SystemHostClockPort final : public HostClockPort {
public:
  Outcome<TimePoint> now() override {
    return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
  }
  Outcome<std::string> runtime_epoch() override {
    return hex(16);
  }
  Outcome<std::uint64_t> next_connection_generation() override {
    const auto random = hex(8);
    const auto& text = std::get<std::string>(random);
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (parsed.ec != std::errc{} || value == 0) {
      return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, std::nullopt,
                     std::nullopt};
    }
    return value;
  }

private:
  static Outcome<std::string> hex(std::size_t bytes) {
    std::random_device device;
    std::mt19937_64 generator{device()};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes * 2);
    for (std::size_t index = 0; index < bytes; ++index) {
      const auto value = static_cast<unsigned>(generator() & 0xffU);
      result.push_back(kHex[value >> 4]);
      result.push_back(kHex[value & 0x0f]);
    }
    return result;
  }
};

} // namespace kiarabot::core::target
