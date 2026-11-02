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

#include "kiarabot/core/application/target/scheduler_commands.hpp"
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

struct TimerDeliveryRequest {
  MessageId occurrence_id;
  std::string task_id;
  InstanceId owner_instance_id;
  ResourceVersion schedule_version;
  ConversationKey target;
  std::vector<std::byte> payload;
  HandoffContext handoff;
  std::string trace_id;
};
struct TimerDeliveryReceipt {
  MessageId event_id;
  bool duplicate = false;
  TimePoint persisted_at;
};
// Outer transport/authorization adapter that calls Runtime.AcceptTimerTrigger. Implementations must
// perform network I/O only outside a Scheduler UoW; failures are stable and retryable unless
// terminal (InvalidArgument/PermissionDenied/Conflict). Availability feeds Scheduler readiness.
class TimerDeliveryPort {
public:
  virtual ~TimerDeliveryPort() = default;
  virtual bool available() const = 0;
  virtual Outcome<TimerDeliveryReceipt> accept(const TimerDeliveryRequest& request) = 0;
};
struct SchedulerDeliverySummary {
  std::size_t accepted = 0;
  std::size_t duplicates = 0;
  std::size_t rejected = 0;
  std::size_t retried = 0;
};

struct SchedulerServiceDependencies {
  SchedulerUnitOfWorkFactory& transactions;
  SchedulerIdPort& ids;
  SchedulerTimeZonePort& timezone;
  std::chrono::hours horizon{kTaskHorizon};
  std::size_t instance_capacity{kInstanceTaskCapacity};
  std::size_t installation_capacity{kInstallationTaskCapacity};
  // Optional Runtime handoff port. Null keeps the service startable (P02/readiness=false) and
  // makes trigger delivery fail closed with CapabilityNotSupported.
  TimerDeliveryPort* delivery = nullptr;
  // Composition-root owned bundle (factory/ids/zones). The application stores it so the borrowed
  // references above stay valid for the whole service lifetime.
  std::shared_ptr<void> keep_alive;
};

// P10a Scheduler application. Owns one-shot task/occurrence rules and the durable trigger outbox
// intent; Runtime AcceptTimerTrigger wiring and plugin callback completion stay in P10b.
class SchedulerService final : public ServiceReadinessPort, public SchedulerCommandHandler {
public:
  explicit SchedulerService(SchedulerServiceDependencies dependencies);
  SchedulerService(const SchedulerService&) = delete;
  SchedulerService& operator=(const SchedulerService&) = delete;

  Service service() const noexcept override;
  ServiceReadiness readiness() const override;

  Outcome<ScheduledTask> create_task(const SchedulerCommandContext& context,
                                     const TaskCreationInput& input) override;
  Outcome<ScheduledTask> cancel_task(const SchedulerCommandContext& context,
                                     std::string_view task_id) override;
  Outcome<ScheduledTask> get_task(const SchedulerCommandContext& context,
                                  std::string_view task_id) override;
  Outcome<SchedulerListPage> list_tasks(const SchedulerCommandContext& context,
                                        std::optional<TaskState> state, std::uint32_t page_size,
                                        std::string_view page_token) override;
  Outcome<Occurrence> get_occurrence(const SchedulerCommandContext& context,
                                     std::string_view occurrence_id) override;
  Outcome<Operation> get_operation(const SchedulerCommandContext& context,
                                   const OperationRef& operation) override;

  // Maintenance/trigger handoff. The due sweep only persists intent + outbox (no Runtime/plugin
  // calls); P10b claims the outbox, calls Runtime.AcceptTimerTrigger and confirms the receipt.
  Outcome<std::size_t> due_sweep(TimePoint now, std::size_t limit);
  Outcome<std::vector<SchedulerTriggerClaim>> claim_triggers(TimePoint now, std::size_t limit,
                                                             std::chrono::seconds lease);
  Outcome<Occurrence> confirm_trigger_accepted(const OutboxClaim& claim,
                                               std::string_view accepted_event_id, TimePoint now);
  Outcome<std::size_t> expire_stale_triggers(TimePoint now, std::size_t limit);
  // Maintenance handoff: claim trigger outbox, call Runtime.AcceptTimerTrigger outside a UoW, then
  // confirm the acceptance (same occurrence idempotent) or permanently reject the occurrence.
  // Retryable Runtime failures leave the claim to expire and retry with the same occurrence/event.
  Outcome<SchedulerDeliverySummary> deliver_triggers(TimePoint now, std::size_t limit);

private:
  Outcome<Occurrence> fail_trigger(const OutboxClaim& claim, ErrorCode reason, TimePoint now);
  SchedulerServiceDependencies dependencies_;
};

} // namespace kiarabot::core::target
