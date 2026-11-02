#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// Owner-local bounded query over the service's authoritative audit facts. Times are UTC database
// times; the stable entry id is a positive monotonic bigint. Keyset order is newest first
// (occurred_at DESC, id DESC); the opaque cursor is the last returned (occurred_at,id).
struct AuditQueryFilter {
  std::optional<TimePoint> occurred_from;   // inclusive
  std::optional<TimePoint> occurred_until;  // exclusive
  std::optional<SubjectId> actor;
  std::optional<ResourceRef> resource;
};
struct AuditCursor {
  TimePoint occurred_at;
  std::int64_t id = 0;
};
struct AuditRecord {
  std::int64_t id = 0;
  SubjectId actor;
  ResourceRef resource;
  std::string request_id;
  std::string trace_id;
  std::string method;
  std::optional<ErrorCode> rejection;
  ResourceVersion authorization_version;
  TimePoint occurred_at;
  std::optional<std::string> target_account_id;
  std::optional<std::string> target_conversation_id;
};
struct AuditPage {
  std::vector<AuditRecord> records;
  std::string next_cursor;
  TimePoint checked_at;
};
// The operational log projection is derived from the same authoritative audit facts. message is a
// closed redacted summary ("request.recorded"/"request.rejected"), never raw text or secrets.
struct LogRecord {
  std::int64_t id = 0;
  TimePoint occurred_at;
  std::string severity;
  std::string component;
  std::string method;
  std::optional<SubjectId> actor;
  std::optional<ResourceRef> resource;
  std::string request_id;
  std::string trace_id;
  std::string message;
};
struct LogPage {
  std::vector<LogRecord> records;
  std::string next_cursor;
  TimePoint checked_at;
};
struct AuditQuery {
  AuditQueryFilter filter;
  std::optional<AuditCursor> cursor;
  std::size_t limit = 20;
};
using LogQuery = AuditQuery;

// Opaque cursor codec shared by the application/RPC boundary. Rejects non-canonical tokens.
inline std::string encode_audit_cursor(const AuditCursor& cursor) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         cursor.occurred_at.time_since_epoch())
                         .count();
  return std::to_string(nanos) + ":" + std::to_string(cursor.id);
}
inline std::optional<AuditCursor> decode_audit_cursor(std::string_view token) {
  const auto colon = token.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= token.size() ||
      token.size() > 64) {
    return std::nullopt;
  }
  const auto parse = [](std::string_view text) -> std::optional<std::int64_t> {
    if (text.empty() || text.size() > 19 || (text.size() > 1 && text.front() == '0')) {
      return std::nullopt;
    }
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0) {
      return std::nullopt;
    }
    return value;
  };
  const auto nanos = parse(token.substr(0, colon));
  const auto id = parse(token.substr(colon + 1));
  if (!nanos || !id) {
    return std::nullopt;
  }
  return AuditCursor{TimePoint{std::chrono::nanoseconds{*nanos}}, *id};
}

} // namespace kiarabot::core::target
