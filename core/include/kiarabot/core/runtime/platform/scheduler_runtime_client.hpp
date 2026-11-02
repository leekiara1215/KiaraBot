#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/runtime.grpc.pb.h>

#include "kiarabot/core/application/target/scheduler_service.hpp"

namespace kiarabot::core::platform {

struct SchedulerRuntimeClientOptions {
  // Runtime owner gRPC listener (internal mTLS host:port), validated by the service config.
  std::string endpoint;
  // Production composition reuses the Scheduler service TLS material as outbound mTLS identity;
  // tests may inject insecure credentials for a loopback fake Runtime.
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  std::chrono::milliseconds budget{5000};
};

// P10b Scheduler -> Runtime.AcceptTimerTrigger client. It owns wire conversion only; retry,
// occurrence idempotency and COMPLETED state remain in SchedulerService. Availability reflects the
// last transport observation and feeds readiness; a down Runtime never fabricates acceptance.
class SchedulerRuntimeClient final : public target::TimerDeliveryPort {
public:
  static target::Outcome<std::unique_ptr<SchedulerRuntimeClient>>
  create(SchedulerRuntimeClientOptions options);
  ~SchedulerRuntimeClient() override;
  SchedulerRuntimeClient(const SchedulerRuntimeClient&) = delete;
  SchedulerRuntimeClient& operator=(const SchedulerRuntimeClient&) = delete;

  bool available() const override;
  target::Outcome<target::TimerDeliveryReceipt>
  accept(const target::TimerDeliveryRequest& request) override;

private:
  explicit SchedulerRuntimeClient(SchedulerRuntimeClientOptions options);
  SchedulerRuntimeClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<kiarabot::v1::RuntimeService::Stub> stub_;
  std::atomic<bool> available_{false};
};

} // namespace kiarabot::core::platform
