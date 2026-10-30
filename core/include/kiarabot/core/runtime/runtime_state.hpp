#pragma once

#include "kiarabot/core/domain/runtime_state_port.hpp"

namespace kiarabot::core::detail {

// Transaction-local adapter between RuntimeSnapshot and RuntimeStatePort.
// The outer RuntimeStore remains responsible for durable commits.
class RuntimeState final : public RuntimeStatePort {
private:
  class Transaction final : public UnitOfWork {
  public:
    explicit Transaction(RuntimeState& store)
        : store_(store), events_(store.events_), sessions_(store.sessions_),
          actions_(store.actions_), plugins_(store.plugins_), deliveries_(store.deliveries_),
          delivery_order_(store.delivery_order_) {}

    ~Transaction() override {
      rollback();
    }

    bool commit() override {
      if (finished_) {
        return committed_;
      }
      ++store_.commit_count_;
      if (store_.fail_commit_) {
        return false;
      }
      finished_ = true;
      committed_ = true;
      store_.transaction_active_ = false;
      return true;
    }

    void rollback() noexcept override {
      if (finished_) {
        return;
      }
      finished_ = true;
      store_.events_ = std::move(events_);
      store_.sessions_ = std::move(sessions_);
      store_.actions_ = std::move(actions_);
      store_.plugins_ = std::move(plugins_);
      store_.deliveries_ = std::move(deliveries_);
      store_.delivery_order_ = delivery_order_;
      store_.transaction_active_ = false;
      ++store_.rollback_count_;
    }

  private:
    RuntimeState& store_;
    std::unordered_map<std::string, EventEntry> events_;
    std::unordered_map<std::string, SessionStatus> sessions_;
    std::unordered_map<std::string, ActionEntry> actions_;
    std::unordered_map<std::string, PluginEntry> plugins_;
    std::unordered_map<std::string, std::unordered_map<std::string, PluginDelivery>> deliveries_;
    std::uint64_t delivery_order_;
    bool finished_{false};
    bool committed_{false};
  };

public:
  RuntimeSnapshot snapshot() const {
    return {events_, sessions_, actions_,    plugins_,       deliveries_, delivery_order_,
            {},      {},        namespaces_, host_receipts_, tasks_};
  }
  explicit RuntimeState(const RuntimeSnapshot& snapshot = {})
      : events_(snapshot.events), sessions_(snapshot.sessions), actions_(snapshot.actions),
        plugins_(snapshot.plugins), deliveries_(snapshot.deliveries),
        delivery_order_(snapshot.delivery_order), namespaces_(snapshot.namespaces),
        host_receipts_(snapshot.host_receipts), tasks_(snapshot.tasks) {}

  [[nodiscard]] std::unique_ptr<UnitOfWork> begin_transaction() override {
    if (transaction_active_) {
      return nullptr;
    }
    ++begin_count_;
    std::unique_ptr<UnitOfWork> transaction = std::make_unique<Transaction>(*this);
    transaction_active_ = true;
    return transaction;
  }

  [[nodiscard]] bool contains_event(std::string_view event_id) const override {
    return events_.contains(std::string(event_id));
  }

  [[nodiscard]] std::optional<EventEntry> find_event(std::string_view event_id) const override {
    const auto iterator = events_.find(std::string(event_id));
    if (iterator == events_.end()) {
      return std::nullopt;
    }
    return iterator->second;
  }

