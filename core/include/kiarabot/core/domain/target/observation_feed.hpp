#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// v1.2 read-only owner observation families. A cursor is valid for exactly one (owner, family)
// durable dataset; it is never an authorization capability and never crosses families.
enum class ObservationFamily { Logs, Messages, Tasks };

std::string_view observation_family_name(ObservationFamily family);

// Mirrors the frozen common.proto ScheduledTaskState order; terminal states stay observable.
enum class TaskFeedState { Pending, Running, Retrying, Completed, Failed, Cancelled };

// One owner dataset incarnation for one feed family. Ordinary owner restart preserves this
// epoch; restore/rejournal/sequence reset must rotate it (see the offline restore tool).
struct ObservationFeedCursor {
  Service owner = Service::Identity;
  ObservationFamily family = ObservationFamily::Logs;
  std::string epoch;
  std::vector<ResourceRef> scope;
  std::uint64_t sequence = 0;
  bool operator==(const ObservationFeedCursor&) const = default;
};

struct ObservationFeedQuery {
  ObservationFamily family = ObservationFamily::Logs;
  // Caller must currently authorize every exact resource before storage access.
  std::vector<ResourceRef> scope;
  std::optional<ObservationFeedCursor> cursor;
  std::uint32_t page_size = 0; // 0 means 32; otherwise 1..100.
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
};

// Safe metadata projections only: no audit payload, free text, task payload/target, message body,
// platform raw payload, configuration or credential material.
struct LogObservation {
  Service owner = Service::Identity; // identity or runtime
  std::string entry_id;
  std::string severity; // closed set DEBUG/INFO/WARN/ERROR
  std::string component;
  std::string method;
  std::string event; // closed set request.recorded/request.rejected
  ResourceRef resource;
  std::string request_id;
  std::string trace_id;
  TimePoint occurred_at{};
};

struct TaskObservation {
  std::string owner_instance_id;
  std::string task_id;
  TaskFeedState state = TaskFeedState::Pending;
  std::uint64_t version = 0;
  TimePoint updated_at{};
  bool cancel_requested = false;
  std::optional<std::string> occurrence_id;
  std::optional<ErrorCode> reason_code;
};

struct MessageObservation {
  std::string message_id;
  std::string conversation_id;
  std::string account_id;
  TimePoint received_at{};
};

using ObservationPayload = std::variant<LogObservation, TaskObservation, MessageObservation>;

// One append unit staged by the owning business UoW. The payload is the owner's already safe
// typed projection; infrastructure encodes it and the business mutation commits atomically.
struct ObservationFeedAppend {
  ResourceRef resource;
  ObservationPayload payload;
  TimePoint observed_at{};
};

struct ObservationFeedChange {
  ObservationPayload payload;
  ObservationFeedCursor cursor;
};

struct ObservationFeedPage {
  std::vector<ObservationFeedChange> changes;
  ObservationFeedCursor next_cursor;
  bool has_more = false;
  TimePoint checked_at{};
};

// Bound values are per (owner, family) journal and match the frozen contract: at least 600s
// retention, capacity eviction advances low-water explicitly, never a silent gap.
inline constexpr std::size_t kObservationFeedCapacity = 16384;
inline constexpr std::size_t kObservationFeedBytes = 16 * 1024 * 1024;
inline constexpr auto kObservationFeedRetention = std::chrono::seconds{600};
inline constexpr std::size_t kObservationFeedMaximumPayloadBytes = 4096;

} // namespace kiarabot::core::target
