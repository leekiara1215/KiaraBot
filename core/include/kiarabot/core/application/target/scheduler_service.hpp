#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/core/domain/target/scheduler_rules.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"
#include "kiarabot/core/ports/target/scheduler_ports.hpp"
#include "kiarabot/core/ports/target/service_readiness.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// Trusted command context derived by the RPC boundary from the authenticated delegation/peer,
// never from request fields. owner/installation are therefore not client-controlled.
struct SchedulerCommandContext {
  InstanceId owner;
  InstallationId installation;
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  std::string idempotency_key;
};

struct SchedulerListPage {
  std::vector<ScheduledTask> tasks;
  std::string next_page_token;
};

struct SchedulerTriggerClaim {
  OutboxClaim claim;
  Occurrence occurrence;
};

struct SchedulerServiceDependencies {
  SchedulerUnitOfWorkFactory& transactions;
  SchedulerIdPort& ids;
  SchedulerTimeZonePort& timezone;
  std::chrono::hours horizon{kTaskHorizon};
  std::size_t instance_capacity{kInstanceTaskCapacity};
  std::size_t installation_capacity{kInstallationTaskCapacity};
};

// P10a Scheduler application. Owns one-shot task/occurrence rules and the durable trigger outbox
// intent; Runtime AcceptTimerTrigger wiring and plugin callback completion stay in P10b.
class SchedulerService final : public ServiceReadinessPort {
public:
  explicit SchedulerService(SchedulerServiceDependencies dependencies);
  SchedulerService(const SchedulerService&) = delete;
  SchedulerService& operator=(const SchedulerService&) = delete;

  Service service() const noexcept override;
  ServiceReadiness readiness() const override;

  Outcome<ScheduledTask> create_task(const SchedulerCommandContext& context,
                                     const TaskCreationInput& input);
  Outcome<ScheduledTask> cancel_task(const SchedulerCommandContext& context,
                                     std::string_view task_id);
  Outcome<ScheduledTask> get_task(const SchedulerCommandContext& context,
                                  std::string_view task_id);
  Outcome<SchedulerListPage> list_tasks(const SchedulerCommandContext& context,
                                        std::optional<TaskState> state, std::uint32_t page_size,
                                        std::string_view page_token);
  Outcome<Occurrence> get_occurrence(const SchedulerCommandContext& context,
                                     std::string_view occurrence_id);
  Outcome<Operation> get_operation(const SchedulerCommandContext& context,
                                   const OperationRef& operation);

  // Maintenance/trigger handoff. The due sweep only persists intent + outbox (no Runtime/plugin
  // calls); P10b claims the outbox, calls Runtime.AcceptTimerTrigger and confirms the receipt.
  Outcome<std::size_t> due_sweep(TimePoint now, std::size_t limit);
  Outcome<std::vector<SchedulerTriggerClaim>> claim_triggers(TimePoint now, std::size_t limit,
                                                            std::chrono::seconds lease);
  Outcome<Occurrence> confirm_trigger_accepted(const OutboxClaim& claim,
                                               std::string_view accepted_event_id, TimePoint now);
  Outcome<std::size_t> expire_stale_triggers(TimePoint now, std::size_t limit);

private:
  SchedulerServiceDependencies dependencies_;
};

} // namespace kiarabot::core::target
