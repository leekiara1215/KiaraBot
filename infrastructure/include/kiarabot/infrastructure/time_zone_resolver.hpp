#pragma once

#include <functional>

#include "kiarabot/core/ports/ports.hpp"

namespace kiarabot::infrastructure {

// Converts a one-shot local civil time to UTC wall time through Abseil and the
// system IANA zoneinfo database.
//
// Error contract:
// - malformed/invalid civil time, unknown zone name or ambiguous/nonexistent
//   local time -> INVALID_ARGUMENT;
// - IANA zone data unavailable to the process -> UPSTREAM_UNAVAILABLE.
// Call zone_info_available() once during startup to fail fast instead of
// turning every schedule request into a dependency error.
class TimeZoneResolver final : public core::ITimeZoneResolver {
public:
  using AvailabilityProbe = std::function<bool()>;

  TimeZoneResolver();
  // Test/composition seam: overrides the zone-data availability probe.
  explicit TimeZoneResolver(AvailabilityProbe probe);

  // Probes the system zoneinfo database (not this machine's local time zone).
  static bool zone_info_available();

  core::PortResult<core::WallTime> resolve(const kiarabot::v1::LocalTaskTime& local) override;

private:
  AvailabilityProbe available_;
};
} // namespace kiarabot::infrastructure
