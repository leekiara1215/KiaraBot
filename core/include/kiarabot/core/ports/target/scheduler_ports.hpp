#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/scheduler_rules.hpp"

namespace kiarabot::core::target {

// Hash exactly the supplied bytes, including embedded NULs; no framing or normalization here.
// Returns the 32 SHA-256 bytes or an existing stable Failure, never a fallback digest.
class SchedulerDigestPort {
public:
  virtual ~SchedulerDigestPort() = default;
  virtual Outcome<Sha256Digest> sha256(std::string_view value) = 0;
};

// IANA civil-time resolution port. The production adapter wraps infrastructure::TimeZoneResolver
// and keeps protobuf/Absl outside the application.
class SchedulerTimeZonePort {
public:
  virtual ~SchedulerTimeZonePort() = default;
  virtual bool available() const = 0;
  virtual Outcome<TimePoint> resolve(const LocalCivilTime& local) = 0;
};

// CSPRNG-backed operation/occurrence id allocation for the Scheduler owner.
class SchedulerIdPort {
public:
  virtual ~SchedulerIdPort() = default;
  virtual Outcome<MessageId> occurrence_id() = 0;
  virtual Outcome<OperationId> operation_id() = 0;
};

} // namespace kiarabot::core::target
