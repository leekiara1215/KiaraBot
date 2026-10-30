#pragma once

#include <mutex>

#include "kiarabot/core/authorized_commands.hpp"
#include "kiarabot/v1/plugins.pb.h"

namespace kiarabot::core {
struct PluginPolicy {
  std::string identity;
  std::vector<std::string> permissions;
};
struct PluginSession {
  std::string identity;
  std::string instance;
  std::string fence;
  std::vector<std::string> subscriptions;
  std::vector<std::string> permissions{};
};
struct PluginRegistration {
  kiarabot::v1::RegisterPluginResponse response;
  PluginSession session;
};

// Shared across adapter and plugin bridges. No transport calls under its lock.
// Store must support concurrent independent transactions and outlive this hub.
class PluginHub {
public:
  PluginHub(RuntimeStore& store, std::vector<PluginPolicy> policies, RuntimeClock clock = {});
  PluginRegistration attach(const std::string& principal,
                            const kiarabot::v1::RegisterPluginRequest& request);
  PortResult<std::optional<kiarabot::v1::EventEnvelope>> next(const PluginSession& session);
  PortStatus acknowledge(const PluginSession& session, const kiarabot::v1::PluginEventAck& ack);
  // Caller must stop/join the previous sender before detach. Failures remain
  // recoverable at next attach; the active registry is removed even on failure.
  PortStatus detach(const PluginSession& session);
  bool current(const PluginSession& session);
  bool connected(const std::string& identity);
  std::vector<std::string> targets(const RuntimeSnapshot& snapshot,
                                   const kiarabot::v1::EventEnvelope& event);

private:
  bool current_locked(const PluginSession& session) const;
  RuntimeStore& store_;
  const std::vector<PluginPolicy> policies_;
  RuntimeClock clock_;
  std::mutex mutex_;
  std::unordered_map<std::string, PluginSession> active_;
};
} // namespace kiarabot::core
