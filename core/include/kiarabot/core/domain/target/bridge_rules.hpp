#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// External Bridge peer families. Both authenticate with a per-instance bootstrap credential and
// exchange a short-lived opaque ConnectionAuthorization proof; after registration every frame
// carries the current complete Fence plus the proof. The two families never share a credential
// namespace or a connection registry entry.
enum class BridgePeerKind { Adapter, Plugin };

inline std::string_view bridge_peer_name(BridgePeerKind kind) noexcept {
  return kind == BridgePeerKind::Adapter ? "adapter" : "plugin";
}

// Wire-visible fence of one accepted external connection. Execution generation is supplied for
// P06 because the already-shipped SDK/adapter clients require a positive value: for an external
// peer the registration connection represents the external process execution. A P08 Host-managed
// process keeps its own real execution generation instead.
struct BridgeFence {
  InstanceId instance;
  RuntimeEpoch runtime_epoch;
  ExecutionGeneration execution_generation;
  ConnectionGeneration connection_generation;
  bool operator==(const BridgeFence&) const = default;
  bool operator!=(const BridgeFence&) const = default;
};

// Immutable authorization material of one connection. Proof and refresh credentials exist only in
// process memory (and the peer's); Runtime never persists them.
struct BridgeAuthorization {
  BridgeFence fence;
  std::array<std::byte, 32> proof{};
  std::array<std::byte, 32> refresh_credential{};
  std::uint64_t refresh_generation = 0;
  std::uint64_t credential_version = 0;
  std::uint64_t authorization_version = 0;
  TimePoint issued_at;
  TimePoint expires_at;
};

// Frozen connection authorization lifetime: the credential itself lives at most 56 seconds so the
// total budget including refresh skew stays under the 60-second contract bound.
inline constexpr auto kBridgeAuthorizationLifetime = std::chrono::seconds{56};
inline constexpr std::uint64_t kBridgeCredentialVersion = 1;
// Bounded external connection table; registration above the limit is rejected with RATE_LIMITED.
inline constexpr std::size_t kMaximumBridgeConnections = 128;
// Local authorization gate refresh interval: a revoked/disabled peer is rejected within 1 second.
inline constexpr auto kBridgeAuthorizationGateInterval = std::chrono::milliseconds{500};
inline constexpr std::size_t kBridgeProofBytes = 32;

inline bool valid_bridge_fence(const BridgeFence& fence) {
  return !fence.instance.value.empty() && !fence.runtime_epoch.value.empty() &&
         fence.execution_generation.value > 0 && fence.connection_generation.value > 0;
}

inline bool valid_bridge_authorization(const BridgeAuthorization& authorization, TimePoint now) {
  if (!valid_bridge_fence(authorization.fence) || authorization.refresh_generation == 0 ||
      authorization.credential_version == 0 || authorization.authorization_version == 0 ||
      authorization.issued_at >= authorization.expires_at || now < authorization.issued_at ||
      now >= authorization.expires_at ||
      authorization.expires_at - authorization.issued_at > kBridgeAuthorizationLifetime) {
    return false;
  }
  bool nonzero = false;
  for (const auto byte : authorization.proof) {
    nonzero = nonzero || byte != std::byte{0};
  }
  if (!nonzero) {
    return false;
  }
  bool same = true;
  for (std::size_t index = 0; index < authorization.proof.size(); ++index) {
    same = same && authorization.proof[index] == authorization.refresh_credential[index];
  }
  return !same;
}

// Constant-time comparison of opaque connection material. Lengths are fixed, so a length mismatch
// is a protocol error rather than a partial match.
inline bool bridge_secret_equal(const std::array<std::byte, 32>& expected,
                                const std::array<std::byte, 32>& supplied) {
  unsigned char difference = 0;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    difference |= static_cast<unsigned char>(expected[index] ^ supplied[index]);
  }
  return difference == 0;
}

struct BridgeRefreshRequest {
  BridgeFence fence;
  std::array<std::byte, 32> proof{};
  std::array<std::byte, 32> refresh_credential{};
  std::uint64_t refresh_generation = 0;
};

inline bool bridge_frame_fence_matches(const BridgeAuthorization& authorization,
                                       const BridgeFence& supplied) {
  return authorization.fence == supplied;
}

// The local Runtime gate for a bootstrap/refresh: an active per-instance credential plus the
// resource lifecycle gates. Identity online authorization stays authoritative for business
// operations; this gate only proves the connection fact.
inline bool bridge_peer_eligible(bool credential_active, bool resource_active,
                                 bool externally_owned) {
  return credential_active && resource_active && externally_owned;
}

} // namespace kiarabot::core::target
