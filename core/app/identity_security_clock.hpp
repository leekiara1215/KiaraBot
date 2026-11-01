#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

#include "kiarabot/core/ports/target/identity_crypto.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"

namespace kiarabot::core::composition::identity_clock_detail {

// Private composition measurement seam. Only native clocks produce runtime observations.
struct Observation {
  target::TimePoint utc;
  std::chrono::steady_clock::time_point before;
  std::chrono::steady_clock::time_point after;
  std::chrono::nanoseconds resolution;
};
struct State {
  bool declared_trusted = false;
  std::chrono::milliseconds maximum_known_skew{};
  bool failed = false;
  std::optional<Observation> baseline;
  std::optional<std::chrono::steady_clock::time_point> last_after;
};
// Pure measurement validation for deterministic tests. A detected drift/regression latches
// failure; a preempted (>1ms) measurement is unavailable without replacing the baseline.
target::Outcome<target::SecurityClockSample> observe(State& state,
                                                     const Observation& observation) noexcept;

} // namespace kiarabot::core::composition::identity_clock_detail

namespace kiarabot::core::composition {

// Outer deployment clock, not an NTP detector. Trust/skew require an explicit declaration.
// Samples UTC between steady-clock reads, accounting for sampling interval/clock precision.
// Drift is compared to the original baseline (never a rolling window). Faults remain unavailable
// until a new instance is explicitly constructed; no worker thread or network access is created.
class SystemSecurityClock final : public target::SecurityClockPort {
public:
  explicit SystemSecurityClock(const platform::IdentityClockDeclaration& declaration);
  ~SystemSecurityClock() override = default;
  SystemSecurityClock(const SystemSecurityClock&) = delete;
  SystemSecurityClock& operator=(const SystemSecurityClock&) = delete;
  target::Outcome<target::SecurityClockSample> sample() noexcept override;
  // Snapshot only: never samples clocks, locks or accesses a database from readiness.
  bool healthy() const noexcept;

private:
  std::mutex mutex_;
  identity_clock_detail::State state_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> available_{false};
};

} // namespace kiarabot::core::composition