  [[nodiscard]] std::size_t pending_event_count() const override {
    std::size_t count = 0;
    for (const auto& [event_id, entry] : events_) {
      static_cast<void>(event_id);
      if (entry.state != DeliveryState::acked && entry.state != DeliveryState::dead_letter) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] std::vector<EventEntry> events() const override {
    std::vector<EventEntry> result;
    result.reserve(events_.size());
    for (const auto& [event_id, entry] : events_) {
      static_cast<void>(event_id);
      result.push_back(entry);
    }
    return result;
  }

  StoreInsertResult insert_event(EventEntry entry) override {
    if (fail_event_writes_) {
      return StoreInsertResult::failed;
    }
    const std::string event_id = entry.record.event_id;
    return events_.emplace(event_id, std::move(entry)).second ? StoreInsertResult::inserted
                                                              : StoreInsertResult::duplicate;
  }

  bool update_event(EventEntry entry) override {
    if (fail_event_writes_) {
      return false;
    }
    const auto iterator = events_.find(entry.record.event_id);
    if (iterator == events_.end()) {
      return false;
    }
    iterator->second = std::move(entry);
    return true;
  }

  bool erase_event(std::string_view event_id) override {
    if (fail_event_writes_) {
      return false;
    }
    if (events_.erase(std::string(event_id)) == 0) {
      return false;
    }
    deliveries_.erase(std::string(event_id));
    return true;
  }

  [[nodiscard]] std::optional<std::uint64_t> allocate_delivery_order() override {
    if (fail_delivery_writes_ || delivery_order_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    return ++delivery_order_;
  }

  [[nodiscard]] std::optional<PluginDelivery>
  find_delivery(std::string_view event_id, std::string_view plugin_id) const override {
    const auto event = deliveries_.find(std::string(event_id));
    if (event == deliveries_.end()) {
      return std::nullopt;
    }
    const auto delivery = event->second.find(std::string(plugin_id));
    if (delivery == event->second.end()) {
      return std::nullopt;
    }
    return delivery->second;
  }

  [[nodiscard]] std::vector<PluginDelivery> deliveries() const override {
    std::vector<PluginDelivery> result;
    for (const auto& [event_id, entries] : deliveries_) {
      static_cast<void>(event_id);
      for (const auto& [plugin_id, entry] : entries) {
        static_cast<void>(plugin_id);
        result.push_back(entry);
      }
    }
    return result;
  }

  StoreInsertResult insert_delivery(PluginDelivery delivery) override {
    if (fail_delivery_writes_) {
      return StoreInsertResult::failed;
    }
    const std::string event_id = delivery.event_id;
    const std::string plugin_id = delivery.plugin_id;
    return deliveries_[event_id].emplace(plugin_id, std::move(delivery)).second
               ? StoreInsertResult::inserted
               : StoreInsertResult::duplicate;
  }

  bool update_delivery(PluginDelivery delivery) override {
    if (fail_delivery_writes_) {
      return false;
    }
    const auto event = deliveries_.find(delivery.event_id);
    if (event == deliveries_.end()) {
      return false;
    }
    const auto current = event->second.find(delivery.plugin_id);
    if (current == event->second.end()) {
      return false;
    }
    current->second = std::move(delivery);
    return true;
  }

  [[nodiscard]] std::optional<SessionStatus>
  find_session(std::string_view session_key) const override {
    const auto iterator = sessions_.find(std::string(session_key));
    if (iterator == sessions_.end()) {
      return std::nullopt;
    }
    return iterator->second;
  }

  [[nodiscard]] std::vector<SessionStatus> sessions() const override {
    std::vector<SessionStatus> result;
    result.reserve(sessions_.size());
    for (const auto& [session_key, status] : sessions_) {
      static_cast<void>(session_key);
      result.push_back(status);
    }
    return result;
  }

  bool upsert_session(std::string session_key, SessionStatus status) override {
    if (fail_event_writes_ || fail_session_writes_) {
      return false;
    }
    sessions_[std::move(session_key)] = status;
    return true;
  }

  [[nodiscard]] std::optional<ActionEntry>
  find_action_by_idempotency(std::string_view idempotency_key) const override {
    const auto iterator = actions_.find(std::string(idempotency_key));
    if (iterator == actions_.end()) {
      return std::nullopt;
    }
    return iterator->second;
  }

  [[nodiscard]] std::vector<ActionEntry> actions() const override {
    std::vector<ActionEntry> result;
    result.reserve(actions_.size());
    for (const auto& [idempotency_key, entry] : actions_) {
      static_cast<void>(idempotency_key);
      result.push_back(entry);
    }
    return result;
  }

  StoreInsertResult insert_action(ActionEntry entry) override {
    if (fail_action_writes_) {
      return StoreInsertResult::failed;
    }
    const std::string idempotency_key = entry.record.idempotency_key;
    return actions_.emplace(idempotency_key, std::move(entry)).second
               ? StoreInsertResult::inserted
               : StoreInsertResult::duplicate;
  }

  bool update_action(ActionEntry entry) override {
    if (fail_action_writes_) {
      return false;
    }
    const auto iterator = actions_.find(entry.record.idempotency_key);
    if (iterator == actions_.end()) {
      return false;
    }
    iterator->second = std::move(entry);
    return true;
  }

  bool erase_action(std::string_view idempotency_key) override {
    if (fail_action_writes_) {
      return false;
    }
    return actions_.erase(std::string(idempotency_key)) != 0;
  }

  [[nodiscard]] std::optional<PluginEntry> find_plugin(std::string_view plugin_id) const override {
    const auto iterator = plugins_.find(std::string(plugin_id));
    if (iterator == plugins_.end()) {
      return std::nullopt;
    }
    return iterator->second;
  }

  bool upsert_plugin(std::string plugin_id, PluginEntry entry) override {
    if (fail_plugin_writes_) {
      return false;
    }
    plugins_[std::move(plugin_id)] = entry;
    return true;
  }

  void set_fail_event_writes(bool value) {
    fail_event_writes_ = value;
  }
  void set_fail_session_writes(bool value) {
    fail_session_writes_ = value;
  }
  void set_fail_delivery_writes(bool value) {
    fail_delivery_writes_ = value;
  }
  void set_fail_action_writes(bool value) {
    fail_action_writes_ = value;
  }
  void set_fail_plugin_writes(bool value) {
    fail_plugin_writes_ = value;
  }
  void set_fail_commit(bool value) {
    fail_commit_ = value;
  }

  [[nodiscard]] std::size_t begin_count() const noexcept {
    return begin_count_;
  }
  [[nodiscard]] std::size_t commit_count() const noexcept {
    return commit_count_;
  }
  [[nodiscard]] std::size_t rollback_count() const noexcept {
    return rollback_count_;
  }

private:
  std::unordered_map<std::string, EventEntry> events_;
  std::unordered_map<std::string, SessionStatus> sessions_;
  std::unordered_map<std::string, ActionEntry> actions_;
  std::unordered_map<std::string, PluginEntry> plugins_;
  std::unordered_map<std::string, std::unordered_map<std::string, PluginDelivery>> deliveries_;
  std::uint64_t delivery_order_{0};
  decltype(RuntimeSnapshot::namespaces) namespaces_;
  decltype(RuntimeSnapshot::host_receipts) host_receipts_;
  decltype(RuntimeSnapshot::tasks) tasks_;
  bool fail_event_writes_{false};
  bool fail_session_writes_{false};
  bool fail_delivery_writes_{false};
  bool fail_action_writes_{false};
  bool fail_plugin_writes_{false};
  bool fail_commit_{false};
  bool transaction_active_{false};
  std::size_t begin_count_{0};
  std::size_t commit_count_{0};
  std::size_t rollback_count_{0};
};

} // namespace kiarabot::core::detail
