#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kiarabot::core {

using RuntimeTimePoint = std::chrono::system_clock::time_point;
using RuntimeClock = std::function<RuntimeTimePoint()>;

enum class EventResult { accepted, duplicate, invalid, out_of_order, rate_limited };

enum class DeliveryState { persisted, dispatched, retrying, acked, dead_letter };

enum class ActionResult { accepted, duplicate, invalid, expired };

enum class ActionState { accepted, succeeded, failed, expired, cancelled };

enum class PluginState { ready, running, unavailable, failed };

struct EventRecord {
  std::string event_id;
  std::string trace_id;
  std::string session_key;
  std::uint64_t sequence{0};
  bool has_sequence{false};
};

struct EventEntry {
  EventRecord record;
  DeliveryState state{DeliveryState::persisted};
  std::size_t attempts{0};
  RuntimeTimePoint next_attempt{};
  RuntimeTimePoint updated_at{};
};

struct ActionRecord {
  std::string action_id;
  std::string idempotency_key;
  RuntimeTimePoint deadline{};
};

struct ActionEntry {
  ActionRecord record;
  ActionState state{ActionState::accepted};
  RuntimeTimePoint updated_at{};
};

struct PluginEntry {
  PluginState state{PluginState::ready};
  RuntimeTimePoint disconnected_at{};
};

struct RuntimeOptions {
  std::size_t queue_limit{1024};
  std::size_t max_attempts{5};
  std::chrono::hours retention{168};
  std::chrono::minutes plugin_failure_after{5};
};

// RuntimeStore is the Core port implemented by infrastructure. It owns the
// durable inbox/outbox/action records; Runtime owns the business transitions.
class RuntimeStore {
 public:
  virtual ~RuntimeStore() = default;

  [[nodiscard]] virtual bool contains_event(std::string_view event_id) const = 0;
  [[nodiscard]] virtual std::optional<EventEntry> find_event(
      std::string_view event_id) const = 0;
  [[nodiscard]] virtual std::size_t pending_event_count() const = 0;
  [[nodiscard]] virtual std::vector<EventEntry> events() const = 0;
  virtual bool insert_event(EventEntry entry) = 0;
  virtual bool update_event(EventEntry entry) = 0;
  virtual bool erase_event(std::string_view event_id) = 0;

  [[nodiscard]] virtual std::optional<ActionEntry> find_action_by_idempotency(
      std::string_view idempotency_key) const = 0;
  [[nodiscard]] virtual std::vector<ActionEntry> actions() const = 0;
  virtual bool insert_action(ActionEntry entry) = 0;
  virtual bool update_action(ActionEntry entry) = 0;

  [[nodiscard]] virtual std::optional<PluginEntry> find_plugin(
      std::string_view plugin_id) const = 0;
  virtual bool upsert_plugin(std::string plugin_id, PluginEntry entry) = 0;
};

// Test and local-development store. Production infrastructure can replace it
// with SQLite or another repository without changing Runtime.
class InMemoryRuntimeStore final : public RuntimeStore {
 public:
  [[nodiscard]] bool contains_event(std::string_view event_id) const override {
    return events_.contains(std::string(event_id));
  }

  [[nodiscard]] std::optional<EventEntry> find_event(
      std::string_view event_id) const override {
    const auto iterator = events_.find(std::string(event_id));
    if (iterator == events_.end()) return std::nullopt;
    return iterator->second;
  }

