#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/scheduler.grpc.pb.h>

#include "kiarabot/core/ports/target/scheduler_task.hpp"

namespace kiarabot::core::platform {

struct SchedulerTaskClientOptions {
  std::string endpoint;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  // Runtime's current delegation proof for audience=scheduler. P05b-class delegation acquisition
  // is not yet confirmed; production composition installs a fail-closed provider until it is.
  std::function<target::Outcome<std::string>()> delegation_provider;
  std::chrono::milliseconds maximum_budget{5000};
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

private:
  explicit SchedulerTaskClient(SchedulerTaskClientOptions options);
  SchedulerTaskClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<kiarabot::v1::SchedulerService::Stub> stub_;
};

// Fail-closed provider used until Runtime delegation acquisition for Scheduler exists.
std::function<target::Outcome<std::string>()> unavailable_scheduler_task_delegation();

} // namespace kiarabot::core::platform
