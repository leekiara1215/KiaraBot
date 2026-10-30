#pragma once

#include <mutex>
#include <unordered_set>

#include "kiarabot/core/adapter_ingress.hpp"
#include "kiarabot/core/plugin_hub.hpp"

namespace kiarabot::core {
// Authenticated bridges supply sessions; no network I/O under this lock. Store
// outlives router and supports concurrent transactions. Dispatch commits first.
class ActionRouter {
public:
  explicit ActionRouter(RuntimeStore& store, RuntimeClock clock = {});
  void attach(const AdapterSession& session, const kiarabot::v1::RegisterAdapterRequest& request);
  void detach(const AdapterSession& session);
  kiarabot::v1::PluginActionResponse submit(const PluginSession& session,
                                            const kiarabot::v1::PluginActionRequest& request);
  kiarabot::v1::PluginActionResponse query(const PluginSession& session,
                                           const kiarabot::v1::PluginActionQuery& request);
  kiarabot::v1::PluginActionResponse cancel(const PluginSession& session,
                                            const kiarabot::v1::PluginActionCancel& request);
  PortResult<std::optional<kiarabot::v1::ActionEnvelope>> next(const AdapterSession& session);
  PortStatus complete(const AdapterSession& session, const kiarabot::v1::ActionResult& result);

private:
  struct AdapterRoute {
    AdapterSession session;
    bool text{false};
    std::unordered_set<std::string> sent;
  };
  RuntimeTimePoint now() const;
  PortResult<kiarabot::v1::ActionResult> lookup(const std::string& owner,
                                                const std::string& selector, bool by_key);
  RuntimeStore& store_;
  RuntimeClock clock_;
  std::mutex mutex_;
  std::unordered_map<std::string, AdapterRoute> adapters_;
};
} // namespace kiarabot::core
