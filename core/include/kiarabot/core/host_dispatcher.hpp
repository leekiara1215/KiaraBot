#pragma once

#include "kiarabot/core/plugin_hub.hpp"
#include "kiarabot/core/task_scheduler.hpp"

namespace kiarabot::core {
struct NamespacePolicy {
  std::size_t value_bytes{65536};
  std::size_t namespace_bytes{1048576};
  std::size_t keys{1024};
  std::size_t receipts{10000};
  std::size_t audit_records{100000};
};
// Storage authorization, data, receipt and audit share one transaction. External
// model I/O uses separate audit transactions; tasks delegate to TaskScheduler.
// Identity always comes from the authenticated session.
class HostDispatcher {
public:
  explicit HostDispatcher(RuntimeStore& store, RuntimeClock clock = {}, NamespacePolicy policy = {},
                          std::shared_ptr<IModelProvider> model = {},
                          std::shared_ptr<TaskScheduler> scheduler = {});
  bool has_model() const {
    return model_ != nullptr;
  }
  bool has_scheduler() const {
    return scheduler_ != nullptr;
  }
  kiarabot::v1::HostResponse dispatch(const PluginSession& session,
                                      const kiarabot::v1::HostRequest& request);

private:
  kiarabot::v1::HostResponse model_chat(const PluginSession& session,
                                        const kiarabot::v1::HostRequest& request);
  std::shared_ptr<IModelProvider> model_;
  std::shared_ptr<TaskScheduler> scheduler_;
  RuntimeStore& store_;
  RuntimeClock clock_;
  NamespacePolicy policy_;
  std::mutex mutex_;
};
} // namespace kiarabot::core
