#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace kiarabot::core::target {

// P04 invalidations are a bounded accelerator for authorization convergence, never the authority
// for high-risk decisions (online CheckAuthorization remains the final check). The stream is
// per-Identity-private-database, ordered by a committed sequence, and bounded by a fixed replay
// window; a subscriber that cannot prove continuity must take a full AuthorizationSnapshot.
inline constexpr std::chrono::minutes kInvalidationRetention{10};
inline constexpr std::uint64_t kInvalidationCapacity = 4096;
inline constexpr std::size_t kMaximumInvalidationScope = 128;
inline constexpr std::string_view kInvalidationSourceService = "identity";

// One committed authorization-change fact. principal is the subject whose authorization version
// advanced; scope lists the exact resources whose authority may have changed (assignment scope at
// the time of the change). resync_required is only used by the stream signal, never stored as a
// real authorization fact.
struct InvalidationEvent {
  SubjectId principal;
  ResourceVersion authorization_version;
  std::vector<ResourceRef> scope;
  bool resync_required = false;
};
struct InvalidationCursor {
  std::string source_service;
  std::string epoch;
  std::vector<ResourceRef> scope;
  std::uint64_t sequence = 0;
};
// Persisted stream incarnation. epoch changes only when the durable log is reinitialized (empty
// database/restore), so a process restart keeps real continuity instead of faking or resetting it.
struct InvalidationStreamState {
  std::string epoch;
  std::uint64_t next_sequence = 0;
};
struct InvalidationRecord {
  std::string epoch;
  std::uint64_t sequence = 0;
  InvalidationEvent event;
  TimePoint created_at;
};
// Watermark returned to snapshot callers so a later watch resumes from the same committed point.
struct InvalidationWatermark {
  std::string epoch;
  std::uint64_t sequence = 0;
};

inline bool valid_uuid_text(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    const bool hex = (character >= '0' && character <= '9') ||
                     (character >= 'a' && character <= 'f') ||
                     (character >= 'A' && character <= 'F');
    if (!hex) {
      return false;
    }
  }
  return true;
}
inline bool unique_invalidation_scope(std::span<const ResourceRef> scope) {
  if (scope.empty() || scope.size() > kMaximumInvalidationScope) {
    return false;
  }
  for (std::size_t index = 0; index < scope.size(); ++index) {
    if (!valid_policy_resource(scope[index])) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (scope[previous] == scope[index]) {
        return false;
      }
    }
  }
  return true;
}
inline Status validate_invalidation_event(const InvalidationEvent& event) {
  if (event.principal.value.empty() || event.authorization_version.value == 0 ||
      !unique_invalidation_scope(event.scope)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}
// Cursor shape only; continuity (epoch/sequence gap) is decided against committed stream state.
inline Status validate_invalidation_cursor(const InvalidationCursor& cursor) {
  if (!cursor.source_service.empty() && cursor.source_service != kInvalidationSourceService) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (cursor.scope.size() > kMaximumInvalidationScope ||
      (!cursor.scope.empty() && !unique_invalidation_scope(cursor.scope))) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (cursor.sequence == 0 && cursor.epoch.empty()) {
    // Truly no cursor yet: first subscription asks for a full resync.
    return std::monostate{};
  }
  if (cursor.epoch.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}
// True when the requested continuation cannot be served from the bounded durable log: a different
// epoch (restored/reinitialized log), a cursor older than the retained window, or an initial
// subscription. A future sequence is caller error, not resync.
inline bool invalidation_resync_required(const InvalidationWatermark& watermark,
                                         std::optional<std::uint64_t> oldest_retained,
                                         const InvalidationCursor& cursor) {
  // An empty epoch means "no cursor at all" (first subscription). A snapshot cursor with
  // sequence 0 and the current epoch is a genuine up-to-date watermark, not a resync trigger.
  if (cursor.epoch.empty() || cursor.epoch != watermark.epoch) {
    return true;
  }
  if (cursor.sequence > watermark.sequence) {
    return false;
  }
  if (oldest_retained && cursor.sequence + 1 < *oldest_retained) {
    return true;
  }
  return false;
}
inline Status validate_invalidation_continuation(const InvalidationWatermark& watermark,
                                                 const InvalidationCursor& cursor) {
  if (cursor.sequence > watermark.sequence) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}

} // namespace kiarabot::core::target
