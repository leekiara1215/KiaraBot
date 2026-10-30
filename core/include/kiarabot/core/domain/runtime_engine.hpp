#pragma once

#include "kiarabot/core/domain/runtime_state_port.hpp"

namespace kiarabot::core::detail {

class RuntimeEngine {
public:
  explicit RuntimeEngine(RuntimeStatePort& store, RuntimeOptions options = {},
                         RuntimeClock clock = {}, RuntimeRandom random = {})
      : store_(&store), options_(options), clock_(std::move(clock)),
        random_(std::move(random)) {
    options_.retention = std::max(options_.retention, std::chrono::hours(24));
  }

  RuntimeEngine(const RuntimeEngine&) = delete;
  RuntimeEngine& operator=(const RuntimeEngine&) = delete;
  RuntimeEngine(RuntimeEngine&&) = delete;
  RuntimeEngine& operator=(RuntimeEngine&&) = delete;

  EventResult ingest(EventRecord event, const std::vector<std::string>& plugin_ids = {}) {
    if (event.event_id.empty()) {
      return EventResult::invalid;
    }
    std::unique_ptr<UnitOfWork> transaction = store_->begin_unit_of_work();
    if (!transaction) {
      return EventResult::storage_error;
    }
    if (store_->contains_event(event.event_id)) {
      transaction->rollback();
      return EventResult::duplicate;
    }
    if (store_->pending_event_count() >= options_.queue_limit) {
      transaction->rollback();
      return EventResult::rate_limited;
    }

    std::vector<std::pair<std::string, bool>> targets;
    for (const std::string& plugin_id : plugin_ids) {
      const auto plugin = store_->find_plugin(plugin_id);
      if (plugin_id.empty() || !plugin.has_value() ||
          std::any_of(targets.begin(), targets.end(),
                      [&](const auto& target) { return target.first == plugin_id; })) {
        transaction->rollback();
        return EventResult::invalid;
      }
      targets.emplace_back(plugin_id, plugin->ordered);
    }

    std::optional<SessionStatus> next_status;
    const std::string session_key = event.session_key;
    if (event.has_sequence) {
      if (event.session_key.empty() || event.sequence == 0) {
        transaction->rollback();
        return EventResult::invalid;
      }
      SessionStatus status = store_->find_session(event.session_key).value_or(SessionStatus{});
      if (status.has_sequence && event.sequence <= status.last_sequence) {
        transaction->rollback();
        return EventResult::out_of_order;
      }
      if (!status.has_sequence && event.sequence != 1) {
        status.degraded = true;
      }
      if (status.has_sequence && event.sequence != status.last_sequence + 1) {
        status.degraded = true;
      }
      status.last_sequence = event.sequence;
      status.has_sequence = true;
      next_status = status;
    } else if (!event.session_key.empty()) {
      SessionStatus status = store_->find_session(event.session_key).value_or(SessionStatus{});
      status.degraded = true;
      next_status = status;
    }

    const RuntimeTimePoint now = current_time();
    std::optional<std::uint64_t> order;
    if (!targets.empty()) {
      order = store_->allocate_delivery_order();
      if (!order.has_value()) {
        transaction->rollback();
        return EventResult::storage_error;
      }
    }
    EventEntry entry{std::move(event), DeliveryState::persisted, 0, now, now, !targets.empty()};
    const std::string event_id = entry.record.event_id;
    const std::string trace_id = entry.record.trace_id;
    const StoreInsertResult insertion = store_->insert_event(std::move(entry));
    if (insertion != StoreInsertResult::inserted) {
      transaction->rollback();
      return insertion == StoreInsertResult::duplicate ? EventResult::duplicate
                                                       : EventResult::storage_error;
    }
    if ((next_status.has_value() && !store_->upsert_session(session_key, *next_status)) ||
        !insert_deliveries(event_id, session_key, trace_id, order, targets, now) ||
        !transaction->commit()) {
      transaction->rollback();
      return EventResult::storage_error;
    }
    return EventResult::accepted;
  }

