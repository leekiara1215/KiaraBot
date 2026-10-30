#pragma once

#include "kiarabot/core/domain/runtime_types.hpp"

namespace kiarabot::core::detail {

// Transaction within the private in-memory state adapter; not a durable commit.
class UnitOfWork {
public:
  virtual ~UnitOfWork() = default;
  virtual bool commit() = 0;
  virtual void rollback() noexcept = 0;
};

// Internal state-machine interface over a transaction-local snapshot.
// Storage errors and durable commits are handled by the outer RuntimeStore.
class RuntimeStatePort {
public:
  virtual ~RuntimeStatePort() = default;

  [[nodiscard]] virtual std::unique_ptr<UnitOfWork> begin_transaction() = 0;

  [[nodiscard]] virtual std::unique_ptr<UnitOfWork> begin_unit_of_work() {
    return begin_transaction();
  }

  [[nodiscard]] virtual bool contains_event(std::string_view event_id) const = 0;
  [[nodiscard]] virtual std::optional<EventEntry> find_event(std::string_view event_id) const = 0;
  [[nodiscard]] virtual std::size_t pending_event_count() const = 0;
  [[nodiscard]] virtual std::vector<EventEntry> events() const = 0;
  virtual StoreInsertResult insert_event(EventEntry entry) = 0;
  virtual bool update_event(EventEntry entry) = 0;
  virtual bool erase_event(std::string_view event_id) = 0;

  [[nodiscard]] virtual std::optional<std::uint64_t> allocate_delivery_order() = 0;
  [[nodiscard]] virtual std::optional<PluginDelivery>
  find_delivery(std::string_view event_id, std::string_view plugin_id) const = 0;
  [[nodiscard]] virtual std::vector<PluginDelivery> deliveries() const = 0;
  virtual StoreInsertResult insert_delivery(PluginDelivery delivery) = 0;
  virtual bool update_delivery(PluginDelivery delivery) = 0;

  [[nodiscard]] virtual std::optional<SessionStatus>
  find_session(std::string_view session_key) const = 0;
  [[nodiscard]] virtual std::vector<SessionStatus> sessions() const = 0;
  virtual bool upsert_session(std::string session_key, SessionStatus status) = 0;

  [[nodiscard]] virtual std::optional<ActionEntry>
  find_action_by_idempotency(std::string_view idempotency_key) const = 0;
  [[nodiscard]] virtual std::vector<ActionEntry> actions() const = 0;
  virtual StoreInsertResult insert_action(ActionEntry entry) = 0;
  virtual bool update_action(ActionEntry entry) = 0;
  virtual bool erase_action(std::string_view idempotency_key) = 0;

  [[nodiscard]] virtual std::optional<PluginEntry>
  find_plugin(std::string_view plugin_id) const = 0;
  virtual bool upsert_plugin(std::string plugin_id, PluginEntry entry) = 0;
};

} // namespace kiarabot::core::detail
