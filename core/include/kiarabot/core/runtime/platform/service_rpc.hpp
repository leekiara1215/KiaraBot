#pragma once

#include <memory>

#include "kiarabot/core/runtime/platform/service_config.hpp"

namespace grpc {
class Service;
}

namespace kiarabot::core::platform {

// Owns gRPC server and generated service objects; no business state or Core internals.
class ServiceRpc {
public:
  explicit ServiceRpc(target::Service owner);
  ~ServiceRpc();
  ServiceRpc(const ServiceRpc&) = delete;
  ServiceRpc& operator=(const ServiceRpc&) = delete;
  // Composition-root use only, before start. Owns each adapter until server shutdown completes.
  // All adapters must belong to this process; empty registration keeps generated rejecting stubs.
  target::Status add_service(target::Service owner, std::unique_ptr<grpc::Service> service);
  // External Bridge listener only (AdapterBridge/PluginBridge). Server-verified TLS, no client
  // certificate request. Runtime-only; requires bridge_listen in start(). Registered separately
  // from the internal owner mTLS listener so the two services never share a socket.
  target::Status add_bridge_service(target::Service owner, std::unique_ptr<grpc::Service> service);
  bool start(const ProcessConfig& config);
  int port() const noexcept;
  int bridge_port() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::core::platform
