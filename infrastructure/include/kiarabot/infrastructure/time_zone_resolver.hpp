#pragma once

#include "kiarabot/core/ports.hpp"

namespace kiarabot::infrastructure {
class TimeZoneResolver final : public core::ITimeZoneResolver {
public:
  core::PortResult<core::WallTime> resolve(const kiarabot::v1::LocalTaskTime& local) override;
};
} // namespace kiarabot::infrastructure
