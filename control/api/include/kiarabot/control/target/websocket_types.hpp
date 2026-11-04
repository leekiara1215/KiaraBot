#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/v1/observations.pb.h"
#include "kiarabot/v1/runtime.pb.h"

namespace kiarabot::control::target {

struct WebSocketFailure {
  v1::ErrorCode code = v1::INTERNAL;
  // Set only by a validated typed owner Conflict/NOT_ACCEPTED/RESYNC_SNAPSHOT.
  bool cursor_expired = false;
};
template <typename Value> using WebSocketResult = std::variant<Value, WebSocketFailure>;
using WebSocketDeadline = std::chrono::steady_clock::time_point;
struct WebSocketIdentity {
  std::string principal;
  std::string identity_session;
  std::uint64_t authorization_version = 0;
  core::target::TimePoint expires_at{};
  core::target::TimePoint checked_at{};
};
struct WebSocketSelection {
  std::string channel;
  std::string owner;
  v1::ResourceScope scope;
};
struct WebSocketFirstFrame {
  std::string access_token;
  std::string session_id;
  std::uint64_t last_sequence = 0;
  v1::ResourceScope scope;
  std::optional<v1::StreamCursor> owner_cursor;
  bool resume = false;
};
// v1.2 feed families consumed by the scoped target channels. Exactly one family is non-empty per
// poll; the owner-issued cursor is carried on every change and on the page boundary.
struct WebSocketScopedBatch {
  v1::StreamCursor next_cursor;
  bool has_more = false;
  std::vector<v1::LogChange> logs;
  std::vector<v1::TaskChange> tasks;
  std::vector<v1::MessageMetadataChange> messages;
};
// Snapshot-style target channels: health readiness and exact-resource adapter/plugin projections.
// Snapshot observations are value-compared by the session; observed_at-only changes never emit an
// event. Unavailable owner records stay unknown/stale (no fabricated value event).
struct WebSocketHealthObservation {
  bool identity_confirmed = false;
  bool runtime_ready = false;
  bool scheduler_ready = false;
  bool model_ready = false;
};
struct WebSocketAdapterObservation {
  v1::AdapterRecord record;
  bool available = false;
};
struct WebSocketPluginObservation {
  v1::InstanceRecord record;
  bool available = false;
};
struct WebSocketSnapshotBatch {
  std::optional<WebSocketHealthObservation> health;
  std::vector<WebSocketAdapterObservation> adapters;
  std::vector<WebSocketPluginObservation> plugins;
};
struct WebSocketPoll {
  WebSocketIdentity identity;
  v1::OperationChangesResponse operations;
  // Set for the v1.2 scoped feed channels (logs/tasks/messages); operations leaves it empty.
  std::optional<WebSocketScopedBatch> scoped;
  // Set for health/adapters/plugins; exactly one of scoped/snapshot/operations is populated.
  std::optional<WebSocketSnapshotBatch> snapshot;
};
// Frozen RESYNC_REQUIRED reasons. CURSOR_EXPIRED covers retained owner/Control replay loss and
// conservative owner dataset gaps; INVALID_CURSOR is a locally invalid replay boundary;
// SERVER_RESTART is a changed/failed Control stream process epoch.
enum class WebSocketResyncReason { CursorExpired, InvalidCursor, ServerRestart };
// Transport measurements used by the bounded retained store and session accounting.
struct WebSocketEventRecord {
  std::uint64_t sequence = 0;
  std::string json;
  std::optional<v1::StreamCursor> owner_cursor;
  std::chrono::steady_clock::time_point retained_at{};
};

} // namespace kiarabot::control::target
