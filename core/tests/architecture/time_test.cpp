#include <cstdlib>
#include <iostream>
#include <limits>
#include <ratio>
#include <string_view>

#include "kiarabot/core/domain/target/time.hpp"

namespace target = kiarabot::core::target;
static_assert(std::ratio_equal_v<target::TimePoint::duration::period, std::nano>);
static_assert(std::numeric_limits<target::TimePoint::duration::rep>::digits == 63);
void check(bool condition, std::string_view scenario) {
  if (!condition) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
void roundtrip(target::TimestampParts parts, std::int64_t expected) {
  const auto decoded = target::time_point_from_parts(parts);
  const auto* timestamp = std::get_if<target::TimePoint>(&decoded);
  check(timestamp != nullptr, "representable timestamp must decode");
  check(timestamp->time_since_epoch().count() == expected, "epoch ns must be exact");
  check(target::time_point_to_parts(*timestamp) == parts, "seconds/nanos must round-trip exactly");
}
void rejected(target::TimestampParts parts) {
  const auto result = target::time_point_from_parts(parts);
  const auto* failure = std::get_if<target::Failure>(&result);
  check(failure && failure->code == target::ErrorCode::InvalidArgument &&
            failure->acceptance == target::Acceptance::NotAccepted,
        "invalid timestamp must reject explicitly");
}
int main() {
  roundtrip({0, 0}, 0);
  roundtrip({0, 1}, 1);
  roundtrip({0, 999}, 999); // Sub-microsecond precision must survive macOS native clock resolution.
  roundtrip({1, 123}, 1000000123);
  roundtrip({-1, 999999999}, -1);
  roundtrip({-1, 999999001}, -999);
  roundtrip({-1, 0}, -1000000000);
  roundtrip({-2, 1}, -1999999999);
  roundtrip({1793491200, 987654321}, 1793491200987654321);
  roundtrip({-9223372037, 145224192}, std::numeric_limits<std::int64_t>::min());
  roundtrip({9223372036, 854775807}, std::numeric_limits<std::int64_t>::max());
  rejected({-9223372037, 145224191});
  rejected({9223372036, 854775808});
  rejected({-9223372038, 999999999});
  rejected({9223372037, 0});
  rejected({0, -1});
  rejected({0, 1000000000});
  rejected({std::numeric_limits<std::int64_t>::min(), 0});
  rejected({std::numeric_limits<std::int64_t>::max(), 999999999});
  rejected({-62135596800, 0});         // Wire year 1 is outside this business-time representation.
  rejected({253402300799, 999999999}); // Wire year 9999 also rejects, without narrowing first.
  const auto lower = target::TimePoint{std::chrono::nanoseconds::min()};
  const auto upper = target::TimePoint{std::chrono::nanoseconds::max()};
  check(std::get<target::TimePoint>(
            target::time_point_from_parts(target::time_point_to_parts(lower))) == lower,
        "minimum internal timestamp round-trips");
  check(std::get<target::TimePoint>(
            target::time_point_from_parts(target::time_point_to_parts(upper))) == upper,
        "maximum internal timestamp round-trips");
}
