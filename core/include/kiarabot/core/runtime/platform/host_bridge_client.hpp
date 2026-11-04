#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/domain/target/host_records.hpp"
#include "kiarabot/core/ports/target/host_ports.hpp"
#include "kiarabot/core/ports/target/host_registration_ports.hpp"

namespace kiarabot::core::platform {

struct HostBridgeClientConfig {
  std::string endpoint;
  core::target::HostIdentity identity;
  // Production: grpc::SslCredentials with Host leaf+key and Runtime CA (internal mTLS).
  // Tests may inject InsecureChannelCredentials for the framing fixture only.
  std::shared_ptr<grpc::ChannelCredentials> credentials;
  std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{5}};
  std::chrono::milliseconds registration_timeout{std::chrono::seconds{5}};
};

// gRPC HostBridge client. Registers the Host on the Runtime internal mTLS listener, owns the
// 5s heartbeat, validates the Runtime-assigned connection fence on every inbound frame and
// routes apply/stop/drain commands to the HostBridgeHandler. No unary HostService listener.
class HostBridgeClient final : public core::target::HostBridgeTransport {
public:
  explicit HostBridgeClient(HostBridgeClientConfig config);
  ~HostBridgeClient() override;
  HostBridgeClient(const HostBridgeClient&) = delete;
  HostBridgeClient& operator=(const HostBridgeClient&) = delete;

  core::target::Status start(core::target::HostBridgeHandler& handler) override;
  void stop() override;
  bool registered() const override;
  core::target::Status
  send_observation(const core::target::ExecutionObservation& observation) override;
  core::target::Status send_receipt(const core::target::HostReceipt& receipt) override;
  std::uint64_t delivered_sequence() const override;
  // Current registration fence for Host->Runtime coordination calls (admission gate). Absent
  // until this connection has been accepted by Runtime; callers must fail closed, never invent it.
  core::target::Outcome<core::target::HostFence> current_fence() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::core::platform
