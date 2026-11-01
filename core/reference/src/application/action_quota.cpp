#include "kiarabot/core/application/action_quota.hpp"

#include <algorithm>

namespace kiarabot::core {
namespace {
std::string window_key(const std::string& kind, std::string_view scope, const std::string& value) {
  return std::to_string(kind.size()) + ":" + kind + std::to_string(scope.size()) + ":" +
         std::string(scope) + std::to_string(value.size()) + ":" + value;
}
} // namespace

ActionQuota::ActionQuota(ActionQuotaPolicy policy) : policy_(policy) {
  if (policy_.account_window <= std::chrono::seconds::zero()) {
    policy_.account_window = std::chrono::seconds(60);
  }
  if (policy_.group_window <= std::chrono::seconds::zero()) {
    policy_.group_window = std::chrono::seconds(60);
  }
  if (policy_.private_window <= std::chrono::seconds::zero()) {
    policy_.private_window = std::chrono::seconds(10);
  }
  if (policy_.max_tracked_windows == 0) {
    policy_.max_tracked_windows = 8192;
  }
}

std::size_t ActionQuota::daily_limit(const std::string& kind) const {
  const auto found = policy_.daily_limits.find(kind);
  return found == policy_.daily_limits.end() ? 0 : found->second;
}

bool ActionQuota::conversation_allowed(const std::string& kind,
                                       const std::string& conversation) const {
  const auto found = policy_.allowed_conversations.find(kind);
  if (found == policy_.allowed_conversations.end()) {
    return true;
  }
  return found->second.contains(conversation);
}

bool ActionQuota::allow(const std::string& kind, const std::string& account,
                        const std::string& conversation, bool group, RuntimeTimePoint now) {
  if (policy_.account_limit == 0 && policy_.group_limit == 0 && policy_.private_limit == 0) {
    return true;
  }
  const auto account_key = window_key(kind, "account", account);
  const auto conversation_key = window_key(kind, group ? "group" : "private", conversation);
  auto& account_window = pruned(account_key, policy_.account_window, now, conversation_key);
  auto& conversation_window = pruned(
      conversation_key, group ? policy_.group_window : policy_.private_window, now, account_key);
  const auto conversation_limit = group ? policy_.group_limit : policy_.private_limit;
  if (!fits(account_window, policy_.account_limit) ||
      !fits(conversation_window, conversation_limit)) {
    return false;
  }
  if (policy_.account_limit != 0) {
    record(account_window, now);
  }
  if (conversation_limit != 0) {
    record(conversation_window, now);
  }
  return true;
}

ActionQuota::Window& ActionQuota::pruned(const std::string& key, std::chrono::seconds span,
                                         RuntimeTimePoint now, const std::string& keep) {
  auto found = windows_.find(key);
  if (found == windows_.end()) {
    evict_one(keep);
    found = windows_.emplace(key, Window{}).first;
  }
  auto& window = found->second;
  window.touched = now;
  // Relative comparison keeps correctness if an injected clock steps backwards.
  std::erase_if(window.attempts, [&](RuntimeTimePoint when) { return when + span <= now; });
  return window;
}

bool ActionQuota::fits(const Window& window, std::size_t limit) {
  return limit == 0 || window.attempts.size() < limit;
}

void ActionQuota::record(Window& window, RuntimeTimePoint now) {
  window.attempts.push_back(now);
}

void ActionQuota::evict_one(const std::string& keep) {
  if (windows_.size() < policy_.max_tracked_windows) {
    return;
  }
  auto oldest = windows_.end();
  for (auto candidate = windows_.begin(); candidate != windows_.end(); ++candidate) {
    if (candidate->first == keep) {
      continue;
    }
    if (oldest == windows_.end() || candidate->second.touched < oldest->second.touched) {
      oldest = candidate;
    }
  }
  if (oldest != windows_.end()) {
    windows_.erase(oldest);
  }
}
} // namespace kiarabot::core
