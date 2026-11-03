#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/scheduler.grpc.pb.h>

#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/ports/target/scheduler_task.hpp"

namespace kiarabot::core::platform {

struct SchedulerTaskClientOptions {
  std::string endpoint;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  // Runtime's current delegation proof for audience=scheduler (plugin task path). P05b-class
  // delegation acquisition is not yet confirmed; production composition installs a fail-closed
  // provider until it is. The P13h-2 lifecycle client instead sets platform_identity below.
  using DelegationProvider = std::function<target::Outcome<std::string>(
      const target::SchedulerTaskContext&, target::Permission)>;
  DelegationProvider delegation_provider;
  std::chrono::milliseconds maximum_budget{5000};
  // P13h-2: true selects the Runtime mTLS platform identity path (no user delegation) used only
  // for ListTasks/CancelTask by the lifecycle participant. Scheduler P13h-1 authorizes exactly
  // these two methods from the Runtime platform peer + explicit RESOURCE_INSTANCE scope; false
  // keeps the existing delegated plugin task path unchanged.
  bool platform_identity = false;
};

// Runtime -> Scheduler typed client for the three plugin task operations. Wire conversion only;
// owner/installation/idempotency authority stays in RuntimeTaskApplication/RuntimePluginBridge.
class SchedulerTaskClient final : public target::SchedulerTaskPort {
public:
  static target::Outcome<std::unique_ptr<SchedulerTaskClient>>
  create(SchedulerTaskClientOptions options);
  ~SchedulerTaskClient() override;
  SchedulerTaskClient(const SchedulerTaskClient&) = delete;
  SchedulerTaskClient& operator=(const SchedulerTaskClient&) = delete;

  bool available() const override;
  target::Outcome<target::ScheduledTask>
  create_task(const target::SchedulerTaskContext& context,
              const target::TaskCreationInput& input) override;
  target::Outcome<target::ScheduledTask> get_task(const target::SchedulerTaskContext& context,
                                                  std::string_view task_id) override;
  target::Outcome<target::ScheduledTask> cancel_task(const target::SchedulerTaskContext& context,
                                                     std::string_view task_id) override;
  target::Outcome<std::vector<target::ScheduledTask>>
  list_tasks(const target::SchedulerTaskContext& context, std::optional<target::TaskState> state,
             std::size_t page_size) override;

private:
  explicit SchedulerTaskClient(SchedulerTaskClientOptions options);
  SchedulerTaskClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<kiarabot::v1::SchedulerService::Stub> stub_;
};

// Fail-closed provider used until Runtime delegation acquisition for Scheduler exists.
SchedulerTaskClientOptions::DelegationProvider unavailable_scheduler_task_delegation();

} // namespace kiarabot::core::platform
