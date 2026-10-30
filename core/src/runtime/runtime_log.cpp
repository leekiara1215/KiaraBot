#include "kiarabot/core/runtime/runtime_log.hpp"

#include <array>
#include <stdexcept>

namespace kiarabot::core {
namespace {
std::string identifier(std::string_view input) {
  std::string result;
  for (const unsigned char ch : input.substr(0, 256)) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '.' || ch == '-' || ch == '_' || ch == '/' || ch == ':') {
      result += static_cast<char>(ch);
    } else {
      result += '_';
    }
  }
  return result;
}
std::string storage_code(StorageError error) {
  return error == StorageError::Conflict  ? "CONFLICT"
         : error == StorageError::Corrupt ? "INTERNAL"
                                          : "UPSTREAM_UNAVAILABLE";
}
} // namespace
struct LoggingRuntimeStore::State {
  std::shared_ptr<IRuntimeLogSink> sink;
  RuntimeClock clock;
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<std::uint64_t> failures{0};
  void emit(std::string operation, std::string outcome, std::string resource = {},
            std::string request = {}, std::string trace = {}, std::string level = "INFO") noexcept {
    try {
      const RuntimeLog record{std::to_string(++sequence), clock(),
                              std::move(level),           "core",
                              identifier(operation),      identifier(outcome),
                              identifier(resource),       identifier(request),
                              identifier(trace)};
      if (!sink || !sink->write(record)) {
        ++failures;
      }
    } catch (...) {
      ++failures;
    }
  }
};
class LoggingRuntimeStore::Transaction final : public RuntimeTransaction {
public:
  Transaction(std::unique_ptr<RuntimeTransaction> transaction, std::shared_ptr<State> state)
      : transaction_(std::move(transaction)), state_(std::move(state)),
        before_(transaction_->snapshot()) {}
  RuntimeSnapshot& snapshot() override {
    return transaction_->snapshot();
  }
  StorageStatus commit() override {
    const auto result = transaction_->commit();
    if (const auto* failure = std::get_if<StorageError>(&result)) {
      state_->emit("storage.commit", storage_code(*failure), {}, {}, {}, "ERROR");
      return result;
    }
    // Observation failures cannot change the successful transaction result.
    try {
      const auto& after = transaction_->snapshot();
      for (std::size_t index = before_.audit.size(); index < after.audit.size(); ++index) {
        const auto& entry = after.audit[index];
        state_->emit(entry.permission, entry.outcome, entry.resource, entry.request_id,
                     entry.trace_id, entry.allowed ? "INFO" : "WARN");
      }
      for (const auto& [id, event] : after.events) {
        if (!before_.events.contains(id)) {
          state_->emit("event.persist", "OK", id, {}, event.record.trace_id);
        }
      }
      for (const auto& [id, deliveries] : after.deliveries) {
        for (const auto& [plugin, delivery] : deliveries) {
          const auto previous = before_.deliveries.find(id);
          if (previous != before_.deliveries.end() && previous->second.contains(plugin) &&
              previous->second.at(plugin).state == delivery.state &&
              previous->second.at(plugin).attempts == delivery.attempts) {
            continue;
          }
          const auto outcome = delivery.state == DeliveryState::retrying      ? "RETRYING"
                               : delivery.state == DeliveryState::dead_letter ? "DEAD_LETTER"
                               : delivery.state == DeliveryState::acked       ? "ACKED"
                               : delivery.state == DeliveryState::dispatched  ? "DISPATCHED"
                                                                              : "PERSISTED";
          state_->emit("event.delivery", outcome, plugin, id, delivery.trace_id,
                       delivery.state == DeliveryState::dead_letter ? "ERROR" : "INFO");
        }
      }
      for (const auto& [id, action] : after.actions) {
        const auto found = before_.actions.find(id);
        if (found != before_.actions.end() && found->second.state == action.state) {
          continue;
        }
        constexpr std::array names{"ACCEPTED", "SUCCEEDED", "FAILED", "EXPIRED", "CANCELLED"};
        state_->emit("action.state", names.at(static_cast<std::size_t>(action.state)),
                     action.record.action_id);
      }
    } catch (...) {
      ++state_->failures;
    }
    return result;
  }

private:
  std::unique_ptr<RuntimeTransaction> transaction_;
  std::shared_ptr<State> state_;
  RuntimeSnapshot before_;
};
LoggingRuntimeStore::LoggingRuntimeStore(std::unique_ptr<RuntimeStore> store,
                                         std::shared_ptr<IRuntimeLogSink> sink, RuntimeClock clock)
    : store_(std::move(store)), state_(std::make_shared<State>()) {
  if (!store_ || !sink) {
    throw std::invalid_argument("logging store dependencies required");
  }
  state_->sink = std::move(sink);
  state_->clock =
      clock ? std::move(clock) : RuntimeClock([] { return std::chrono::system_clock::now(); });
}
StorageResult<RuntimeSnapshot> LoggingRuntimeStore::read() const {
  auto result = store_->read();
  if (const auto* failure = std::get_if<StorageError>(&result)) {
    state_->emit("storage.read", storage_code(*failure), {}, {}, {}, "ERROR");
  }
  return result;
}
StorageResult<std::unique_ptr<RuntimeTransaction>> LoggingRuntimeStore::begin() {
  auto result = store_->begin();
  if (const auto* failure = std::get_if<StorageError>(&result)) {
    state_->emit("storage.begin", storage_code(*failure), {}, {}, {}, "ERROR");
    return *failure;
  }
  return std::unique_ptr<RuntimeTransaction>(std::make_unique<Transaction>(
      std::get<std::unique_ptr<RuntimeTransaction>>(std::move(result)), state_));
}
std::uint64_t LoggingRuntimeStore::sink_failures() const {
  return state_->failures.load();
}
} // namespace kiarabot::core
