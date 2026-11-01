#pragma once

#include <algorithm>
#include <limits>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

inline Failure rejected(ErrorCode code, Recovery recovery = Recovery::ReadCurrent) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
inline bool valid_fence(const Fence& fence) {
  return !fence.instance.value.empty() && !fence.runtime_epoch.value.empty() &&
         !fence.execution.value.empty() && !fence.connection.value.empty() &&
         fence.execution_generation.value != 0 && fence.connection_generation.value != 0;
}
inline Status require_current_fence(const Fence& supplied, const Fence& current) {
  if (!valid_fence(supplied) || !valid_fence(current)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (supplied != current) {
    return rejected(ErrorCode::Conflict);
  }
  return std::monostate{};
}
// Only used for update. Create-only absence is a separate Repository condition.
inline Outcome<ResourceVersion> next_version(ResourceVersion current, ResourceVersion expected) {
  if (current.value == 0 || expected.value == 0) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (current != expected) {
    return rejected(ErrorCode::Conflict);
  }
  if (current.value == std::numeric_limits<std::uint64_t>::max()) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  return ResourceVersion{current.value + 1};
}

enum class AttemptState { Active, Acknowledged, Cancelled, Expired };
enum class Acknowledgement { Applied, Duplicate };
// A transaction-owned value, no locks or clocks. Caller supplies authoritative database UTC.
class AttemptLease {
public:
  static Outcome<AttemptLease> start(AttemptId id, Fence fence, TimePoint now) {
    if (id.value.empty() || !valid_fence(fence)) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    return AttemptLease{std::move(id), std::move(fence), now};
  }
  Outcome<TimePoint> renew(const AttemptId& id, const Fence& supplied, const Fence& current,
                           TimePoint now) {
    if (const auto failure = validate(id, supplied, current, now)) {
      return *failure;
    }
    if (state_ != AttemptState::Active || now >= lease_until_) {
      return rejected(ErrorCode::Conflict);
    }
    lease_until_ =
        std::min(now + std::chrono::seconds{30}, started_at_ + std::chrono::seconds{300});
    return lease_until_;
  }
  Outcome<Acknowledgement> acknowledge(const AttemptId& id, const Fence& supplied,
                                       const Fence& current, TimePoint now) {
    if (const auto failure = validate(id, supplied, current, now)) {
      return *failure;
    }
    if (state_ == AttemptState::Acknowledged) {
      return Acknowledgement::Duplicate;
    }
    if (state_ != AttemptState::Active || now >= lease_until_) {
      return rejected(ErrorCode::Conflict);
    }
    state_ = AttemptState::Acknowledged;
    return Acknowledgement::Applied;
  }
  bool expire(TimePoint now) {
    if (state_ != AttemptState::Active || now < lease_until_) {
      return false;
    }
    state_ = AttemptState::Expired;
    return true;
  }
  bool cancel() {
    if (state_ != AttemptState::Active) {
      return false;
    }
    state_ = AttemptState::Cancelled;
    return true;
  }
  TimePoint lease_until() const {
    return lease_until_;
  }
  AttemptState state() const {
    return state_;
  }

private:
  AttemptLease(AttemptId id, Fence fence, TimePoint now)
      : id_(std::move(id)), fence_(std::move(fence)), started_at_(now),
        lease_until_(now + std::chrono::seconds{30}) {}
  std::optional<Failure> validate(const AttemptId& id, const Fence& supplied, const Fence& current,
                                  TimePoint now) const {
    const auto checked = require_current_fence(supplied, current);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    if (id != id_ || supplied != fence_) {
      return rejected(ErrorCode::Conflict);
    }
    if (now < started_at_) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    return std::nullopt;
  }
  AttemptId id_;
  Fence fence_;
  TimePoint started_at_;
  TimePoint lease_until_;
  AttemptState state_ = AttemptState::Active;
};

} // namespace kiarabot::core::target
