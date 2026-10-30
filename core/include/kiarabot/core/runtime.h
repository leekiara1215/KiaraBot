#pragma once

#include "kiarabot/core/runtime_engine.hpp"
#include "kiarabot/core/runtime_store.hpp"

namespace kiarabot::core {

// Serial application facade. All reads distinguish absence from storage failure.
// Every mutation reads and commits a single snapshot. Conflict is never auto-retried.
class Runtime {
public:
  explicit Runtime(RuntimeOptions options = {}, RuntimeClock clock = {}, RuntimeRandom random = {})
      : store_(&owned_), options_(options), clock_(std::move(clock)), random_(std::move(random)) {}
  explicit Runtime(RuntimeStore& store, RuntimeOptions options = {}, RuntimeClock clock = {},
                   RuntimeRandom random = {})
      : store_(&store), options_(options), clock_(std::move(clock)), random_(std::move(random)) {}
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;
  StorageResult<EventResult> ingest(EventRecord event,
                                    const std::vector<std::string>& plugin_ids = {}) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.ingest(std::move(event), plugin_ids); });
  }
  StorageResult<std::size_t> inbox_size() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.inbox_size(); });
  }
  StorageResult<std::size_t> pending_size() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.pending_size(); });
  }
  StorageResult<bool> sequence_gap_detected() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.sequence_gap_detected(); });
  }
  StorageResult<bool> session_degraded(std::string_view session_key) const {
    return query(
        [&](detail::RuntimeEngine& engine) { return engine.session_degraded(session_key); });
  }
  StorageResult<std::optional<SessionStatus>> session_status(std::string_view session_key) const {
    return query([&](detail::RuntimeEngine& engine) { return engine.session_status(session_key); });
  }
  StorageResult<bool> mark_dispatched(std::string_view event_id) {
    return mutate([&](detail::RuntimeEngine& engine) { return engine.mark_dispatched(event_id); });
  }
  StorageResult<bool> acknowledge_event(std::string_view event_id) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.acknowledge_event(event_id); });
  }
  StorageResult<bool> fail_event(std::string_view event_id, bool retryable) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.fail_event(event_id, retryable); });
  }
  StorageResult<std::vector<std::string>> due_retries() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.due_retries(); });
  }
  StorageResult<BatchResult> recover_inflight_events() {
    return mutate([&](detail::RuntimeEngine& engine) { return engine.recover_inflight_events(); });
  }
  StorageResult<std::vector<std::string>> dispatchable_events() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.dispatchable_events(); });
  }
  StorageResult<std::vector<PluginDelivery>>
  dispatchable_deliveries(std::string_view plugin_id) const {
    return query(
        [&](detail::RuntimeEngine& engine) { return engine.dispatchable_deliveries(plugin_id); });
  }
  StorageResult<std::vector<PluginDelivery>> plugin_dead_letters() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.plugin_dead_letters(); });
  }
  StorageResult<bool> mark_plugin_dispatched(std::string_view event_id,
                                             std::string_view plugin_id) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.mark_plugin_dispatched(event_id, plugin_id);
    });
  }
  StorageResult<bool> acknowledge_plugin_event(std::string_view event_id,
                                               std::string_view plugin_id) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.acknowledge_plugin_event(event_id, plugin_id);
    });
  }
  StorageResult<bool> fail_plugin_event(std::string_view event_id, std::string_view plugin_id,
                                        bool retryable) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.fail_plugin_event(event_id, plugin_id, retryable);
    });
  }
  StorageResult<bool> replay_plugin_dead_letter(std::string_view event_id,
                                                std::string_view plugin_id,
                                                std::string trace_id = {}) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.replay_plugin_dead_letter(event_id, plugin_id, std::move(trace_id));
    });
  }
  StorageResult<BatchResult> recover_plugin_inflight(std::string_view plugin_id) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.recover_plugin_inflight(plugin_id); });
  }
  StorageResult<std::vector<EventEntry>> dead_letters() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.dead_letters(); });
  }
  StorageResult<std::size_t> dead_letter_size() const {
    return query([&](detail::RuntimeEngine& engine) { return engine.dead_letter_size(); });
  }
  StorageResult<bool> replay_dead_letter(std::string_view event_id, std::string trace_id = {}) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.replay_dead_letter(event_id, std::move(trace_id));
    });
  }
  StorageResult<BatchResult> purge_expired() {
    return mutate([&](detail::RuntimeEngine& engine) { return engine.purge_expired(); });
  }
  StorageResult<ActionSubmission> submit_action(ActionRecord action) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.submit_action(std::move(action)); });
  }
  StorageResult<std::optional<ActionEntry>> find_action(std::string_view idempotency_key) const {
    return query(
        [&](detail::RuntimeEngine& engine) { return engine.find_action(idempotency_key); });
  }
  StorageResult<bool> complete_action(std::string_view idempotency_key, bool succeeded) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.complete_action(idempotency_key, succeeded);
    });
  }
  StorageResult<bool> complete_action(std::string_view idempotency_key, ActionState state,
                                      std::string result, std::string error_code = {}) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.complete_action(idempotency_key, state, std::move(result),
                                    std::move(error_code));
    });
  }
  StorageResult<bool> cancel_action(std::string_view idempotency_key) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.cancel_action(idempotency_key); });
  }
  StorageResult<BatchResult> expire_actions() {
    return mutate([&](detail::RuntimeEngine& engine) { return engine.expire_actions(); });
  }
  StorageResult<bool> register_plugin(std::string plugin_id, std::string api_version = {},
                                      std::vector<std::string> permissions = {},
                                      bool ordered = false) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.register_plugin(std::move(plugin_id), std::move(api_version),
                                    std::move(permissions), ordered);
    });
  }
  StorageResult<bool> plugin_started(std::string_view plugin_id) {
    return mutate([&](detail::RuntimeEngine& engine) { return engine.plugin_started(plugin_id); });
  }
  StorageResult<bool> plugin_reconnected(std::string_view plugin_id,
                                         std::string_view api_version = {},
                                         std::vector<std::string> permissions = {},
                                         bool ordered = false) {
    return mutate([&](detail::RuntimeEngine& engine) {
      return engine.plugin_reconnected(plugin_id, api_version, std::move(permissions), ordered);
    });
  }
  StorageResult<bool> plugin_disconnected(std::string_view plugin_id) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.plugin_disconnected(plugin_id); });
  }
  StorageResult<bool> update_plugin_recovery(std::string_view plugin_id) {
    return mutate(
        [&](detail::RuntimeEngine& engine) { return engine.update_plugin_recovery(plugin_id); });
  }
  StorageResult<std::chrono::milliseconds> retry_delay_for_attempt(std::size_t attempt) const {
    return query(
        [&](detail::RuntimeEngine& engine) { return engine.retry_delay_for_attempt(attempt); });
  }

