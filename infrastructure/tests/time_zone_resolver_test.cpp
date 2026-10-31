// Error-contract tests for the IANA time zone resolver:
// - valid UTC/IANA civil times convert to the expected UTC instant;
// - invalid dates, unknown zones, path-like names, DST gaps and folds are
//   INVALID_ARGUMENT;
// - when zone data is unavailable every otherwise-valid request is
//   UPSTREAM_UNAVAILABLE (never disguised as a caller error), and the startup
//   probe can be checked once to fail fast.
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

#include "kiarabot/infrastructure/time_zone_resolver.hpp"

namespace {
using kiarabot::core::WallTime;
using kiarabot::infrastructure::TimeZoneResolver;
using kiarabot::v1::LocalTaskTime;

void check(bool result, const std::source_location& location = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("Time zone assertion failed at " + std::to_string(location.line()));
  }
}
template <typename T> T value(kiarabot::core::PortResult<T> result) {
  check(std::holds_alternative<T>(result));
  return std::get<T>(std::move(result));
}
kiarabot::v1::ErrorCode code_of(const kiarabot::core::PortResult<WallTime>& result) {
  const auto* error = std::get_if<kiarabot::v1::Error>(&result);
  return error == nullptr ? kiarabot::v1::ERROR_CODE_UNSPECIFIED : error->code();
}
LocalTaskTime local(int year, int month, int day, int hour, int minute, std::string zone) {
  LocalTaskTime value;
  value.set_year(year);
  value.set_month(month);
  value.set_day(day);
  value.set_hour(hour);
  value.set_minute(minute);
  value.set_second(0);
  value.set_time_zone(std::move(zone));
  return value;
}
void valid_times() {
  TimeZoneResolver resolver;
  check(TimeZoneResolver::zone_info_available());
  check(value(resolver.resolve(local(2026, 1, 1, 0, 0, "UTC"))) ==
        WallTime(std::chrono::seconds(1767225600)));
  check(value(resolver.resolve(local(2026, 1, 1, 8, 0, "Asia/Shanghai"))) ==
        WallTime(std::chrono::seconds(1767225600)));
  check(value(resolver.resolve(local(2026, 7, 1, 0, 0, "Europe/Berlin"))).time_since_epoch() ==
        std::chrono::seconds(1782856800));
}
void rejected_times() {
  TimeZoneResolver resolver;
  // DST gap and fold in America/New_York.
  check(code_of(resolver.resolve(local(2026, 3, 8, 2, 30, "America/New_York"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 11, 1, 1, 30, "America/New_York"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 2, 30, 0, 0, "UTC"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(1969, 12, 31, 23, 59, "UTC"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2101, 1, 1, 0, 0, "UTC"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 1, 1, 0, 0, "../etc/passwd"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 1, 1, 0, 0, "/etc/passwd"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 1, 1, 0, 0, "UTC+8"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 1, 1, 0, 0, "Unknown/Zone"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
  check(code_of(resolver.resolve(local(2026, 1, 1, 0, 0, std::string(129, 'a')))) ==
        kiarabot::v1::INVALID_ARGUMENT);
}
void unavailable_zone_data() {
  TimeZoneResolver resolver([] { return false; });
  check(code_of(resolver.resolve(local(2026, 1, 1, 8, 0, "Asia/Shanghai"))) ==
        kiarabot::v1::UPSTREAM_UNAVAILABLE);
  check(code_of(resolver.resolve(local(2026, 1, 1, 0, 0, "Unknown/Zone"))) ==
        kiarabot::v1::UPSTREAM_UNAVAILABLE);
  // Caller errors stay INVALID_ARGUMENT even when the dependency is down.
  check(code_of(resolver.resolve(local(2026, 2, 30, 0, 0, "UTC"))) ==
        kiarabot::v1::INVALID_ARGUMENT);
}
} // namespace

int main() {
  try {
    valid_times();
    rejected_times();
    unavailable_zone_data();
    std::cout << "time zone valid, invalid, DST and unavailable-data paths passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
