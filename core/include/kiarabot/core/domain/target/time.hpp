#pragma once

#include <cstdint>
#include <limits>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

struct TimestampParts {
  std::int64_t seconds;
  std::int32_t nanos;
  bool operator==(const TimestampParts&) const = default;
};
// Validate BEFORE multiplication/conversion. Wire timestamp range is wider than int64 epoch ns;
// unsupported extreme values reject explicitly rather than wrap or lose precision.
inline Outcome<TimePoint> time_point_from_parts(TimestampParts timestamp) {
  constexpr std::int64_t kNanosPerSecond = 1000000000;
  constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  // C++ division truncates toward zero; represent the negative lower edge using floor seconds.
  constexpr auto kMinimumSeconds = kMinimum / kNanosPerSecond - 1;
  constexpr auto kMinimumNanos = kMinimum % kNanosPerSecond + kNanosPerSecond;
  constexpr auto kMaximumSeconds = kMaximum / kNanosPerSecond;
  constexpr auto kMaximumNanos = kMaximum % kNanosPerSecond;
  if (timestamp.nanos < 0 || timestamp.nanos >= kNanosPerSecond ||
      timestamp.seconds < kMinimumSeconds || timestamp.seconds > kMaximumSeconds ||
      (timestamp.seconds == kMinimumSeconds && timestamp.nanos < kMinimumNanos) ||
      (timestamp.seconds == kMaximumSeconds && timestamp.nanos > kMaximumNanos)) {
    return Failure{ErrorCode::InvalidArgument, Acceptance::NotAccepted, Recovery::CorrectInput,
                   std::nullopt, std::nullopt};
  }
  std::int64_t count = 0;
  if (timestamp.seconds == kMinimumSeconds) {
    // floor seconds alone would underflow, but the validated final timestamp is representable.
    count = kMinimum + (timestamp.nanos - kMinimumNanos);
  } else if (timestamp.seconds == kMaximumSeconds) {
    count = kMaximum - (kMaximumNanos - timestamp.nanos);
  } else {
    count = timestamp.seconds * kNanosPerSecond + timestamp.nanos;
  }
  return TimePoint{std::chrono::nanoseconds{count}};
}
// Canonical negative timestamps use floor seconds and nonnegative nanos, never a negative nanos
// remainder. Every internal int64-ns value round-trips, including both representable endpoints.
inline TimestampParts time_point_to_parts(TimePoint timestamp) noexcept {
  constexpr std::int64_t kNanosPerSecond = 1000000000;
  const auto count = timestamp.time_since_epoch().count();
  auto seconds = count / kNanosPerSecond;
  auto nanos = count % kNanosPerSecond;
  if (nanos < 0) {
    --seconds;
    nanos += kNanosPerSecond;
  }
  return {seconds, static_cast<std::int32_t>(nanos)};
}

} // namespace kiarabot::core::target
