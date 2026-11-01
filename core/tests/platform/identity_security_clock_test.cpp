#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include "identity_security_clock.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace c = kiarabot::core::composition;
namespace d = c::identity_clock_detail;
using Ns = std::chrono::nanoseconds;
using Ms = std::chrono::milliseconds;
using Sec = std::chrono::seconds;
using Steady = std::chrono::steady_clock;

d::Observation observation(Ns wall, Ns before, Ns after, Ns resolution = Ns{0}) {
  return {t::TimePoint{wall}, Steady::time_point{before}, Steady::time_point{after}, resolution};
}
d::State trusted(Ms tolerance = Ms{2000}) {
  return {true, tolerance, false, {}, {}};
}
void accepted(d::State& state, const d::Observation& value) {
  const auto result = d::observe(state, value);
  assert(std::holds_alternative<t::SecurityClockSample>(result));
  const auto& sample = std::get<t::SecurityClockSample>(result);
  assert(sample.trusted && sample.utc == value.utc && sample.monotonic == value.after);
  assert(sample.maximum_known_skew == state.maximum_known_skew);
}
void refused(d::State& state, const d::Observation& value) {
  const auto result = d::observe(state, value);
  assert(std::get<t::Failure>(result).code == t::ErrorCode::UpstreamUnavailable);
}
} // namespace

int main() {
  d::State missing_declaration;
  refused(missing_declaration, observation(Sec{1000}, Sec{0}, Sec{0}));
  auto invalid = trusted(Ms{2001});
  refused(invalid, observation(Sec{1000}, Sec{0}, Sec{0}));
  invalid = trusted(Ms{-1});
  refused(invalid, observation(Sec{1000}, Sec{0}, Sec{0}));

  auto exact = trusted(Ms{0});
  accepted(exact, observation(Sec{1000}, Sec{0}, Sec{0}));
  accepted(exact, observation(Sec{1001}, Sec{1}, Sec{1}));
  refused(exact, observation(Sec{1002} + Ns{1}, Sec{2}, Sec{2}));
  assert(exact.failed);
  refused(exact, observation(Sec{1003}, Sec{3}, Sec{3})); // No automatic recalibration.

  auto forward = trusted();
  accepted(forward, observation(Sec{1000}, Sec{0}, Sec{0}));
  accepted(forward, observation(Sec{1003}, Sec{1}, Sec{1})); // Exact +2000ms boundary.
  refused(forward, observation(Sec{1004} + Ns{1}, Sec{2}, Sec{2}));
  auto backward = trusted();
  accepted(backward, observation(Sec{1000}, Sec{0}, Sec{0}));
  accepted(backward, observation(Sec{999}, Sec{1}, Sec{1}));
  refused(backward, observation(Sec{1000} - Ns{1}, Sec{2}, Sec{2}));

  auto cumulative = trusted(Ms{100});
  accepted(cumulative, observation(Sec{1000}, Sec{0}, Sec{0}));
  accepted(cumulative, observation(Sec{1001} + Ms{60}, Sec{1}, Sec{1}));
  refused(cumulative, observation(Sec{1002} + Ms{120}, Sec{2}, Sec{2})); // Not a rolling baseline.

  auto sampling = trusted(Ms{0});
  accepted(sampling, observation(Sec{1000}, Ns{0}, Ns{100}, Ns{10}));
  accepted(sampling, observation(Sec{1001} + Ns{50}, Sec{1}, Sec{1} + Ns{100}, Ns{10}));
  auto preempted = trusted();
  accepted(preempted, observation(Sec{1000}, Sec{0}, Sec{0}));
  refused(preempted, observation(Sec{1001}, Sec{1}, Sec{1} + Ms{2}));
  assert(!preempted.failed && preempted.baseline->utc == t::TimePoint{Sec{1000}});
  accepted(preempted, observation(Sec{1002}, Sec{2}, Sec{2}));
  refused(preempted, observation(Sec{1003}, Sec{1}, Sec{1})); // Steady regression latches failure.

  auto invalid_interval = trusted();
  refused(invalid_interval, observation(Sec{1000}, Ns{1}, Ns{0}));
  auto invalid_resolution = trusted();
  refused(invalid_resolution, observation(Sec{1000}, Ns{0}, Ns{0}, Ns{-1}));
  auto extreme = trusted();
  accepted(extreme, {t::TimePoint::max(), Steady::time_point{}, Steady::time_point{}, Ns{0}});
  refused(extreme,
          {t::TimePoint::max(), Steady::time_point{Sec{1}}, Steady::time_point{Sec{1}}, Ns{0}});
  auto elapsed_overflow = trusted();
  accepted(elapsed_overflow,
           {t::TimePoint{}, Steady::time_point::min(), Steady::time_point::min(), Ns{0}});
  refused(elapsed_overflow,
          {t::TimePoint{}, Steady::time_point::max(), Steady::time_point::max(), Ns{0}});

  const kiarabot::core::platform::TlsValidity validity{t::TimePoint{Sec{100}},
                                                       t::TimePoint{Sec{200}}};
  assert(!kiarabot::core::platform::tls_valid_at(validity, t::TimePoint{Sec{100} - Ns{1}}));
  assert(kiarabot::core::platform::tls_valid_at(validity, t::TimePoint{Sec{100}}));
  assert(kiarabot::core::platform::tls_valid_at(validity, t::TimePoint{Sec{200} - Ns{1}}));
  assert(!kiarabot::core::platform::tls_valid_at(validity, t::TimePoint{Sec{200}}));
  assert(!kiarabot::core::platform::tls_valid_at(validity, t::TimePoint{Sec{200} + Ns{1}}));
  assert(!kiarabot::core::platform::tls_valid_at({validity.not_after, validity.not_before},
                                                 t::TimePoint{Sec{150}}));
  c::SystemSecurityClock expired({true, 2000}, validity);
  assert(std::holds_alternative<t::Failure>(expired.sample()));
  assert(expired.credentials_expired() && !expired.healthy());
  assert(std::holds_alternative<t::Failure>(expired.sample()));

  c::SystemSecurityClock untrusted({false, 0});
  assert(!untrusted.healthy());
  assert(std::holds_alternative<t::Failure>(untrusted.sample()));
  c::SystemSecurityClock native({true, 2000});
  assert(!native.healthy());
  auto first = native.sample();
  assert(std::holds_alternative<t::SecurityClockSample>(first));
  assert(native.healthy());
  std::vector<std::thread> callers;
  for (int index = 0; index < 4; ++index) {
    callers.emplace_back([&] {
      for (int iteration = 0; iteration < 20; ++iteration) {
        const auto value = native.sample();
        if (const auto* sample = std::get_if<t::SecurityClockSample>(&value)) {
          assert(sample->trusted);
        } else {
          assert(std::get<t::Failure>(value).code == t::ErrorCode::UpstreamUnavailable);
        }
      }
    });
  }
  for (auto& caller : callers) {
    caller.join();
  }
}
