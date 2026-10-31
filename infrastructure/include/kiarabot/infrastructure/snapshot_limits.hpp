#pragma once

#include <cstddef>
#include <cstdint>

namespace kiarabot::infrastructure {

// Hard limits of the versioned runtime snapshot. The encoded payload is a
// private database format, so these are infrastructure acceptance limits, not a
// public wire contract. They are enforced on both sides of the storage port:
// commit() rejects snapshots that exceed them and read()/open() reject stored
// payloads that already exceed them.
inline constexpr std::size_t kMaximumSnapshotBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumSnapshotItems = 1000000U;

struct SnapshotLimits {
  std::size_t bytes{kMaximumSnapshotBytes};
  std::size_t items{kMaximumSnapshotItems};
};

// Monotonic per-store rejection counters. They make capacity failures
// observable without exposing the payload or inventing a new StorageError
// value: an oversized commit stays StorageError::Unavailable and an oversized
// stored payload stays StorageError::Corrupt.
struct SnapshotRejections {
  std::uint64_t oversized_bytes{0};
  std::uint64_t oversized_items{0};
  std::uint64_t oversized_reads{0};
};

} // namespace kiarabot::infrastructure
