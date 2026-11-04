#include "kiarabot/control/target/websocket_stream_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "kiarabot/control/target/websocket_protocol.hpp"

namespace kiarabot::control::target {
namespace {
std::string hex(std::array<unsigned char, 32> bytes, std::size_t count) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(count * 2);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(kDigits[bytes[index] >> 4U]);
    result.push_back(kDigits[bytes[index] & 0x0fU]);
  }
  return result;
}
bool same_identity(const WebSocketIdentity& left, const WebSocketIdentity& right) {
  return left.principal == right.principal && left.identity_session == right.identity_session &&
         left.authorization_version == right.authorization_version;
}
bool same_selection(const WebSocketSelection& left, const WebSocketSelection& right) {
  return left.channel == right.channel && left.owner == right.owner &&
         websocket_same_scope(left.scope, right.scope);
}
} // namespace

WebSocketStreamStore::WebSocketStreamStore(
    WebSocketRetainedLimits limits, std::function<std::chrono::steady_clock::time_point()> now)
    : limits_(limits), now_(std::move(now)) {
  if (!now_) {
    now_ = [] { return std::chrono::steady_clock::now(); };
  }
}

std::string WebSocketStreamStore::random_text(std::size_t bytes) const {
  std::array<unsigned char, 32> material{};
  const auto size = std::min(bytes, material.size());
  if (size == 0 || RAND_bytes(material.data(), static_cast<int>(size)) != 1) {
    return {};
  }
  return hex(material, size);
}

bool WebSocketStreamStore::owned(const Session& session,
                                 const std::shared_ptr<WebSocketSocketHandle>& socket) const {
  return session.socket != nullptr && socket != nullptr && session.socket == socket;
}

void WebSocketStreamStore::prune_session_locked(Session& session,
                                                std::chrono::steady_clock::time_point now,
                                                std::size_t incoming_events,
                                                std::size_t incoming_bytes) {
  const auto pop = [&] {
    if (session.events.empty()) {
      return false;
    }
    total_bytes_ -= session.events.front().json.size();
    session.bytes -= session.events.front().json.size();
    if (session.events.front().owner_cursor) {
      session.boundary_cursor = session.events.front().owner_cursor;
    }
    session.events.pop_front();
    session.replay_from =
        session.events.empty() ? session.head_sequence + 1 : session.events.front().sequence;
    return true;
  };
  // Retention is a hard bound: expired replay is gone even if the session record remains.
  while (!session.events.empty() && session.events.front().retained_at + limits_.retention <= now) {
    pop();
  }
  // Capacity bounds advance the replay boundary explicitly instead of dropping arbitrary frames.
  while (!session.events.empty() &&
         (session.events.size() + incoming_events > limits_.maximum_session_events ||
          session.bytes + incoming_bytes > limits_.maximum_session_bytes)) {
    pop();
  }
}

void WebSocketStreamStore::append_tombstone_locked(std::string session_id,
                                                   WebSocketResyncReason reason) {
  tombstones_[session_id] = reason;
  tombstone_order_.push_back(session_id);
  while (tombstone_order_.size() > limits_.maximum_tombstones) {
    tombstones_.erase(tombstone_order_.front());
    tombstone_order_.pop_front();
  }
}

void WebSocketStreamStore::evict_session_locked(
    const std::string& session_id, WebSocketResyncReason reason,
    std::vector<std::shared_ptr<WebSocketSocketHandle>>& terminated) {
  const auto found = sessions_.find(session_id);
  if (found == sessions_.end()) {
    return;
  }
  // The caller may pass a reference into the map key it is deleting, so copy the identity before
  // erasing the node and record the tombstone first.
  const std::string id = found->first;
  if (found->second.socket) {
    terminated.push_back(found->second.socket);
  }
  total_bytes_ -= found->second.bytes;
  append_tombstone_locked(id, reason);
  sessions_.erase(found);
}

