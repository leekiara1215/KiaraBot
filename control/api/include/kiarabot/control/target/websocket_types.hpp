#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/v1/observations.pb.h"

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
struct WebSocketPoll {
  WebSocketIdentity identity;
  v1::OperationChangesResponse operations;
};

} // namespace kiarabot::control::target
