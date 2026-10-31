#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kiarabot::core {

using RuntimeTimePoint = std::chrono::system_clock::time_point;
using RuntimeClock = std::function<RuntimeTimePoint()>;
using RuntimeRandom = std::function<double()>;

enum class EventResult {
  accepted,
  duplicate,
  invalid,
  out_of_order,
  rate_limited,
  storage_error,
};

enum class DeliveryState { persisted, dispatched, retrying, acked, dead_letter };

enum class ActionResult { accepted, duplicate, invalid, expired, storage_error };

enum class ActionState { accepted, succeeded, failed, expired, cancelled };

enum class PluginState { ready, running, unavailable, failed };

enum class StoreInsertResult { inserted, duplicate, failed };

struct EventRecord {
  std::string event_id;
  std::string trace_id;
  std::string session_key;
  std::uint64_t sequence{0};
  bool has_sequence{false};
  // Opaque serialized EventEnvelope; encoding and decoding belong to the transport.
  std::string serialized_envelope{};
};

struct EventEntry {
  EventRecord record;
  DeliveryState state{DeliveryState::persisted};
  std::size_t attempts{0};
  RuntimeTimePoint next_attempt{};
  RuntimeTimePoint updated_at{};
  bool per_plugin{false};
};

struct PluginDelivery {
  std::string event_id;
  std::string plugin_id;
  std::string session_key;
  std::string trace_id;
  std::uint64_t order{0};
  bool ordered{false};
  DeliveryState state{DeliveryState::persisted};
  std::size_t attempts{0};
  RuntimeTimePoint next_attempt{};
  RuntimeTimePoint updated_at{};
};

struct ActionRecord {
  std::string action_id;
  std::string idempotency_key;
  RuntimeTimePoint deadline{};
  std::string owner{};
  std::string serialized_envelope{};
  std::string fingerprint{};
};

struct ActionEntry {
  ActionRecord record;
  ActionState state{ActionState::accepted};
  RuntimeTimePoint updated_at{};
  std::string result;
  std::string error_code;
  bool dispatched{false};
};

struct ActionSubmission {
  ActionResult result;
  std::optional<ActionEntry> action;

  operator ActionResult() const noexcept {
    return result;
  }
};

struct BatchResult {
  std::size_t count{0};
  bool storage_error{false};

  operator std::size_t() const noexcept {
    return count;
  }
};

struct PluginEntry {
  PluginState state{PluginState::ready};
  RuntimeTimePoint disconnected_at{};
  std::string api_version;
  std::vector<std::string> permissions;
  bool ordered{false};
};

struct SessionStatus {
  std::uint64_t last_sequence{0};
  bool has_sequence{false};
  bool degraded{false};
};

struct RuntimeOptions {
  std::size_t queue_limit{1024};
  std::size_t max_attempts{5};
  std::chrono::hours retention{168};
  std::chrono::minutes plugin_failure_after{5};
  // A dispatched per-plugin delivery that is not acknowledged within this
  // window is treated as one retryable failure (backoff, then dead letter).
  std::chrono::milliseconds ack_timeout{30000};
};

// Capacity limits shared by the Core application services. Injected by the
// composition root; services never own deployment policy.
struct RuntimeLimits {
  std::size_t audit_limit{100000};
  std::size_t action_limit{10000};
  std::size_t command_limit{100000};
};

struct RuntimeAuditEntry {
  std::string actor;
  std::string permission;
  std::string resource;
  std::string request_id;
  std::string trace_id;
  RuntimeTimePoint occurred_at{};
  bool allowed{false};
  std::string outcome;
};
struct RuntimeCommandRecord {
  std::string fingerprint;
};
struct NamespaceEntry {
  std::string value;
  RuntimeTimePoint expires_at{}; // Epoch means no expiration.
};
struct HostReceipt {
  std::string fingerprint;
  std::string serialized_response;
  RuntimeTimePoint expires_at{};
};
enum class TaskState {
  Pending = 1,
  Running = 2,
  Retrying = 3,
  Completed = 4,
  Failed = 5,
  Cancelled = 6
};
struct TaskEntry {
  std::string serialized_request;
  TaskState state{TaskState::Pending};
  RuntimeTimePoint scheduled_at{};
  RuntimeTimePoint next_run_at{};
  std::uint32_t attempts{0};
  bool cancel_requested{false};
  std::string claim_token;
  std::string event_id;
};
struct RuntimeSnapshot {
  std::unordered_map<std::string, EventEntry> events;
  std::unordered_map<std::string, SessionStatus> sessions;
  std::unordered_map<std::string, ActionEntry> actions;
  std::unordered_map<std::string, PluginEntry> plugins;
  std::unordered_map<std::string, std::unordered_map<std::string, PluginDelivery>> deliveries;
  std::uint64_t delivery_order{0};
  std::vector<RuntimeAuditEntry> audit;
  std::unordered_map<std::string, std::unordered_map<std::string, RuntimeCommandRecord>> commands;
  std::unordered_map<std::string, std::unordered_map<std::string, NamespaceEntry>> namespaces;
  std::unordered_map<std::string, std::unordered_map<std::string, HostReceipt>> host_receipts;
  std::unordered_map<std::string, std::unordered_map<std::string, TaskEntry>> tasks;
};
} // namespace kiarabot::core
