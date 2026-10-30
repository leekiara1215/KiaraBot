#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include "kiarabot/core/authorized_commands.hpp"
#include "kiarabot/v1/adapters.pb.h"

namespace kiarabot::core {
class PluginHub;
struct AdapterPolicy {
  std::string identity;
  std::vector<std::string> accounts;
};
struct AdapterSession {
  std::string identity;
  std::string instance;
  std::vector<std::string> accounts;
};
struct AdapterRegistration {
  kiarabot::v1::RegisterAdapterResponse response;
  AdapterSession session;
};

// Trusted bridge calls this using its authenticated principal. Owns no transport
// or database. Serializes registration and ingestion; store must outlive it.
class AdapterIngress {
public:
  AdapterIngress(RuntimeStore& store, std::vector<AdapterPolicy> policies,
                 RuntimeOptions options = {}, std::shared_ptr<PluginHub> plugins = {});
  AdapterRegistration register_adapter(const std::string& principal,
                                       const kiarabot::v1::RegisterAdapterRequest& request);
  kiarabot::v1::AdapterEventAck ingest(const AdapterSession& session,
                                       const kiarabot::v1::EventEnvelope& event);
  bool current(const AdapterSession& session);
  void disconnect(const AdapterSession& session);

private:
  RuntimeStore& store_;
  const std::vector<AdapterPolicy> policies_;
  RuntimeOptions options_;
  std::shared_ptr<PluginHub> plugins_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::string> instances_;
};
} // namespace kiarabot::core
