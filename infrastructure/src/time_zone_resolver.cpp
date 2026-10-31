#include "kiarabot/infrastructure/time_zone_resolver.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <absl/time/time.h>

namespace kiarabot::infrastructure {
namespace {
// Any real IANA region probes the zoneinfo database; "UTC" may be built into
// Abseil without reading zone data and therefore is not a reliable probe.
constexpr std::string_view kProbeZone = "America/New_York";

kiarabot::v1::Error protocol_error(kiarabot::v1::ErrorCode code) {
  kiarabot::v1::Error error;
  error.set_code(code);
  error.set_message(kiarabot::v1::ErrorCode_Name(code));
  return error;
}
bool load_zone(std::string_view name, absl::TimeZone* zone) {
  return absl::LoadTimeZone(std::string(name), zone);
}
} // namespace

bool TimeZoneResolver::zone_info_available() {
  static const bool available = [] {
    absl::TimeZone zone;
    return load_zone(kProbeZone, &zone);
  }();
  return available;
}

TimeZoneResolver::TimeZoneResolver() : available_([] { return zone_info_available(); }) {}

TimeZoneResolver::TimeZoneResolver(AvailabilityProbe probe) : available_(std::move(probe)) {
  if (!available_) {
    available_ = [] { return zone_info_available(); };
  }
}

core::PortResult<core::WallTime>
TimeZoneResolver::resolve(const kiarabot::v1::LocalTaskTime& local) try {
  const auto& name = local.time_zone();
  if (local.year() < 1970 || local.year() > 2100 || name.empty() || name.size() > 128 ||
      name.front() == '/' || (name != "UTC" && name.find('/') == std::string::npos) ||
      !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '_' || c == '-' || c == '+';
      })) {
    return protocol_error(kiarabot::v1::INVALID_ARGUMENT);
  }
  const absl::CivilSecond civil(local.year(), local.month(), local.day(), local.hour(),
                                local.minute(), local.second());
  if (civil.year() != local.year() || civil.month() != local.month() ||
      civil.day() != local.day() || civil.hour() != local.hour() ||
      civil.minute() != local.minute() || civil.second() != local.second()) {
    return protocol_error(kiarabot::v1::INVALID_ARGUMENT);
  }
  if (!available_()) {
    return protocol_error(kiarabot::v1::UPSTREAM_UNAVAILABLE);
  }
  absl::TimeZone zone;
  if (!load_zone(name, &zone)) {
    return protocol_error(kiarabot::v1::INVALID_ARGUMENT);
  }
  const auto resolved = zone.At(civil);
  if (resolved.kind != absl::TimeZone::TimeInfo::UNIQUE) {
    return protocol_error(kiarabot::v1::INVALID_ARGUMENT);
  }
  return core::WallTime(std::chrono::seconds(absl::ToUnixSeconds(resolved.pre)));
} catch (...) {
  return protocol_error(kiarabot::v1::INTERNAL);
}
} // namespace kiarabot::infrastructure