  [[nodiscard]] std::size_t pending_event_count() const override {
    std::size_t count = 0;
    for (const auto& [_, entry] : events_) {
      if (entry.state != DeliveryState::acked && entry.state != DeliveryState::dead_letter) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::vector<EventEntry> events() const override {
    std::vector<EventEntry> result;
    result.reserve(events_.size());
    for (const auto& [_, entry] : events_) result.push_back(entry);
    return result;
  }

  bool insert_event(EventEntry entry) override {
    return events_.emplace(entry.record.event_id, std::move(entry)).second;
  }

  bool update_event(EventEntry entry) override {
    const auto iterator = events_.find(entry.record.event_id);
    if (iterator == events_.end()) return false;
    iterator->second = std::move(entry);
    return true;
  }

  bool erase_event(std::string_view event_id) override {
    return events_.erase(std::string(event_id)) != 0;
  }

  [[nodiscard]] std::optional<ActionEntry> find_action_by_idempotency(
      std::string_view idempotency_key) const override {
    const auto iterator = actions_.find(std::string(idempotency_key));
    if (iterator == actions_.end()) return std::nullopt;
    return iterator->second;
  }

  [[nodiscard]] std::vector<ActionEntry> actions() const override {
    std::vector<ActionEntry> result;
    result.reserve(actions_.size());
    for (const auto& [_, entry] : actions_) result.push_back(entry);
    return result;
  }

  bool insert_action(ActionEntry entry) override {
    return actions_.emplace(entry.record.idempotency_key, std::move(entry)).second;
  }

  bool update_action(ActionEntry entry) override {
    const auto iterator = actions_.find(entry.record.idempotency_key);
    if (iterator == actions_.end()) return false;
    iterator->second = std::move(entry);
    return true;
  }

  [[nodiscard]] std::optional<PluginEntry> find_plugin(
      std::string_view plugin_id) const override {
    const auto iterator = plugins_.find(std::string(plugin_id));
    if (iterator == plugins_.end()) return std::nullopt;
    return iterator->second;
  }

  bool upsert_plugin(std::string plugin_id, PluginEntry entry) override {
    plugins_[std::move(plugin_id)] = entry;
    return true;
  }

 private:
  std::unordered_map<std::string, EventEntry> events_;
  std::unordered_map<std::string, ActionEntry> actions_;
  std::unordered_map<std::string, PluginEntry> plugins_;
};

class Runtime {
 public:
  explicit Runtime(RuntimeOptions options = {}, RuntimeClock clock = {})
      : owned_store_(), store_(&owned_store_), options_(options), clock_(std::move(clock)) {
    rebuild_sequence_index();
  }

  explicit Runtime(RuntimeStore& store, RuntimeOptions options = {}, RuntimeClock clock = {})
      : owned_store_(), store_(&store), options_(options), clock_(std::move(clock)) {
    rebuild_sequence_index();
  }

  EventResult ingest(EventRecord event) {
    if (event.event_id.empty()) return EventResult::invalid;
    if (store_->contains_event(event.event_id)) return EventResult::duplicate;
    if (store_->pending_event_count() >= options_.queue_limit) return EventResult::rate_limited;

    if (event.has_sequence) {
      if (event.session_key.empty() || event.sequence == 0) return EventResult::invalid;
      const auto last = last_sequences_.find(event.session_key);
      if (last != last_sequences_.end() && event.sequence <= last->second) {
        return EventResult::out_of_order;
      }
      if (last != last_sequences_.end() && event.sequence > last->second + 1) {
        sequence_gap_detected_ = true;
      }
      last_sequences_[event.session_key] = event.sequence;
    }

    const RuntimeTimePoint now = current_time();
    EventEntry entry{std::move(event), DeliveryState::persisted, 0, now, now};
    return store_->insert_event(std::move(entry)) ? EventResult::accepted : EventResult::duplicate;
  }

  [[nodiscard]] std::size_t inbox_size() const { return store_->events().size(); }

  [[nodiscard]] std::size_t pending_size() const { return store_->pending_event_count(); }

  [[nodiscard]] bool sequence_gap_detected() const noexcept { return sequence_gap_detected_; }

  bool mark_dispatched(std::string_view event_id) {
    const auto entry = store_->find_event(event_id);
    if (!entry.has_value()) return false;
    EventEntry updated = *entry;
    updated.state = DeliveryState::dispatched;
    updated.updated_at = current_time();
    return store_->update_event(std::move(updated));
  }

  bool acknowledge_event(std::string_view event_id) {
    const auto entry = store_->find_event(event_id);
    if (!entry.has_value()) return false;
    EventEntry updated = *entry;
    updated.state = DeliveryState::acked;
    updated.updated_at = current_time();
    return store_->update_event(std::move(updated));
  }

  bool fail_event(std::string_view event_id, bool retryable) {
    const auto entry = store_->find_event(event_id);
    if (!entry.has_value()) return false;
    EventEntry updated = *entry;
    ++updated.attempts;
    if (!retryable || updated.attempts >= options_.max_attempts) {
      updated.state = DeliveryState::dead_letter;
      updated.updated_at = current_time();
      return store_->update_event(std::move(updated));
    }
    updated.state = DeliveryState::retrying;
    updated.next_attempt = current_time() + retry_delay(updated.attempts);
    updated.updated_at = current_time();
    return store_->update_event(std::move(updated));
  }

  [[nodiscard]] std::vector<std::string> due_retries() const {
    const RuntimeTimePoint now = current_time();
    std::vector<std::string> result;
    for (const EventEntry& entry : store_->events()) {
      if (entry.state == DeliveryState::retrying && entry.next_attempt <= now) {
        result.push_back(entry.record.event_id);
      }
    }
    return result;
  }

  [[nodiscard]] std::size_t dead_letter_size() const {
    std::size_t count = 0;
    for (const EventEntry& entry : store_->events()) {
      if (entry.state == DeliveryState::dead_letter) ++count;
    }
    return count;
  }

  std::size_t purge_expired() {
    const RuntimeTimePoint cutoff = current_time() - options_.retention;
    std::size_t removed = 0;
    for (const EventEntry& entry : store_->events()) {
      const bool terminal = entry.state == DeliveryState::acked ||
                            entry.state == DeliveryState::dead_letter;
      if (terminal && entry.updated_at <= cutoff && store_->erase_event(entry.record.event_id)) {
        ++removed;
      }
    }
    return removed;
  }

  ActionResult submit_action(ActionRecord action) {
    if (action.action_id.empty() || action.idempotency_key.empty()) return ActionResult::invalid;
    if (store_->find_action_by_idempotency(action.idempotency_key).has_value()) {
      return ActionResult::duplicate;
    }
    const RuntimeTimePoint now = current_time();
    if (action.deadline <= now) return ActionResult::expired;
    ActionEntry entry{std::move(action), ActionState::accepted, now};
    return store_->insert_action(std::move(entry)) ? ActionResult::accepted
                                                    : ActionResult::duplicate;
  }

  bool complete_action(std::string_view idempotency_key, bool succeeded) {
    const auto entry = store_->find_action_by_idempotency(idempotency_key);
    if (!entry.has_value() || entry->state != ActionState::accepted) return false;
    ActionEntry updated = *entry;
    updated.state = succeeded ? ActionState::succeeded : ActionState::failed;
    updated.updated_at = current_time();
    return store_->update_action(std::move(updated));
  }

  std::size_t expire_actions() {
    const RuntimeTimePoint now = current_time();
    std::size_t expired = 0;
    for (const ActionEntry& entry : store_->actions()) {
      if (entry.state == ActionState::accepted && entry.record.deadline <= now) {
        ActionEntry updated = entry;
        updated.state = ActionState::expired;
        updated.updated_at = now;
        if (store_->update_action(std::move(updated))) ++expired;
      }
    }
    return expired;
  }

  bool register_plugin(std::string plugin_id) {
    if (plugin_id.empty()) return false;
    return store_->upsert_plugin(std::move(plugin_id), PluginEntry{PluginState::ready, {}});
  }

  bool plugin_started(std::string_view plugin_id) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value()) return false;
    PluginEntry updated = *entry;
    updated.state = PluginState::running;
    updated.disconnected_at = {};
    return store_->upsert_plugin(std::string(plugin_id), updated);
  }

