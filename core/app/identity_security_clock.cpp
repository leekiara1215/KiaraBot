#include "identity_security_clock.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "kiarabot/core/domain/target/time.hpp"

namespace kiarabot::core::composition {
namespace {

target::Failure unavailable() {
  return {target::ErrorCode::UpstreamUnavailable,
          target::Acceptance::NotAccepted,
          target::Recovery::Diagnose,
          {},
          {}};
}
// Avoid overflowing time_point subtraction before checking the supported int64-ns range.
std::optional<std::chrono::nanoseconds> elapsed(std::chrono::steady_clock::time_point later,
                                                std::chrono::steady_clock::time_point earlier) {
  if (later < earlier) {
    return std::nullopt;
  }
  const auto later_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(later.time_since_epoch());
  const auto earlier_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(earlier.time_since_epoch());
  if (earlier_seconds.count() < 0 &&
      later_seconds.count() > std::numeric_limits<std::int64_t>::max() + earlier_seconds.count()) {
    return std::nullopt;
  }
  auto seconds = later_seconds.count() - earlier_seconds.count();
  auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(later.time_since_epoch() - later_seconds)
          .count() -
      std::chrono::duration_cast<std::chrono::nanoseconds>(earlier.time_since_epoch() -
                                                           earlier_seconds)
          .count();
  if (nanos < 0) {
    --seconds;
    nanos += 1000000000;
  }
  if (nanos >= 1000000000) {
    if (seconds == std::numeric_limits<std::int64_t>::max()) {
      return std::nullopt;
    }
    ++seconds;
    nanos -= 1000000000;
  }
  const auto checked = target::time_point_from_parts({seconds, static_cast<std::int32_t>(nanos)});
  if (const auto* point = std::get_if<target::TimePoint>(&checked)) {
    return point->time_since_epoch();
  }
  return std::nullopt;
}
std::optional<target::TimePoint> add(target::TimePoint start, std::chrono::nanoseconds duration) {
  if (duration < std::chrono::nanoseconds::zero() || start > target::TimePoint::max() - duration) {
    return std::nullopt;
  }
  return start + duration;
}
target::Outcome<identity_clock_detail::Observation> native_observation() {
  const auto before = std::chrono::steady_clock::now();
  const auto wall = std::chrono::system_clock::now().time_since_epoch();
  const auto after = std::chrono::steady_clock::now();
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(wall);
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(wall - seconds);
  if (nanos < std::chrono::nanoseconds::zero()) {
    --seconds;
    nanos += std::chrono::seconds{1};
  }
  auto checked =
      target::time_point_from_parts({seconds.count(), static_cast<std::int32_t>(nanos.count())});
  if (const auto* point = std::get_if<target::TimePoint>(&checked)) {
    const auto resolution =
        std::max(std::chrono::nanoseconds{1}, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::system_clock::duration{1}));
    return identity_clock_detail::Observation{*point, before, after, resolution};
  }
  return unavailable();
}

} // namespace

namespace identity_clock_detail {

target::Outcome<target::SecurityClockSample> observe(State& state,
                                                     const Observation& current) noexcept {
  const auto reject = [&]() -> target::Outcome<target::SecurityClockSample> {
    state.failed = true;
    return unavailable();
  };
  constexpr auto kMaximumMeasurement = std::chrono::milliseconds{1};
  if (state.failed || !state.declared_trusted ||
      state.maximum_known_skew < std::chrono::milliseconds::zero() ||
      state.maximum_known_skew > target::kMaximumKnownClockSkew || current.after < current.before ||
      (state.last_after && current.before < *state.last_after) ||
      current.resolution < std::chrono::nanoseconds::zero() ||
      current.resolution > kMaximumMeasurement) {
    return reject();
  }
  const auto width = elapsed(current.after, current.before);
  if (!width) {
    return reject();
  }
  state.last_after = current.after;
  if (*width > kMaximumMeasurement) {
    return unavailable(); // Cannot attest this preempted measurement; do not recalibrate.
  }
  if (!state.baseline) {
    state.baseline = current;
  } else {
    const auto& baseline = *state.baseline;
    const auto minimum_elapsed = elapsed(current.before, baseline.after);
    const auto maximum_elapsed = elapsed(current.after, baseline.before);
    if (!minimum_elapsed || !maximum_elapsed) {
      return reject();
    }
    const auto minimum_utc = add(baseline.utc, *minimum_elapsed);
    const auto maximum_utc = add(baseline.utc, *maximum_elapsed);
    if (!minimum_utc || !maximum_utc) {
      return reject();
    }
    const auto tolerance =
        std::chrono::duration_cast<std::chrono::nanoseconds>(state.maximum_known_skew) +
        baseline.resolution + current.resolution;
    const auto lower = *minimum_utc < target::TimePoint::min() + tolerance
                           ? target::TimePoint::min()
                           : *minimum_utc - tolerance;
    const auto upper = *maximum_utc > target::TimePoint::max() - tolerance
                           ? target::TimePoint::max()
                           : *maximum_utc + tolerance;
    if (current.utc < lower || current.utc > upper) {
      return reject();
    }
  }
  return target::SecurityClockSample{current.utc, current.after, state.maximum_known_skew, true};
}

} // namespace identity_clock_detail

SystemSecurityClock::SystemSecurityClock(const platform::IdentityClockDeclaration& declaration)
    : state_{declaration.trusted,
             std::chrono::milliseconds{declaration.maximum_known_skew_ms},
             false,
             {},
             {}},
      failed_(!declaration.trusted || state_.maximum_known_skew > target::kMaximumKnownClockSkew) {}

SystemSecurityClock::SystemSecurityClock(const platform::IdentityClockDeclaration& declaration,
                                         platform::TlsValidity own_credentials)
    : SystemSecurityClock(declaration) {
  own_credentials_ = own_credentials;
}

target::Outcome<target::SecurityClockSample> SystemSecurityClock::sample() noexcept {
  if (failed_.load()) {
    return unavailable();
  }
  try {
    const std::lock_guard lock(mutex_);
    if (failed_.load()) {
      return unavailable();
    }
    const auto measured =
        native_observation(); // Native bounded clock reads only, no callbacks/I/O.
    if (const auto* error = std::get_if<target::Failure>(&measured)) {
      state_.failed = true;
      failed_.store(true);
      available_.store(false);
      return *error;
    }
    auto result = identity_clock_detail::observe(
        state_, std::get<identity_clock_detail::Observation>(measured));
    if (const auto* sample = std::get_if<target::SecurityClockSample>(&result);
        sample && own_credentials_ && !platform::tls_valid_at(*own_credentials_, sample->utc)) {
      credentials_expired_.store(true);
      state_.failed = true;
      result = unavailable();
    }
    available_.store(std::holds_alternative<target::SecurityClockSample>(result));
    failed_.store(state_.failed);
    return result;
  } catch (...) {
    failed_.store(true);
    available_.store(false);
    return unavailable();
  }
}
bool SystemSecurityClock::healthy() const noexcept {
  return available_.load() && !failed_.load();
}

bool SystemSecurityClock::credentials_expired() const noexcept {
  return credentials_expired_.load();
}

target::Outcome<target::TimePoint> SystemSecurityClock::invocation_deadline() noexcept {
  if (!own_credentials_) {
    return unavailable();
  }
  const auto current = sample();
  if (const auto* error = std::get_if<target::Failure>(&current)) {
    return *error;
  }
  return own_credentials_->not_after;
}

} // namespace kiarabot::core::composition
