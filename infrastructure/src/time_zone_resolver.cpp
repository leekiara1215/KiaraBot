#include "kiarabot/infrastructure/time_zone_resolver.hpp"

#include <absl/time/time.h>

namespace kiarabot::infrastructure {
core::PortResult<core::WallTime>
TimeZoneResolver::resolve(const kiarabot::v1::LocalTaskTime& local) try {
  kiarabot::v1::Error error;
  error.set_code(kiarabot::v1::INVALID_ARGUMENT);
  error.set_message("INVALID_ARGUMENT");
  const auto& name = local.time_zone();
  if (local.year() < 1970 || local.year() > 2100 || name.empty() || name.size() > 128 ||
      name.front() == '/' || (name != "UTC" && name.find('/') == std::string::npos) ||
      !std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '/' || c == '_' || c == '-' || c == '+';
      })) {
    return error;
  }
  absl::TimeZone zone;
  if (!absl::LoadTimeZone(name, &zone)) {
    return error;
  }
  const absl::CivilSecond civil(local.year(), local.month(), local.day(), local.hour(),
                                local.minute(), local.second());
  if (civil.year() != local.year() || civil.month() != local.month() ||
      civil.day() != local.day() || civil.hour() != local.hour() ||
      civil.minute() != local.minute() || civil.second() != local.second()) {
    return error;
  }
  const auto resolved = zone.At(civil);
  if (resolved.kind != absl::TimeZone::TimeInfo::UNIQUE) {
    return error;
  }
  return core::WallTime(std::chrono::seconds(absl::ToUnixSeconds(resolved.pre)));
} catch (...) {
  kiarabot::v1::Error error;
  error.set_code(kiarabot::v1::INTERNAL);
  error.set_message("INTERNAL");
  return error;
}
} // namespace kiarabot::infrastructure
