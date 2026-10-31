#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kiarabot/core/domain/runtime_types.hpp"

namespace kiarabot::core {
// Outbound quota for plugin Actions. Limits are deployment policy injected by
// the composition root; the defaults are the starting points recorded in
// doc/17-account-ban-risk.md and must be tuned against measured platform
// behavior.
struct ActionQuotaPolicy {
  std::size_t account_limit{10};
  std::chrono::seconds account_window{60};
  std::size_t group_limit{3};
  std::chrono::seconds group_window{60};
  std::size_t private_limit{1};
  std::chrono::seconds private_window{10};
  // Bounds process memory for long-lived deployments. When a new conversation
  // arrives at capacity the least recently touched window is evicted.
  std::size_t max_tracked_windows{8192};
  // Persistent per-UTC-day limits keyed by action kind (the adapter capability
  // name). An absent or zero entry means no daily budget for that kind.
  std::unordered_map<std::string, std::size_t> daily_limits;
  // Optional conversation allowlist for dangerous action kinds. A kind that
  // appears here may only target the listed conversations; unlisted attempts
  // are denied. Kinds without an entry are not restricted by this policy.
  std::unordered_map<std::string, std::unordered_set<std::string>> allowed_conversations;
};

// Process-local sliding-window quota for platform-bound Actions, keyed by
// (action kind, account) and (action kind, conversation). A zero limit disables
// that rule; rejected attempts do not consume quota. Windows are not persisted,
// so a restart resets them. Callers serialize access; ActionRouter already
// holds its submission lock.
class ActionQuota {
public:
  explicit ActionQuota(ActionQuotaPolicy policy = {});
  // Records one attempt when every applicable window allows it.
  bool allow(const std::string& kind, const std::string& account, const std::string& conversation,
             bool group, RuntimeTimePoint now);
  // Returns the configured persistent daily limit for a kind (0 = unlimited).
  std::size_t daily_limit(const std::string& kind) const;
  // Returns whether a target conversation is permitted for the kind.
  bool conversation_allowed(const std::string& kind, const std::string& conversation) const;

private:
  struct Window {
    std::vector<RuntimeTimePoint> attempts;
    RuntimeTimePoint touched{};
  };
  Window& pruned(const std::string& key, std::chrono::seconds span, RuntimeTimePoint now,
                 const std::string& keep);
  static bool fits(const Window& window, std::size_t limit);
  static void record(Window& window, RuntimeTimePoint now);
  void evict_one(const std::string& keep);
  ActionQuotaPolicy policy_;
  std::unordered_map<std::string, Window> windows_;
};
} // namespace kiarabot::core
