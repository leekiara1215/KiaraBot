#include <cstdlib>
#include <iostream>
#include <limits>

#include "kiarabot/core/domain/target/invariants.hpp"

namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
template <typename Value> bool conflict(const target::Outcome<Value>& result) {
  const auto* failure = std::get_if<target::Failure>(&result);
  return failure != nullptr && failure->code == target::ErrorCode::Conflict;
}
int main() {
  const target::TimePoint start{};
  const target::Fence fence{{"instance"}, {"epoch"}, {"run"}, {1}, {"connection"}, {1}};
  auto newer = fence;
  newer.connection_generation.value = 2;
  check(conflict(target::require_current_fence(fence, newer)), "old connection rejected");
  newer = fence;
  newer.runtime_epoch = {"new-epoch"};
  check(conflict(target::require_current_fence(fence, newer)), "old runtime epoch rejected");
  newer = fence;
  newer.instance = {"other-instance"};
  check(conflict(target::require_current_fence(fence, newer)), "cross instance fence rejected");
  newer = fence;
  newer.execution_generation.value = 2;
  check(conflict(target::require_current_fence(fence, newer)), "old execution rejected");
  check(std::holds_alternative<target::Failure>(target::require_current_fence({}, {})),
        "missing generation never accepted");
  check(std::get<target::ResourceVersion>(target::next_version({2}, {2})).value == 3,
        "matched version advances");
  check(conflict(target::next_version({2}, {1})), "stale write conflicts");
  const auto max = std::numeric_limits<std::uint64_t>::max();
  check(std::holds_alternative<target::Failure>(target::next_version({max}, {max})),
        "version must not wrap");
  auto lease =
      std::get<target::AttemptLease>(target::AttemptLease::start({"attempt"}, fence, start));
  check(conflict(lease.acknowledge({"old-attempt"}, fence, fence, start)),
        "old attempt cannot acknowledge current");
  check(conflict(lease.renew({"attempt"}, fence, newer, start + 10s)),
        "connection replacement rejects renewal");
  check(lease.lease_until() == start + 30s, "rejected renewal leaves lease unchanged");
  for (int seconds = 10; seconds < 300; seconds += 10) {
    const auto result =
        lease.renew({"attempt"}, fence, fence, start + std::chrono::seconds{seconds});
    check(std::holds_alternative<target::TimePoint>(result), "live attempt renews");
    check(lease.lease_until() <= start + 300s, "renewal cannot exceed total cap");
  }
  check(lease.lease_until() == start + 300s, "renewal reaches total cap");
  check(conflict(lease.renew({"attempt"}, fence, fence, start + 300s)), "expiry boundary denies");
  check(lease.expire(start + 300s), "timeout expires active attempt");
  check(conflict(lease.acknowledge({"attempt"}, fence, fence, start + 300s)),
        "late acknowledgement cannot revive expired attempt");
  auto acked =
      std::get<target::AttemptLease>(target::AttemptLease::start({"second"}, fence, start));
  check(std::get<target::Acknowledgement>(acked.acknowledge(
            {"second"}, fence, fence, start + 1s)) == target::Acknowledgement::Applied,
        "live acknowledgement succeeds");
  check(!acked.expire(start + 31s), "timeout cannot overwrite acknowledged state");
  check(std::get<target::Acknowledgement>(acked.acknowledge(
            {"second"}, fence, fence, start + 31s)) == target::Acknowledgement::Duplicate,
        "successful acknowledgement replays");
  auto cancelled =
      std::get<target::AttemptLease>(target::AttemptLease::start({"third"}, fence, start));
  check(cancelled.cancel(), "active attempt cancels");
  check(conflict(cancelled.renew({"third"}, fence, fence, start + 1s)), "cancelled cannot renew");
  auto expired =
      std::get<target::AttemptLease>(target::AttemptLease::start({"fourth"}, fence, start));
  check(conflict(expired.renew({"fourth"}, fence, fence, start + 31s)),
        "expired lease cannot renew even before timeout worker runs");
  check(expired.lease_until() == start + 30s, "expired renewal has no mutation");
  return 0;
}
