#pragma once

#include <memory>

#include "kiarabot/core/runtime/platform/service_config.hpp"

namespace kiarabot::core::platform {

// Owns gRPC server and generated service objects; no business state or Core internals.
class ServiceRpc {
public:
  ServiceRpc();
  ~ServiceRpc();
  ServiceRpc(const ServiceRpc&) = delete;
  ServiceRpc& operator=(const ServiceRpc&) = delete;
  bool start(const ProcessConfig& config);
  int port() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::core::platform
