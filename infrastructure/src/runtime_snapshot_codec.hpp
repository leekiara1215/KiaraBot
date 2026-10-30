#pragma once

#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "kiarabot/core/domain/runtime_types.hpp"

namespace kiarabot::infrastructure::detail {
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
  static constexpr int version = 5;
  std::ostringstream stream;
  std::size_t remaining_items{1000000};
  void reserve_items(std::size_t count) {
    if (count > remaining_items) {
      throw std::runtime_error("Snapshot limit");
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
  std::size_t remaining_items{1000000};
  int version{5};
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
      throw std::runtime_error("Snapshot limit");
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
      throw std::runtime_error("Snapshot limit");
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
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeAuditEntry>
void fields(Archive& archive, Value& value) {
  archive(value.actor, value.permission, value.resource, value.request_id, value.trace_id,
          value.occurred_at, value.allowed, value.outcome);
}
template <typename Archive, typename Value>
  requires std::is_same_v<std::remove_const_t<Value>, core::RuntimeCommandRecord>
void fields(Archive& archive, Value& value) {
  archive(value.fingerprint);
}
inline std::string encode(const core::RuntimeSnapshot& snapshot) {
  Encoder encoder;
  encoder.stream.imbue(std::locale::classic());
  encoder.one(snapshot);
  return encoder.stream.str();
}
inline core::RuntimeSnapshot decode(const std::string& bytes, int version = 5) {
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