private:
  template <typename Function>
  auto query(Function operation) const
      -> StorageResult<decltype(operation(std::declval<detail::RuntimeEngine&>()))> {
    auto loaded = store_->read();
    if (const auto* error = std::get_if<StorageError>(&loaded)) {
      return *error;
    }
    detail::RuntimeState state(std::get<RuntimeSnapshot>(std::move(loaded)));
    RuntimeClock clock = [this] { return clock_ ? clock_() : std::chrono::system_clock::now(); };
    RuntimeRandom random = random_ ? RuntimeRandom([this] { return random_(); }) : RuntimeRandom{};
    detail::RuntimeEngine engine(state, options_, std::move(clock), std::move(random));
    return operation(engine);
  }
  template <typename Function>
  auto mutate(Function operation)
      -> StorageResult<decltype(operation(std::declval<detail::RuntimeEngine&>()))> {
    auto started = store_->begin();
    if (const auto* error = std::get_if<StorageError>(&started)) {
      return *error;
    }
    auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(started));
    if (!transaction) {
      return StorageError::Unavailable;
    }
    detail::RuntimeState state(transaction->snapshot());
    RuntimeClock clock = [this] { return clock_ ? clock_() : std::chrono::system_clock::now(); };
    RuntimeRandom random = random_ ? RuntimeRandom([this] { return random_(); }) : RuntimeRandom{};
    detail::RuntimeEngine engine(state, options_, std::move(clock), std::move(random));
    auto result = operation(engine);
    auto updated = state.snapshot();
    updated.audit = std::move(transaction->snapshot().audit);
    updated.commands = std::move(transaction->snapshot().commands);
    transaction->snapshot() = std::move(updated);
    auto committed = transaction->commit();
    if (const auto* error = std::get_if<StorageError>(&committed)) {
      return *error;
    }
    return result;
  }
  InMemoryRuntimeStore owned_;
  RuntimeStore* store_;
  RuntimeOptions options_;
  RuntimeClock clock_;
  RuntimeRandom random_;
};
} // namespace kiarabot::core
