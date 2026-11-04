#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/control/target/websocket_types.hpp"

namespace kiarabot::control::target {

// Frozen target limits (api/asyncapi.yaml x-control-session-policy). A retained session is memory
// only: Control owns no stream database and sessions never survive a Control process.
struct WebSocketRetainedLimits {
  std::size_t maximum_sessions = 1024;
  std::size_t maximum_session_events = 4096;
  std::size_t maximum_session_bytes = 4 * 1024 * 1024;
  std::size_t maximum_total_bytes = 64 * 1024 * 1024;
  std::size_t maximum_tombstones = 4096;
  std::chrono::seconds retention{600};
};

// The active socket a retained session is currently attached to. Implemented by the transport
// session; terminate() is thread-safe and must only schedule a bounded close (1001/1013/...).
class WebSocketSocketHandle {
public:
  virtual ~WebSocketSocketHandle() = default;
  virtual void terminate(std::uint16_t close_code) = 0;
};

struct WebSocketResumeResult {
  enum class Status { Accepted, Rejected, Resync };
  Status status = Status::Rejected;
  WebSocketFailure failure{};
  WebSocketResyncReason resync = WebSocketResyncReason::InvalidCursor;
  std::uint64_t accepted_sequence = 0;
};

struct WebSocketDelivery {
  std::uint64_t sequence = 0;
  std::string json;
  std::optional<v1::StreamCursor> owner_cursor;
};

struct WebSocketPending {
  std::uint64_t frames = 0;
  std::size_t bytes = 0;
};

// Bounded retained delivery store. One active socket per session; a new socket attaches by
// terminating the previous one. Every operation is an atomic decision against the current
// replay boundary: an unknown/evicted session is never silently treated as an empty stream.
// Thread-safe; no lock is held while calling a socket handle.
class WebSocketStreamStore {
public:
  explicit WebSocketStreamStore(WebSocketRetainedLimits limits = {},
                                std::function<std::chrono::steady_clock::time_point()> now = {});
  WebSocketStreamStore(const WebSocketStreamStore&) = delete;
  WebSocketStreamStore& operator=(const WebSocketStreamStore&) = delete;

  // New authenticated stream. initial_cursor is the owner committed head captured BEFORE the
  // client reads its REST snapshot; operations sessions require it.
  WebSocketResult<std::string> create(const WebSocketIdentity& identity,
                                      const WebSocketSelection& selection,
                                      const std::optional<v1::StreamCursor>& initial_cursor,
                                      const std::shared_ptr<WebSocketSocketHandle>& socket);
  // Reconnect of the same binding. Rejects a binding mismatch (4403), a locally impossible
  // boundary (INVALID_CURSOR), and a lost retained replay window (CURSOR_EXPIRED); an unknown
  // session means this Control process never created it (SERVER_RESTART).
  WebSocketResumeResult resume(std::string_view session_id, const WebSocketIdentity& identity,
                               const WebSocketSelection& selection, std::uint64_t last_sequence,
                               const std::optional<v1::StreamCursor>& owner_cursor,
                               const std::shared_ptr<WebSocketSocketHandle>& socket);
  // Appends one already-rendered event at expected_sequence (the current head + 1). Returns the
  // assigned sequence, or nullopt when the socket no longer owns the session.
  std::optional<std::uint64_t> append(std::string_view session_id,
                                      const std::shared_ptr<WebSocketSocketHandle>& socket,
                                      std::uint64_t expected_sequence, std::string json,
                                      const std::optional<v1::StreamCursor>& owner_cursor);
  // Oldest event with sequence > after (the single writer's next frame).
  std::optional<WebSocketDelivery> next_event(std::string_view session_id,
                                              const std::shared_ptr<WebSocketSocketHandle>& socket,
                                              std::uint64_t after);
  // Advances the client-visible accepted boundary only after a write completed.
  bool acknowledge(std::string_view session_id,
                   const std::shared_ptr<WebSocketSocketHandle>& socket, std::uint64_t sequence);
  // Full committed owner page boundary; only called after the entire page was appended.
  bool set_owner_cursor(std::string_view session_id,
                        const std::shared_ptr<WebSocketSocketHandle>& socket,
                        const v1::StreamCursor& cursor);
  std::optional<v1::StreamCursor>
  owner_cursor(std::string_view session_id,
               const std::shared_ptr<WebSocketSocketHandle>& socket) const;
  std::optional<std::uint64_t> head(std::string_view session_id,
                                    const std::shared_ptr<WebSocketSocketHandle>& socket) const;
  std::optional<std::uint64_t> accepted(std::string_view session_id,
                                        const std::shared_ptr<WebSocketSocketHandle>& socket) const;
  WebSocketPending pending(std::string_view session_id,
                           const std::shared_ptr<WebSocketSocketHandle>& socket) const;
  void detach(std::string_view session_id, const std::shared_ptr<WebSocketSocketHandle>& socket);
  void clear();
  std::size_t retained_sessions() const;
  std::size_t retained_bytes() const;

private:
  struct Session {
    std::string session_id;
    WebSocketIdentity identity;
    WebSocketSelection selection;
    std::shared_ptr<WebSocketSocketHandle> socket;
    std::uint64_t accepted_sequence = 0;
    std::uint64_t head_sequence = 0;
    // First retained event sequence; head+1 when no replay event is retained.
    std::uint64_t replay_from = 1;
    std::optional<v1::StreamCursor> owner_cursor;
    std::optional<v1::StreamCursor> boundary_cursor;
    std::deque<WebSocketEventRecord> events;
    std::size_t bytes = 0;
    std::chrono::steady_clock::time_point last_active{};
  };
  bool owned(const Session& session, const std::shared_ptr<WebSocketSocketHandle>& socket) const;
  void prune_locked(std::chrono::steady_clock::time_point now,
                    std::vector<std::shared_ptr<WebSocketSocketHandle>>& terminated);
  void prune_session_locked(Session& session, std::chrono::steady_clock::time_point now,
                            std::size_t incoming_events, std::size_t incoming_bytes);
  void evict_session_locked(const std::string& session_id, WebSocketResyncReason reason,
                            std::vector<std::shared_ptr<WebSocketSocketHandle>>& terminated);
  void append_tombstone_locked(std::string session_id, WebSocketResyncReason reason);
  std::string random_text(std::size_t bytes) const;

  WebSocketRetainedLimits limits_;
  std::function<std::chrono::steady_clock::time_point()> now_;
  mutable std::mutex mutex_;
  std::map<std::string, Session> sessions_;
  std::map<std::string, WebSocketResyncReason> tombstones_;
  std::deque<std::string> tombstone_order_;
  std::size_t total_bytes_ = 0;
};

} // namespace kiarabot::control::target