void WebSocketStreamStore::prune_locked(
    std::chrono::steady_clock::time_point now,
    std::vector<std::shared_ptr<WebSocketSocketHandle>>& terminated) {
  std::vector<std::string> expired;
  for (const auto& [session_id, session] : sessions_) {
    if (!session.socket && session.last_active + limits_.retention <= now) {
      expired.push_back(session_id);
    }
  }
  for (const auto& session_id : expired) {
    evict_session_locked(session_id, WebSocketResyncReason::CursorExpired, terminated);
  }
  for (auto& [session_id, session] : sessions_) {
    static_cast<void>(session_id);
    prune_session_locked(session, now, 0, 0);
  }
}

WebSocketResult<std::string>
WebSocketStreamStore::create(const WebSocketIdentity& identity, const WebSocketSelection& selection,
                             const std::optional<v1::StreamCursor>& initial_cursor,
                             const std::shared_ptr<WebSocketSocketHandle>& socket) {
  std::vector<std::shared_ptr<WebSocketSocketHandle>> terminated;
  std::string session_id;
  {
    const std::lock_guard lock(mutex_);
    const auto now = now_();
    prune_locked(now, terminated);
    session_id = random_text(32);
    if (session_id.empty()) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
    while (sessions_.size() >= limits_.maximum_sessions) {
      const auto victim = std::find_if(sessions_.begin(), sessions_.end(),
                                       [](const auto& entry) { return !entry.second.socket; });
      if (victim != sessions_.end()) {
        evict_session_locked(victim->first, WebSocketResyncReason::CursorExpired, terminated);
        continue;
      }
      auto oldest = sessions_.begin();
      for (auto current = sessions_.begin(); current != sessions_.end(); ++current) {
        if (current->second.last_active < oldest->second.last_active) {
          oldest = current;
        }
      }
      if (oldest == sessions_.end()) {
        break;
      }
      evict_session_locked(oldest->first, WebSocketResyncReason::CursorExpired, terminated);
    }
    Session session;
    session.session_id = session_id;
    session.identity = identity;
    session.selection = selection;
    session.socket = socket;
    session.owner_cursor = initial_cursor;
    session.boundary_cursor = initial_cursor;
    session.replay_from = 1;
    session.last_active = now;
    sessions_.emplace(session_id, std::move(session));
  }
  for (const auto& handle : terminated) {
    if (handle) {
      handle->terminate(1001);
    }
  }
  return session_id;
}

WebSocketResumeResult
WebSocketStreamStore::resume(std::string_view session_id, const WebSocketIdentity& identity,
                             const WebSocketSelection& selection, std::uint64_t last_sequence,
                             const std::optional<v1::StreamCursor>& owner_cursor,
                             const std::shared_ptr<WebSocketSocketHandle>& socket) {
  WebSocketResumeResult result;
  std::vector<std::shared_ptr<WebSocketSocketHandle>> terminated;
  {
    const std::lock_guard lock(mutex_);
    const auto now = now_();
    prune_locked(now, terminated);
    const auto found = sessions_.find(std::string{session_id});
    if (found == sessions_.end()) {
      result.status = WebSocketResumeResult::Status::Resync;
      const auto tombstone = tombstones_.find(std::string{session_id});
      result.resync =
          tombstone == tombstones_.end() ? WebSocketResyncReason::ServerRestart : tombstone->second;
      return result;
    }
    auto& session = found->second;
    if (!same_identity(session.identity, identity) ||
        !same_selection(session.selection, selection)) {
      result.status = WebSocketResumeResult::Status::Rejected;
      result.failure = {v1::PERMISSION_DENIED, false};
      return result;
    }
    if (last_sequence > session.head_sequence) {
      result.status = WebSocketResumeResult::Status::Resync;
      result.resync = WebSocketResyncReason::InvalidCursor;
      return result;
    }
    if (last_sequence < session.head_sequence && last_sequence + 1 < session.replay_from) {
      result.status = WebSocketResumeResult::Status::Resync;
      result.resync = WebSocketResyncReason::CursorExpired;
      return result;
    }
    if (selection.channel == "operations" && !owner_cursor) {
      result.status = WebSocketResumeResult::Status::Resync;
      result.resync = WebSocketResyncReason::InvalidCursor;
      return result;
    }
    if (owner_cursor) {
      std::optional<v1::StreamCursor> expected;
      if (last_sequence >= session.replay_from && !session.events.empty()) {
        const auto index = static_cast<std::size_t>(last_sequence - session.replay_from);
        if (index < session.events.size()) {
          expected = session.events[index].owner_cursor;
        }
      } else {
        expected = session.boundary_cursor;
      }
      if (!expected || !websocket_same_cursor(*expected, *owner_cursor)) {
        result.status = WebSocketResumeResult::Status::Resync;
        result.resync = WebSocketResyncReason::InvalidCursor;
        return result;
      }
    }
    if (session.socket && session.socket != socket) {
      terminated.push_back(session.socket);
    }
    session.socket = socket;
    session.accepted_sequence = last_sequence;
    session.last_active = now;
    result.status = WebSocketResumeResult::Status::Accepted;
    result.accepted_sequence = last_sequence;
  }
  for (const auto& handle : terminated) {
    if (handle) {
      handle->terminate(1001);
    }
  }
  return result;
}

