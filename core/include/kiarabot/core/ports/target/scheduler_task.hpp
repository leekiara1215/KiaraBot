#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/scheduler_rules.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

// Trusted command context for Runtime -> Scheduler task operations. owner/installation are
// derived from the authenticated PluginBridge session by Runtime; clients never supply them.
struct SchedulerTaskContext {
  InstanceId owner;
  InstallationId installation;
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  std::string idempotency_key;
};

// P10k outer port. The production composition currently installs a fail-closed provider because
// Runtime delegation acquisition for audience=scheduler is not yet confirmed (P05b-class gap).
// Tests inject a fake implementation; null/unavailable never fabricates a task result.
class SchedulerTaskPort {
public:
  virtual ~SchedulerTaskPort() = default;
  virtual bool available() const = 0;
  virtual Outcome<ScheduledTask> create_task(const SchedulerTaskContext& context,
                                             const TaskCreationInput& input) = 0;
  virtual Outcome<ScheduledTask> get_task(const SchedulerTaskContext& context,
                                          std::string_view task_id) = 0;
  virtual Outcome<ScheduledTask> cancel_task(const SchedulerTaskContext& context,
                                             std::string_view task_id) = 0;
  // P13h-2 lifecycle read path. Only the Runtime lifecycle participant uses this with the
  // explicit RESOURCE_INSTANCE scope; plugin task ingress keeps the three typed operations above.
  // Default is fail-closed so partial adapters never observe an empty successful task list.
  virtual Outcome<std::vector<ScheduledTask>> list_tasks(const SchedulerTaskContext&,
                                                         std::optional<TaskState>, std::size_t) {
    return Failure{ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
                   std::nullopt, std::nullopt};
  }
};

} // namespace kiarabot::core::target
