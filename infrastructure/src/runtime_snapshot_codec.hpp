#pragma once

#include <cstddef>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "kiarabot/core/domain/runtime_types.hpp"
#include "kiarabot/core/domain/session_key.hpp"
#include "kiarabot/infrastructure/snapshot_limits.hpp"

namespace kiarabot::infrastructure::detail {
// Raised when the private snapshot exceeds an infrastructure capacity limit.
// Commits map it to StorageError::Unavailable; corruption keeps using the
// generic exceptions below.
class SnapshotLimitError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::EventRecord>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::EventEntry>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::PluginDelivery>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::ActionRecord>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::ActionEntry>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::PluginEntry>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::SessionStatus>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeSnapshot>
void fields(Archive& archive, Value& value);

template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeAuditEntry>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeCommandRecord>
void fields(Archive& archive, Value& value);
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::DailyBudgetEntry>
void fields(Archive& archive, Value& value);
// Private on-disk format, versioned with the database schema. No platform objects.
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::NamespaceEntry>
void fields(Archive& archive, Value& value) {
  archive(value.value, value.expires_at);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::HostReceipt>
void fields(Archive& archive, Value& value) {
  archive(value.fingerprint, value.serialized_response, value.expires_at);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::TaskEntry>
void fields(Archive& archive, Value& value) {
  archive(value.serialized_request, value.state, value.scheduled_at, value.next_run_at,
          value.attempts, value.cancel_requested, value.claim_token, value.event_id);
  if (value.state < core::TaskState::Pending || value.state > core::TaskState::Cancelled ||
      value.event_id.empty()) {
    throw std::runtime_error("Invalid task state");
  }
}
struct Encoder {
  int version{6};
  std::ostringstream stream;
  std::size_t remaining_items{kMaximumSnapshotItems};
  void reserve_items(std::size_t count) {
    if (count > remaining_items) {
      throw SnapshotLimitError("Snapshot item limit");
    }
    remaining_items -= count;
  }
  template <typename... T> void operator()(const T&... values) {
    (one(values), ...);
  }
  template <typename T>
    requires std::is_integral_v<T>
  void one(T value) {
    stream << value << ' ';
  }
  template <typename T>
    requires std::is_enum_v<T>
  void one(T value) {
    one(static_cast<int>(value));
  }
  void one(const std::string& value) {
    stream << std::quoted(value) << ' ';
  }
  void one(core::RuntimeTimePoint value) {
    one(std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
  }
  template <typename T> void one(const std::vector<T>& values) {
    reserve_items(values.size());
    one(values.size());
    for (const auto& value : values) {
      one(value);
    }
  }
  template <typename K, typename V> void one(const std::unordered_map<K, V>& values) {
    reserve_items(values.size());
    one(values.size());
    for (const auto& [key, value] : values) {
      one(key);
      one(value);
    }
  }
  template <typename T>
    requires(!std::is_integral_v<T> && !std::is_enum_v<T>)
  void one(const T& value) {
    fields(*this, value);
  }
};
struct Decoder {
  std::istringstream stream;
  std::size_t remaining_items{kMaximumSnapshotItems};
  int version{6};
  template <typename... T> void operator()(T&... values) {
    (one(values), ...);
  }
  void valid() {
    if (!stream) {
      throw std::runtime_error("Corrupt snapshot");
    }
  }
  template <typename T>
    requires std::is_integral_v<T>
  void one(T& value) {
    stream >> value;
    valid();
  }
  template <typename T>
    requires std::is_enum_v<T>
  void one(T& value) {
    int number = 0;
    one(number);
    const int minimum = std::is_same_v<T, core::TaskState> ? 1 : 0;
    const int maximum = std::is_same_v<T, core::TaskState>     ? 6
                        : std::is_same_v<T, core::PluginState> ? 3
                                                               : 4;
    if (number < minimum || number > maximum) {
      throw std::runtime_error("Corrupt enum");
    }
    value = static_cast<T>(number);
  }
  void one(std::string& value) {
    stream >> std::quoted(value);
    valid();
  }
  void one(core::RuntimeTimePoint& value) {
    std::int64_t number = 0;
    one(number);
    value = core::RuntimeTimePoint(std::chrono::duration_cast<core::RuntimeTimePoint::duration>(
        std::chrono::nanoseconds(number)));
  }
  template <typename T> void one(std::vector<T>& values) {
    std::size_t count = 0;
    one(count);
    if (count > remaining_items) {
      throw SnapshotLimitError("Snapshot item limit");
    }
    remaining_items -= count;
    for (std::size_t i = 0; i < count; ++i) {
      T value{};
      one(value);
      values.push_back(std::move(value));
    }
  }
  template <typename K, typename V> void one(std::unordered_map<K, V>& values) {
    std::size_t count = 0;
    one(count);
    if (count > remaining_items) {
      throw SnapshotLimitError("Snapshot item limit");
    }
    remaining_items -= count;
    for (std::size_t i = 0; i < count; ++i) {
      K key{};
      V value{};
      one(key);
      one(value);
      if (!values.emplace(std::move(key), std::move(value)).second) {
        throw std::runtime_error("Duplicate key");
      }
    }
  }
  template <typename T>
    requires(!std::is_integral_v<T> && !std::is_enum_v<T>)
  void one(T& value) {
    fields(*this, value);
  }
};
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::EventRecord>
void fields(Archive& archive, Value& value) {
  archive(value.event_id, value.trace_id, value.session_key, value.sequence, value.has_sequence,
          value.serialized_envelope);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::EventEntry>
void fields(Archive& archive, Value& value) {
  archive(value.record, value.state, value.attempts, value.next_attempt, value.updated_at,
          value.per_plugin);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::PluginDelivery>
void fields(Archive& archive, Value& value) {
  archive(value.event_id, value.plugin_id, value.session_key, value.trace_id, value.order,
          value.ordered, value.state, value.attempts, value.next_attempt, value.updated_at);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::ActionRecord>
void fields(Archive& archive, Value& value) {
  archive(value.action_id, value.idempotency_key, value.deadline);
  if (archive.version >= 3) {
    archive(value.owner, value.serialized_envelope, value.fingerprint);
  }
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::ActionEntry>
void fields(Archive& archive, Value& value) {
  archive(value.record, value.state, value.updated_at, value.result, value.error_code);
  if (archive.version >= 3) {
    archive(value.dispatched);
  }
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::PluginEntry>
void fields(Archive& archive, Value& value) {
  archive(value.state, value.disconnected_at, value.api_version, value.permissions, value.ordered);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::SessionStatus>
void fields(Archive& archive, Value& value) {
  archive(value.last_sequence, value.has_sequence, value.degraded);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeSnapshot>
void fields(Archive& archive, Value& value) {
  archive(value.events, value.sessions, value.actions, value.plugins, value.deliveries,
          value.delivery_order, value.audit, value.commands);
  if (archive.version >= 4) {
    archive(value.namespaces, value.host_receipts);
  }
  if (archive.version >= 5) {
    archive(value.tasks);
  }
  if (archive.version >= 6) {
    archive(value.daily_budgets);
  }
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeAuditEntry>
void fields(Archive& archive, Value& value) {
  archive(value.actor, value.permission, value.resource, value.request_id, value.trace_id,
          value.occurred_at, value.allowed, value.outcome);
  if (archive.version >= 6) {
    archive(value.target_account_id, value.target_conversation_id);
  }
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeCommandRecord>
void fields(Archive& archive, Value& value) {
  archive(value.fingerprint);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::DailyBudgetEntry>
void fields(Archive& archive, Value& value) {
  archive(value.day, value.used);
}

// Schema v6 migration of the private session ordering keys. Before v6 the key was the
// three-tuple (adapter_id, account_id, conversation_id); it is now the four-tuple with
// the conversation type (doc/01-runtime-semantics.md "顺序和背压"). The stored envelope
// is the only place that still carries the type, so each event with a session key is
// decoded and re-derived; events that cannot be decoded keep their legacy key (and the
// matching watermark cannot be attributed anymore).
//
// A legacy key cannot distinguish a group and a private conversation that reused the
// same platform number, so every typed key derived from it inherits the same watermark.
// The watermark can only make Core accept a continuing sequence, never reject one.
// Watermarks with no surviving event are dropped; the first post-upgrade event for such
// a conversation starts a new session and is reported DEGRADED once.
struct SessionKeyMigration {
  std::size_t rekeyed_events{0};
  std::size_t rekeyed_deliveries{0};
  std::size_t rekeyed_watermarks{0};
  std::size_t dropped_watermarks{0};
  std::size_t unparsed_events{0};
};
inline SessionKeyMigration migrate_session_keys(core::RuntimeSnapshot& snapshot) {
  SessionKeyMigration result;
  std::unordered_map<std::string, std::unordered_set<std::string>> derived;
  std::unordered_map<std::string, std::string> event_keys;
  for (auto& [event_id, entry] : snapshot.events) {
    core::EventRecord& record = entry.record;
    if (record.session_key.empty()) {
      continue;
    }
    kiarabot::v1::EventEnvelope envelope;
    if (!envelope.ParseFromString(record.serialized_envelope)) {
      ++result.unparsed_events;
      continue;
    }
    std::string typed = core::session_key(envelope);
    if (typed == record.session_key) {
      continue;
    }
    derived[record.session_key].insert(typed);
    record.session_key = std::move(typed);
    event_keys[event_id] = record.session_key;
    ++result.rekeyed_events;
  }
  for (auto& [event_id, deliveries] : snapshot.deliveries) {
    const auto found = event_keys.find(event_id);
    if (found == event_keys.end()) {
      continue;
    }
    for (auto& [plugin_id, delivery] : deliveries) {
      static_cast<void>(plugin_id);
      if (delivery.session_key == found->second) {
        continue;
      }
      delivery.session_key = found->second;
      ++result.rekeyed_deliveries;
    }
  }
  std::unordered_map<std::string, core::SessionStatus> sessions;
  for (const auto& [legacy, status] : snapshot.sessions) {
    const auto found = derived.find(legacy);
    if (found == derived.end()) {
      ++result.dropped_watermarks;
      continue;
    }
    for (const std::string& typed : found->second) {
      sessions[typed] = status;
    }
    ++result.rekeyed_watermarks;
  }
  snapshot.sessions = std::move(sessions);
  return result;
}
inline std::string encode(const core::RuntimeSnapshot& snapshot, int version = 6) {
  Encoder encoder;
  encoder.version = version;
  encoder.stream.imbue(std::locale::classic());
  encoder.one(snapshot);
  return encoder.stream.str();
}
inline core::RuntimeSnapshot decode(const std::string& bytes, int version = 6) {
  Decoder decoder{std::istringstream(bytes)};
  decoder.stream.imbue(std::locale::classic());
  decoder.version = version;
  core::RuntimeSnapshot snapshot;
  if (version == 1) {
    decoder(snapshot.events, snapshot.sessions, snapshot.actions, snapshot.plugins,
            snapshot.deliveries, snapshot.delivery_order);
  } else {
    decoder.one(snapshot);
  }
  decoder.stream >> std::ws;
  if (!decoder.stream.eof()) {
    throw std::runtime_error("Trailing snapshot data");
  }
  return snapshot;
}
} // namespace kiarabot::infrastructure::detail