std::optional<std::uint64_t>
WebSocketStreamStore::append(std::string_view session_id,
                             const std::shared_ptr<WebSocketSocketHandle>& socket,
                             std::uint64_t expected_sequence, std::string json,
                             const std::optional<v1::StreamCursor>& owner_cursor) {
  if (json.empty() || json.size() > kMaximumEventBytes || expected_sequence == 0 ||
      expected_sequence > kMaximumSafeSequence) {
    return std::nullopt;
  }
  std::vector<std::shared_ptr<WebSocketSocketHandle>> terminated;
  std::optional<std::uint64_t> assigned;
  {
    const std::lock_guard lock(mutex_);
    const auto now = now_();
    prune_locked(now, terminated);
    const auto found = sessions_.find(std::string{session_id});
    if (found == sessions_.end() || !owned(found->second, socket)) {
      return std::nullopt;
    }
    auto& session = found->second;
    if (expected_sequence != session.head_sequence + 1) {
      return std::nullopt;
    }
    prune_session_locked(session, now, 1, json.size());
    // Total retained budget evicts whole inactive sessions first; only then the oldest event of
    // this stream. Eviction always advances an explicit replay boundary.
    while (total_bytes_ + json.size() > limits_.maximum_total_bytes && !sessions_.empty()) {
      std::string victim;
      bool victim_has_socket = true;
      std::chrono::steady_clock::time_point victim_active{};
      for (const auto& [other_id, other] : sessions_) {
        if (other_id == session.session_id) {
          continue;
        }
        const bool has_socket = other.socket != nullptr;
        if (victim.empty() || (victim_has_socket && !has_socket) ||
            (victim_has_socket == has_socket && other.last_active < victim_active)) {
          victim = other_id;
          victim_has_socket = has_socket;
          victim_active = other.last_active;
        }
      }
      if (victim.empty()) {
        break;
      }
      evict_session_locked(victim, WebSocketResyncReason::CursorExpired, terminated);
    }
    while (total_bytes_ + json.size() > limits_.maximum_total_bytes && !session.events.empty()) {
      total_bytes_ -= session.events.front().json.size();
      session.bytes -= session.events.front().json.size();
      session.boundary_cursor = session.events.front().owner_cursor;
      session.events.pop_front();
      session.replay_from =
          session.events.empty() ? session.head_sequence + 1 : session.events.front().sequence;
    }
    WebSocketEventRecord event;
    event.sequence = expected_sequence;
    event.json = std::move(json);
    event.owner_cursor = owner_cursor;
    event.retained_at = now;
    session.bytes += event.json.size();
    session.events.push_back(std::move(event));
    session.head_sequence = expected_sequence;
    session.owner_cursor = owner_cursor;
    session.replay_from =
        session.events.empty() ? expected_sequence : session.events.front().sequence;
    session.last_active = now;
    total_bytes_ += session.events.back().json.size();
    assigned = expected_sequence;
  }
  for (const auto& handle : terminated) {
    if (handle) {
      handle->terminate(1001);
    }
  }
  return assigned;
}

