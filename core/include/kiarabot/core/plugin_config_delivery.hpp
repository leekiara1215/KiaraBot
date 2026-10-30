#pragma once

#include <mutex>

#include "kiarabot/core/ports.hpp"

namespace kiarabot::core {
struct ConfigDeliveryStatus {
  std::string state{"UNAVAILABLE"};
  std::string applied_version;
  std::optional<kiarabot::v1::Error> error;
};
// Core-owned desired-state delivery. No network or plugin callbacks under locks.
// Application evidence is scoped to a live connection, never persisted across restarts.
class PluginConfigDelivery {
public:
  PluginConfigDelivery(RuntimeStore& store, std::string encryption_key, RuntimeClock clock = {});
  void attach(const std::string& identity, const std::string& fence);
  void detach(const std::string& identity, const std::string& fence);
  PortResult<std::optional<kiarabot::v1::PluginConfig>> next(const std::string& identity,
                                                             const std::string& fence);
  PortStatus acknowledge(const std::string& identity, const std::string& fence,
                         const kiarabot::v1::PluginConfigAck& ack);
  ConfigDeliveryStatus status(const std::string& identity, const std::string& desired_version);
  bool ready(const std::string& identity, const std::string& fence);

private:
  struct Delivery {
    std::string fence;
    kiarabot::v1::PluginConfig frame;
    ConfigDeliveryStatus status{"PENDING", {}, {}};
    WallTime deadline{};
    std::string retired_id;
    std::string retired_version;
  };
  void expire(Delivery& delivery);
  RuntimeStore& store_;
  const std::string key_;
  RuntimeClock clock_;
  std::mutex mutex_;
  std::unordered_map<std::string, Delivery> active_;
};
} // namespace kiarabot::core