  [[nodiscard]] std::size_t inbox_size() const {
    return store_->events().size();
  }
  [[nodiscard]] std::size_t pending_size() const {
    return store_->pending_event_count();
  }

  [[nodiscard]] bool sequence_gap_detected() const {
    for (const SessionStatus& status : store_->sessions()) {
      if (status.degraded) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool session_degraded(std::string_view session_key) const {
    const auto status = store_->find_session(session_key);
    return status.has_value() && status->degraded;
  }

  [[nodiscard]] std::optional<SessionStatus> session_status(std::string_view session_key) const {
    return store_->find_session(session_key);
  }

  bool mark_dispatched(std::string_view event_id) {
    return persist([&] {
      const auto entry = store_->find_event(event_id);
      if (!entry.has_value() || entry->per_plugin ||
          (entry->state != DeliveryState::persisted && entry->state != DeliveryState::retrying) ||
          (entry->state == DeliveryState::retrying && entry->next_attempt > current_time())) {
        return false;
      }
      EventEntry updated = *entry;
      updated.state = DeliveryState::dispatched;
      updated.updated_at = current_time();
      return store_->update_event(std::move(updated));
    });
  }

  bool acknowledge_event(std::string_view event_id) {
    return persist([&] {
      const auto entry = store_->find_event(event_id);
      if (!entry.has_value() || entry->per_plugin || entry->state != DeliveryState::dispatched) {
        return false;
      }
      EventEntry updated = *entry;
      updated.state = DeliveryState::acked;
      updated.updated_at = current_time();
      return store_->update_event(std::move(updated));
    });
  }

  bool fail_event(std::string_view event_id, bool retryable) {
    return persist([&] {
      const auto entry = store_->find_event(event_id);
      if (!entry.has_value() || entry->per_plugin ||
          (entry->state != DeliveryState::persisted && entry->state != DeliveryState::dispatched)) {
        return false;
      }
      EventEntry updated = *entry;
      const bool exhausted = updated.attempts >= options_.max_attempts;
      if (updated.attempts < std::numeric_limits<std::size_t>::max()) {
        ++updated.attempts;
      }
      const RuntimeTimePoint now = current_time();
      if (!retryable || exhausted) {
        updated.state = DeliveryState::dead_letter;
      } else {
        updated.state = DeliveryState::retrying;
        updated.next_attempt = now + retry_delay_for_attempt(updated.attempts);
      }
      updated.updated_at = now;
      return store_->update_event(std::move(updated));
    });
  }

  [[nodiscard]] std::vector<std::string> due_retries() const {
    const RuntimeTimePoint now = current_time();
    std::vector<std::string> result;
    for (const EventEntry& entry : store_->events()) {
      if (!entry.per_plugin && entry.state == DeliveryState::retrying &&
          entry.next_attempt <= now) {
        result.push_back(entry.record.event_id);
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  // Call once after the previous delivery owner has stopped. A dispatched
  // event has an unknown outcome after a crash and must be retried.
  BatchResult recover_inflight_events() {
    BatchResult result;
    for (const EventEntry& entry : store_->events()) {
      if (!entry.per_plugin && entry.state == DeliveryState::dispatched) {
        if (!fail_event(entry.record.event_id, true)) {
          result.storage_error = true;
          return result;
        }
        ++result.count;
      }
    }
    return result;
  }

  [[nodiscard]] std::vector<std::string> dispatchable_events() const {
    const RuntimeTimePoint now = current_time();
    std::vector<std::string> result;
    for (const EventEntry& entry : store_->events()) {
      if (!entry.per_plugin &&
          (entry.state == DeliveryState::persisted ||
           (entry.state == DeliveryState::retrying && entry.next_attempt <= now))) {
        result.push_back(entry.record.event_id);
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  [[nodiscard]] std::vector<PluginDelivery>
  dispatchable_deliveries(std::string_view plugin_id) const {
    std::vector<PluginDelivery> result;
    const RuntimeTimePoint now = current_time();
    const std::vector<PluginDelivery> deliveries = store_->deliveries();
    for (const PluginDelivery& delivery : deliveries) {
      if (delivery.plugin_id == plugin_id && can_dispatch(delivery, deliveries, now)) {
        result.push_back(delivery);
      }
    }
    std::sort(result.begin(), result.end(),
              [](const PluginDelivery& left, const PluginDelivery& right) {
                return left.order < right.order;
              });
    return result;
  }

  [[nodiscard]] std::vector<PluginDelivery> plugin_dead_letters() const {
    std::vector<PluginDelivery> result;
    for (const PluginDelivery& delivery : store_->deliveries()) {
      if (delivery.state == DeliveryState::dead_letter) {
        result.push_back(delivery);
      }
    }
    std::sort(result.begin(), result.end(),
              [](const PluginDelivery& left, const PluginDelivery& right) {
                if (left.order != right.order) {
                  return left.order < right.order;
                }
                return left.plugin_id < right.plugin_id;
              });
    return result;
  }

  bool mark_plugin_dispatched(std::string_view event_id, std::string_view plugin_id) {
    return persist([&] {
      const auto delivery = store_->find_delivery(event_id, plugin_id);
      const RuntimeTimePoint now = current_time();
      if (!delivery.has_value() || !can_dispatch(*delivery, store_->deliveries(), now)) {
        return false;
      }
      PluginDelivery updated = *delivery;
      updated.state = DeliveryState::dispatched;
      updated.updated_at = now;
      return store_->update_delivery(std::move(updated)) && sync_event(event_id, now);
    });
  }

  bool acknowledge_plugin_event(std::string_view event_id, std::string_view plugin_id) {
    return persist([&] {
      const auto delivery = store_->find_delivery(event_id, plugin_id);
      if (!delivery.has_value() || delivery->state != DeliveryState::dispatched) {
        return false;
      }
      PluginDelivery updated = *delivery;
      updated.state = DeliveryState::acked;
      updated.updated_at = current_time();
      return store_->update_delivery(updated) && sync_event(event_id, updated.updated_at);
    });
  }

  bool fail_plugin_event(std::string_view event_id, std::string_view plugin_id, bool retryable) {
    return persist([&] {
      const auto delivery = store_->find_delivery(event_id, plugin_id);
      if (!delivery.has_value() || (delivery->state != DeliveryState::persisted &&
                                    delivery->state != DeliveryState::dispatched)) {
        return false;
      }
      PluginDelivery updated = *delivery;
      const bool exhausted = updated.attempts >= options_.max_attempts;
      if (updated.attempts < std::numeric_limits<std::size_t>::max()) {
        ++updated.attempts;
      }
      const RuntimeTimePoint now = current_time();
      if (!retryable || exhausted) {
        updated.state = DeliveryState::dead_letter;
      } else {
        updated.state = DeliveryState::retrying;
        updated.next_attempt = now + retry_delay_for_attempt(updated.attempts);
      }
      updated.updated_at = now;
      return store_->update_delivery(std::move(updated)) && sync_event(event_id, now);
    });
  }

  bool replay_plugin_dead_letter(std::string_view event_id, std::string_view plugin_id,
                                 std::string trace_id = {}) {
    return persist([&] {
      const auto event = store_->find_event(event_id);
      const auto delivery = store_->find_delivery(event_id, plugin_id);
      if (!event.has_value() || !event->per_plugin || !delivery.has_value() ||
          delivery->state != DeliveryState::dead_letter ||
          ((event->state == DeliveryState::acked || event->state == DeliveryState::dead_letter) &&
           store_->pending_event_count() >= options_.queue_limit)) {
        return false;
      }
      PluginDelivery updated = *delivery;
      if (trace_id.empty()) {
        trace_id = updated.trace_id + ".replay";
      }
      if (trace_id == updated.trace_id) {
        return false;
      }
      const RuntimeTimePoint now = current_time();
      updated.trace_id = std::move(trace_id);
      updated.state = DeliveryState::persisted;
      updated.attempts = 0;
      updated.next_attempt = now;
      updated.updated_at = now;
      return store_->update_delivery(std::move(updated)) && sync_event(event_id, now);
    });
  }

  BatchResult recover_plugin_inflight(std::string_view plugin_id) {
    BatchResult result;
    for (const PluginDelivery& delivery : store_->deliveries()) {
      if (delivery.plugin_id == plugin_id && delivery.state == DeliveryState::dispatched) {
        if (!fail_plugin_event(delivery.event_id, plugin_id, true)) {
          result.storage_error = true;
          return result;
        }
        ++result.count;
      }
    }
    return result;
  }

  [[nodiscard]] std::vector<EventEntry> dead_letters() const {
    std::vector<EventEntry> result;
    for (const EventEntry& entry : store_->events()) {
      if (entry.state == DeliveryState::dead_letter) {
        result.push_back(entry);
      }
    }
    std::sort(result.begin(), result.end(), [](const EventEntry& left, const EventEntry& right) {
      return left.record.event_id < right.record.event_id;
    });
    return result;
  }

  [[nodiscard]] std::size_t dead_letter_size() const {
    return dead_letters().size();
  }

  bool replay_dead_letter(std::string_view event_id, std::string trace_id = {}) {
    return persist([&] {
      const auto entry = store_->find_event(event_id);
      if (!entry.has_value() || entry->per_plugin || entry->state != DeliveryState::dead_letter ||
          store_->pending_event_count() >= options_.queue_limit) {
        return false;
      }
      EventEntry updated = *entry;
      updated.state = DeliveryState::persisted;
      updated.attempts = 0;
      updated.next_attempt = current_time();
      updated.updated_at = updated.next_attempt;
      if (trace_id.empty()) {
        trace_id = updated.record.trace_id + ".replay";
      }
      if (trace_id == updated.record.trace_id) {
        return false;
      }
      updated.record.trace_id = std::move(trace_id);
      return store_->update_event(std::move(updated));
    });
  }

  BatchResult purge_expired() {
    const RuntimeTimePoint cutoff = current_time() - options_.retention;
    BatchResult result;
    for (const EventEntry& entry : store_->events()) {
      const bool terminal =
          entry.state == DeliveryState::acked || entry.state == DeliveryState::dead_letter;
      if (terminal && entry.updated_at <= cutoff) {
        bool removed = false;
        if (!persist([&] {
              const auto current = store_->find_event(entry.record.event_id);
              if (!current.has_value() ||
                  (current->state != DeliveryState::acked &&
                   current->state != DeliveryState::dead_letter) ||
                  current->updated_at > cutoff) {
                return true;
              }
              removed = store_->erase_event(entry.record.event_id);
              return removed;
            })) {
          result.storage_error = true;
          return result;
        }
        if (removed) {
          ++result.count;
        }
      }
    }
    for (const ActionEntry& entry : store_->actions()) {
      const bool terminal = entry.state != ActionState::accepted;
      if (terminal && entry.updated_at <= cutoff) {
        bool removed = false;
        if (!persist([&] {
              const auto current = store_->find_action_by_idempotency(entry.record.idempotency_key);
              if (!current.has_value() || current->state == ActionState::accepted ||
                  current->updated_at > cutoff) {
                return true;
              }
              removed = store_->erase_action(entry.record.idempotency_key);
              return removed;
            })) {
          result.storage_error = true;
          return result;
        }
        if (removed) {
          ++result.count;
        }
      }
    }
    return result;
  }

  ActionSubmission submit_action(ActionRecord action) {
    if (action.action_id.empty() || action.idempotency_key.empty()) {
      return {ActionResult::invalid, std::nullopt};
    }
    std::unique_ptr<UnitOfWork> transaction = store_->begin_unit_of_work();
    if (!transaction) {
      return {ActionResult::storage_error, std::nullopt};
    }
    const auto existing = store_->find_action_by_idempotency(action.idempotency_key);
    if (existing.has_value()) {
      if (existing->state == ActionState::accepted && existing->record.deadline <= current_time()) {
        ActionEntry expired = *existing;
        expired.state = ActionState::expired;
        expired.updated_at = current_time();
        if (!store_->update_action(expired) || !transaction->commit()) {
          transaction->rollback();
          return {ActionResult::storage_error, std::nullopt};
        }
        return {ActionResult::duplicate, expired};
      }
      transaction->rollback();
      return {ActionResult::duplicate, existing};
    }
    const RuntimeTimePoint now = current_time();
    const bool expired = action.deadline <= now;
    ActionEntry entry{
        std::move(action), expired ? ActionState::expired : ActionState::accepted, now, {}, {}};
    const StoreInsertResult insertion = store_->insert_action(entry);
    if (insertion != StoreInsertResult::inserted) {
      transaction->rollback();
      if (insertion == StoreInsertResult::duplicate &&
          store_->find_action_by_idempotency(entry.record.idempotency_key).has_value()) {
        return submit_action(entry.record);
      }
      return {ActionResult::storage_error, std::nullopt};
    }
    if (!transaction->commit()) {
      transaction->rollback();
      return {ActionResult::storage_error, std::nullopt};
    }
    return {expired ? ActionResult::expired : ActionResult::accepted, entry};
  }

  [[nodiscard]] std::optional<ActionEntry> find_action(std::string_view idempotency_key) const {
    return store_->find_action_by_idempotency(idempotency_key);
  }

  bool complete_action(std::string_view idempotency_key, bool succeeded) {
    return update_action(idempotency_key, succeeded ? ActionState::succeeded : ActionState::failed,
                         "", "");
  }

  bool complete_action(std::string_view idempotency_key, ActionState state, std::string result,
                       std::string error_code = {}) {
    return update_action(idempotency_key, state, std::move(error_code), std::move(result));
  }

  bool cancel_action(std::string_view idempotency_key) {
    return update_action(idempotency_key, ActionState::cancelled, "", "");
  }

  BatchResult expire_actions() {
    const RuntimeTimePoint now = current_time();
    BatchResult result;
    for (const ActionEntry& entry : store_->actions()) {
      if (entry.state == ActionState::accepted && entry.record.deadline <= now) {
        bool changed = false;
        if (!persist([&] {
              const auto current = store_->find_action_by_idempotency(entry.record.idempotency_key);
              if (!current.has_value() || current->state != ActionState::accepted ||
                  current->record.deadline > now) {
                return true;
              }
              ActionEntry updated = *current;
              updated.state = ActionState::expired;
              updated.updated_at = now;
              changed = store_->update_action(std::move(updated));
              return changed;
            })) {
          result.storage_error = true;
          return result;
        }
        if (changed) {
          ++result.count;
        }
      }
    }
    return result;
  }

  bool register_plugin(std::string plugin_id, std::string api_version = {},
                       std::vector<std::string> permissions = {}, bool ordered = false) {
    if (plugin_id.empty()) {
      return false;
    }
    std::sort(permissions.begin(), permissions.end());
    const auto existing = store_->find_plugin(plugin_id);
    if (existing.has_value() && existing->state == PluginState::unavailable) {
      return plugin_reconnected(plugin_id, api_version, std::move(permissions), ordered);
    }
    if (existing.has_value() && existing->state != PluginState::failed) {
      return false;
    }
    return persist([&] {
      return store_->upsert_plugin(
          std::move(plugin_id),
          PluginEntry{
              PluginState::ready, {}, std::move(api_version), std::move(permissions), ordered});
    });
  }

  bool plugin_started(std::string_view plugin_id) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value() || entry->state != PluginState::ready) {
      return false;
    }
    PluginEntry updated = *entry;
    updated.state = PluginState::running;
    updated.disconnected_at = {};
    return persist([&] { return store_->upsert_plugin(std::string(plugin_id), updated); });
  }

  bool plugin_reconnected(std::string_view plugin_id, std::string_view api_version = {},
                          std::vector<std::string> permissions = {}, bool ordered = false) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value() || entry->state != PluginState::unavailable) {
      return false;
    }
    std::sort(permissions.begin(), permissions.end());
    if (entry->api_version != api_version || entry->permissions != permissions ||
        entry->ordered != ordered) {
      return false;
    }
    if (current_time() - entry->disconnected_at >= options_.plugin_failure_after) {
      update_plugin_recovery(plugin_id);
      return false;
    }
    PluginEntry updated = *entry;
    updated.state = PluginState::running;
    updated.disconnected_at = {};
    return persist([&] { return store_->upsert_plugin(std::string(plugin_id), updated); });
  }

  bool plugin_disconnected(std::string_view plugin_id) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value()) {
      return false;
    }
    if (entry->state == PluginState::unavailable) {
      return true;
    }
    if (entry->state == PluginState::failed) {
      return false;
    }
    PluginEntry updated = *entry;
    updated.state = PluginState::unavailable;
    updated.disconnected_at = current_time();
    return persist([&] { return store_->upsert_plugin(std::string(plugin_id), updated); });
  }

  bool update_plugin_recovery(std::string_view plugin_id) {
    const auto entry = store_->find_plugin(plugin_id);
    if (!entry.has_value() || entry->state != PluginState::unavailable) {
      return false;
    }
    if (current_time() - entry->disconnected_at < options_.plugin_failure_after) {
      return true;
    }
    PluginEntry updated = *entry;
    updated.state = PluginState::failed;
    return persist([&] { return store_->upsert_plugin(std::string(plugin_id), updated); });
  }

  [[nodiscard]] std::chrono::milliseconds retry_delay_for_attempt(std::size_t attempt) const {
    const std::size_t exponent = attempt > 0 ? std::min<std::size_t>(attempt - 1, 4) : 0;
    const std::uint64_t base_seconds = std::uint64_t{1} << exponent;
    double sample;
    if (random_) {
      sample = random_();
    } else {
      if (!random_engine_.has_value()) {
        random_engine_.emplace(std::random_device{}());
      }
      sample = std::uniform_real_distribution<double>(0.0, 1.0)(*random_engine_);
    }
    if (!std::isfinite(sample)) {
      sample = 0.5;
    }
    sample = std::clamp(sample, 0.0, 1.0);
    const double multiplier = 1.0 + ((sample * 2.0) - 1.0) * 0.20;
    const auto milliseconds = static_cast<std::int64_t>(
        std::llround(static_cast<double>(base_seconds) * 1000.0 * multiplier));
    return std::chrono::milliseconds(std::max<std::int64_t>(1, milliseconds));
  }

private:
  bool insert_deliveries(const std::string& event_id, const std::string& session_key,
                         const std::string& trace_id, std::optional<std::uint64_t> order,
                         const std::vector<std::pair<std::string, bool>>& targets,
                         RuntimeTimePoint now) {
    if (targets.empty()) {
      return true;
    }
    if (!order.has_value()) {
      return false;
    }
    for (const auto& [plugin_id, ordered] : targets) {
      PluginDelivery delivery{
          event_id, plugin_id, session_key, trace_id, *order, ordered, DeliveryState::persisted,
          0,        now,       now};
      if (store_->insert_delivery(std::move(delivery)) != StoreInsertResult::inserted) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool can_dispatch(const PluginDelivery& delivery,
                                  const std::vector<PluginDelivery>& deliveries,
                                  RuntimeTimePoint now) const {
    const auto plugin = store_->find_plugin(delivery.plugin_id);
    if (!plugin.has_value() || plugin->state != PluginState::running ||
        (delivery.state != DeliveryState::persisted && delivery.state != DeliveryState::retrying) ||
        (delivery.state == DeliveryState::retrying && delivery.next_attempt > now)) {
      return false;
    }
    if (delivery.session_key.empty()) {
      return true;
    }
    for (const PluginDelivery& other : deliveries) {
      if (other.plugin_id != delivery.plugin_id || other.session_key != delivery.session_key ||
          other.event_id == delivery.event_id) {
        continue;
      }
      if (!delivery.ordered && !other.ordered && !plugin->ordered) {
        continue;
      }
      const bool terminal =
          other.state == DeliveryState::acked || other.state == DeliveryState::dead_letter;
      if (other.state == DeliveryState::dispatched || (other.order < delivery.order && !terminal)) {
        return false;
      }
    }
    return true;
  }

  bool sync_event(std::string_view event_id, RuntimeTimePoint now) {
    const auto event = store_->find_event(event_id);
    if (!event.has_value() || !event->per_plugin) {
      return false;
    }
    bool found = false;
    bool all_terminal = true;
    bool any_dead = false;
    bool any_dispatched = false;
    bool any_retrying = false;
    for (const PluginDelivery& delivery : store_->deliveries()) {
      if (delivery.event_id != event_id) {
        continue;
      }
      found = true;
      any_dead = any_dead || delivery.state == DeliveryState::dead_letter;
      any_dispatched = any_dispatched || delivery.state == DeliveryState::dispatched;
      any_retrying = any_retrying || delivery.state == DeliveryState::retrying;
      if (delivery.state != DeliveryState::acked && delivery.state != DeliveryState::dead_letter) {
        all_terminal = false;
      }
    }
    if (!found) {
      return false;
    }
    EventEntry updated = *event;
    if (all_terminal) {
      updated.state = any_dead ? DeliveryState::dead_letter : DeliveryState::acked;
    } else if (any_dispatched) {
      updated.state = DeliveryState::dispatched;
    } else if (any_retrying) {
      updated.state = DeliveryState::retrying;
    } else {
      updated.state = DeliveryState::persisted;
    }
    updated.updated_at = now;
    return store_->update_event(std::move(updated));
  }

  template <typename Operation> bool persist(Operation&& operation) {
    std::unique_ptr<UnitOfWork> transaction = store_->begin_unit_of_work();
    if (!transaction) {
      return false;
    }
    if (!operation()) {
      transaction->rollback();
      return false;
    }
    if (!transaction->commit()) {
      transaction->rollback();
      return false;
    }
    return true;
  }

  bool update_action(std::string_view idempotency_key, ActionState state, std::string error_code,
                     std::string result) {
    if (state == ActionState::accepted || state == ActionState::expired) {
      return false;
    }
    bool completed = false;
    const bool saved = persist([&] {
      const auto entry = store_->find_action_by_idempotency(idempotency_key);
      if (!entry.has_value() || entry->state != ActionState::accepted) {
        return false;
      }
      ActionEntry updated = *entry;
      const RuntimeTimePoint now = current_time();
      if (updated.record.deadline <= now) {
        updated.state = ActionState::expired;
      } else {
        updated.state = state;
        updated.error_code = std::move(error_code);
        updated.result = std::move(result);
        completed = true;
      }
      updated.updated_at = now;
      return store_->update_action(std::move(updated));
    });
    return saved && completed;
  }

  [[nodiscard]] RuntimeTimePoint current_time() const {
    return clock_ ? clock_() : std::chrono::system_clock::now();
  }

  RuntimeStatePort* store_;
  RuntimeOptions options_;
  RuntimeClock clock_;
  RuntimeRandom random_;
  mutable std::optional<std::mt19937_64> random_engine_;
};

} // namespace kiarabot::core::detail