std::optional<WebSocketDelivery>
WebSocketStreamStore::next_event(std::string_view session_id,
                                 const std::shared_ptr<WebSocketSocketHandle>& socket,
                                 std::uint64_t after) {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return std::nullopt;
  }
  auto& session = found->second;
  if (after >= session.head_sequence || session.events.empty() || after + 1 < session.replay_from) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(after + 1 - session.replay_from);
  if (index >= session.events.size()) {
    return std::nullopt;
  }
  WebSocketDelivery result;
  result.sequence = session.events[index].sequence;
  result.json = session.events[index].json;
  result.owner_cursor = session.events[index].owner_cursor;
  return result;
}

bool WebSocketStreamStore::acknowledge(std::string_view session_id,
                                       const std::shared_ptr<WebSocketSocketHandle>& socket,
                                       std::uint64_t sequence) {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return false;
  }
  auto& session = found->second;
  if (sequence <= session.accepted_sequence || sequence > session.head_sequence) {
    return false;
  }
  session.accepted_sequence = sequence;
  session.last_active = now_();
  return true;
}

bool WebSocketStreamStore::set_owner_cursor(std::string_view session_id,
                                            const std::shared_ptr<WebSocketSocketHandle>& socket,
                                            const v1::StreamCursor& cursor) {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return false;
  }
  if (found->second.owner_cursor && found->second.owner_cursor->sequence() > cursor.sequence()) {
    return false;
  }
  found->second.owner_cursor = cursor;
  found->second.last_active = now_();
  return true;
}

std::optional<v1::StreamCursor>
WebSocketStreamStore::owner_cursor(std::string_view session_id,
                                   const std::shared_ptr<WebSocketSocketHandle>& socket) const {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return std::nullopt;
  }
  return found->second.owner_cursor;
}

std::optional<std::uint64_t>
WebSocketStreamStore::head(std::string_view session_id,
                           const std::shared_ptr<WebSocketSocketHandle>& socket) const {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return std::nullopt;
  }
  return found->second.head_sequence;
}

std::optional<std::uint64_t>
WebSocketStreamStore::accepted(std::string_view session_id,
                               const std::shared_ptr<WebSocketSocketHandle>& socket) const {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return std::nullopt;
  }
  return found->second.accepted_sequence;
}

WebSocketPending
WebSocketStreamStore::pending(std::string_view session_id,
                              const std::shared_ptr<WebSocketSocketHandle>& socket) const {
  WebSocketPending result;
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return result;
  }
  const auto& session = found->second;
  if (session.head_sequence > session.accepted_sequence) {
    result.frames = session.head_sequence - session.accepted_sequence;
  }
  for (const auto& event : session.events) {
    if (event.sequence > session.accepted_sequence) {
      result.bytes += event.json.size();
    }
  }
  return result;
}

void WebSocketStreamStore::detach(std::string_view session_id,
                                  const std::shared_ptr<WebSocketSocketHandle>& socket) {
  const std::lock_guard lock(mutex_);
  const auto found = sessions_.find(std::string{session_id});
  if (found == sessions_.end() || !owned(found->second, socket)) {
    return;
  }
  found->second.socket.reset();
  found->second.last_active = now_();
}

void WebSocketStreamStore::clear() {
  const std::lock_guard lock(mutex_);
  sessions_.clear();
  tombstones_.clear();
  tombstone_order_.clear();
  total_bytes_ = 0;
}

std::size_t WebSocketStreamStore::retained_sessions() const {
  const std::lock_guard lock(mutex_);
  return sessions_.size();
}

std::size_t WebSocketStreamStore::retained_bytes() const {
  const std::lock_guard lock(mutex_);
  return total_bytes_;
}

} // namespace kiarabot::control::target
