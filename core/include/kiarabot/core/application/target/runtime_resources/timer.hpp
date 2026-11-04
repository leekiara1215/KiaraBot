#pragma once

#include "kiarabot/core/application/target/runtime_resources/types.hpp"

namespace kiarabot::core::target {

// Narrow Runtime resource use cases. Each call owns its local UoW; borrowed ports outlive calls.
class RuntimeTimerApplication {
public:
  virtual ~RuntimeTimerApplication() = default;

  virtual Outcome<TimerTriggerAcceptance>
  accept_timer_trigger(const AcceptTimerTriggerRequest& request) = 0;
};

} // namespace kiarabot::core::target
