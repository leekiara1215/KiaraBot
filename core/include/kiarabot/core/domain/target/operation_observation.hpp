#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// A durable owner dataset boundary, not an authorization proof or a stream sequence.
struct OperationObservationCursor {
  Service owner = Service::Runtime;
  std::string epoch;
  std::vector<ResourceRef> scope;
  std::uint64_t sequence = 0;
  bool operator==(const OperationObservationCursor&) const = default;
};

struct OperationObservationQuery {
  // Caller must currently authorize every exact resource before storage access.
  std::vector<ResourceRef> scope;
  std::optional<OperationObservationCursor> cursor;
  std::uint32_t page_size = 0; // 0 means 32; otherwise 1..100.
  // Application clamps this to the remaining request budget before opening the UoW.
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
};

struct OperationObservation {
  Operation operation;
  OperationObservationCursor cursor;
};

struct OperationObservationPage {
  std::vector<OperationObservation> changes;
  OperationObservationCursor next_cursor;
  bool has_more = false;
  TimePoint checked_at{};
};

inline constexpr std::size_t kOperationObservationCapacity = 65536;
inline constexpr std::size_t kOperationObservationBytes = 64 * 1024 * 1024;
inline constexpr auto kOperationObservationRetention = std::chrono::seconds{600};

} // namespace kiarabot::core::target
