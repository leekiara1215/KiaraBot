#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/scheduler_rules.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

struct SchedulerCommandContext;
struct SchedulerListPage;

// Transport-facing command surface of the Scheduler application. The gRPC adapter depends only on
// this interface so wire conversion/policy can be tested without repositories or PostgreSQL.
class SchedulerCommandHandler {
public:
  virtual ~SchedulerCommandHandler() = default;
  virtual Outcome<ScheduledTask> create_task(const SchedulerCommandContext& context,
                                             const TaskCreationInput& input) = 0;
  virtual Outcome<ScheduledTask> cancel_task(const SchedulerCommandContext& context,
                                             std::string_view task_id) = 0;
  virtual Outcome<ScheduledTask> get_task(const SchedulerCommandContext& context,
                                          std::string_view task_id) = 0;
  virtual Outcome<SchedulerListPage> list_tasks(const SchedulerCommandContext& context,
                                                std::optional<TaskState> state,
                                                std::uint32_t page_size,
                                                std::string_view page_token) = 0;
  virtual Outcome<Occurrence> get_occurrence(const SchedulerCommandContext& context,
                                             std::string_view occurrence_id) = 0;
  virtual Outcome<Operation> get_operation(const SchedulerCommandContext& context,
                                           const OperationRef& operation) = 0;
};

} // namespace kiarabot::core::target