  bool plugin_disconnected(std::string_view plugin_id) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value()) return false;
    PluginEntry updated = *entry;
    updated.state = PluginState::unavailable;
    updated.disconnected_at = current_time();
    return store_->upsert_plugin(std::string(plugin_id), updated);
  }

  bool update_plugin_recovery(std::string_view plugin_id) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value() || entry->state != PluginState::unavailable) return false;
    PluginEntry updated = *entry;
    if (current_time() - updated.disconnected_at >= options_.plugin_failure_after) {
      updated.state = PluginState::failed;
      return store_->upsert_plugin(std::string(plugin_id), updated);
    }
    return true;
  }

 private:
  void rebuild_sequence_index() {
    for (const EventEntry& entry : store_->events()) {
      if (!entry.record.has_sequence || entry.record.session_key.empty()) continue;
      auto& last_sequence = last_sequences_[entry.record.session_key];
      if (entry.record.sequence > last_sequence) last_sequence = entry.record.sequence;
    }
  }

  [[nodiscard]] RuntimeTimePoint current_time() const {
    return clock_ ? clock_() : std::chrono::system_clock::now();
  }

  [[nodiscard]] std::chrono::milliseconds retry_delay(std::size_t attempt) const {
    const std::size_t exponent = attempt > 0 ? attempt - 1 : 0;
    const std::size_t seconds = std::size_t{1} << (exponent < 4 ? exponent : 4);
    return std::chrono::milliseconds(seconds * 1000);
  }

  InMemoryRuntimeStore owned_store_;
  RuntimeStore* store_;
  RuntimeOptions options_;
  RuntimeClock clock_;
  std::unordered_map<std::string, std::uint64_t> last_sequences_;
  bool sequence_gap_detected_{false};
};

}  // namespace kiarabot::core
