#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/runtime.grpc.pb.h>

#include "kiarabot/core/ports/target/host_maintenance.hpp"
#include "kiarabot/core/ports/target/host_registration_ports.hpp"

namespace kiarabot::core::platform {

struct HostRuntimeClientOptions {
  // Runtime owner gRPC listener (internal mTLS host:port), validated by Host service config.
  std::string endpoint;
  // Production composition reuses the Host deployment TLS material as outbound mTLS identity;
  // tests may inject a loopback channel for the wire fixture only.
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  std::chrono::milliseconds budget{2000};
  std::chrono::milliseconds maximum_budget{5000};
  // Current HostBridge registration fence from the connection owner. Every gate request must
  // carry it; a missing/unknown fence fails closed instead of sending a best-effort frame.
  std::function<target::Outcome<target::HostFence>()> fence;
};

// HOST-03 Host -> Runtime.SetHostAdmissionGate client. Owns wire conversion and transport only:
// the durable maintenance operation, monotonic gate versions and idempotency stay in the Host
// maintenance application. Failures are stable and fail-closed; a down Runtime never becomes an
// "admission closed" success, and a lost ACK is retried under the same operation identity by the
// caller (never by this adapter silently mutating local state).
class HostRuntimeClient final : public target::RuntimeAdmissionGatePort {
public:
  static target::Outcome<std::unique_ptr<HostRuntimeClient>>
  create(HostRuntimeClientOptions options);
  ~HostRuntimeClient() override;
  HostRuntimeClient(const HostRuntimeClient&) = delete;
  HostRuntimeClient& operator=(const HostRuntimeClient&) = delete;

  bool available() const;
  target::Outcome<target::HostAdmissionGateReceipt>
  set_admission(const target::HostAdmissionGateRequest& request) override;

private:
  explicit HostRuntimeClient(HostRuntimeClientOptions options);
  HostRuntimeClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<kiarabot::v1::RuntimeService::Stub> stub_;
};

} // namespace kiarabot::core::platform
