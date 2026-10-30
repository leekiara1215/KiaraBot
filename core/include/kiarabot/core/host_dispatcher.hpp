#pragma once

#include "kiarabot/core/plugin_hub.hpp"

namespace kiarabot::core {
struct NamespacePolicy {
  std::size_t value_bytes{65536};
  std::size_t namespace_bytes{1048576};
  std::size_t keys{1024};
  std::size_t receipts{10000};
  std::size_t audit_records{100000};
};
// Single transaction for authorization, business data, receipt and audit. Uses
// only the authenticated session; no caller-provided namespace or database I/O.
class HostDispatcher {
public:
  explicit HostDispatcher(RuntimeStore& store, RuntimeClock clock = {},
                          NamespacePolicy policy = {});
  kiarabot::v1::HostResponse dispatch(const PluginSession& session,
                                      const kiarabot::v1::HostRequest& request);

private:
  RuntimeStore& store_;
  RuntimeClock clock_;
  NamespacePolicy policy_;
  std::mutex mutex_;
};
} // namespace kiarabot::core
