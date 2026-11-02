#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/scheduler_rules.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"
#include "kiarabot/core/ports/target/scheduler_task.hpp"

namespace kiarabot::core::target {

struct RuntimeTaskApplicationDependencies {
  // Optional P10k Scheduler task port. Null keeps the PluginBridge task frames fail-closed with
  // UPSTREAM_UNAVAILABLE; the composition installs a fail-closed client until Runtime delegation
  // acquisition for Scheduler is confirmed.
  SchedulerTaskPort* scheduler = nullptr;
  std::size_t maximum_payload_bytes = 64 * 1024;
};

// Runtime-side task use cases consumed by the authenticated PluginBridge ingress. This class does
// not trust client owner fields: the bridge builds the context from its authenticated session.
class RuntimeTaskApplication {
public:
  explicit RuntimeTaskApplication(RuntimeTaskApplicationDependencies dependencies);

  Outcome<ScheduledTask> create_task(const SchedulerTaskContext& context, TaskCreationInput input);
  Outcome<ScheduledTask> get_task(const SchedulerTaskContext& context, std::string_view task_id);
  Outcome<ScheduledTask> cancel_task(const SchedulerTaskContext& context, std::string_view task_id);

private:
  Outcome<SchedulerTaskPort*> port(const SchedulerTaskContext& context);
  RuntimeTaskApplicationDependencies dependencies_;
};

} // namespace kiarabot::core::target
