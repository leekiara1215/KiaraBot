#include <iostream>
#include <stdexcept>

#include "kiarabot/core/runtime/in_memory_runtime_store.hpp"
#include "kiarabot/core/application/runtime.h"

namespace {
void check(bool result) {
  if (!result) {
    throw std::runtime_error("Runtime store assertion failed");
  }
}
template <typename T> T value(kiarabot::core::StorageResult<T> result) {
  check(std::holds_alternative<T>(result));
  return std::get<T>(std::move(result));
}
} // namespace
int main() {
  using kiarabot::core::ActionResult;
  using kiarabot::core::DeliveryState;
  using kiarabot::core::EventResult;
  using kiarabot::core::InMemoryRuntimeStore;
  using kiarabot::core::Runtime;
  using kiarabot::core::RuntimeTimePoint;
  using kiarabot::core::StorageError;
  try {
    InMemoryRuntimeStore store;
    auto now = RuntimeTimePoint{} + std::chrono::hours(1);
    Runtime runtime(store, {}, [&] { return now; }, [] { return 0.5; });
    check(!value(runtime.find_action("missing")).has_value());
    store.set_fail_reads(true);
    check(std::get<StorageError>(runtime.find_action("missing")) == StorageError::Unavailable);
    check(std::get<StorageError>(runtime.ingest({"event", "trace", "session", 1, true})) ==
          StorageError::Unavailable);
    store.set_fail_reads(false);
    check(value(runtime.register_plugin("a", "v1", {}, true)));
    check(value(runtime.plugin_started("a")));
    store.set_fail_commits(true);
    check(std::get<StorageError>(runtime.ingest({"event", "trace", "session", 1, true}, {"a"})) ==
          StorageError::Unavailable);
    check(value(runtime.inbox_size()) == 0);
    check(!value(runtime.session_status("session")).has_value());
    store.set_fail_commits(false);
    check(value(runtime.ingest({"event", "trace", "session", 1, true}, {"a"})) ==
          EventResult::accepted);
    check(value(runtime.ingest({"event", "trace", "session", 1, true}, {"a"})) ==
          EventResult::duplicate);
    check(value(runtime.dispatchable_deliveries("a")).size() == 1);
    check(value(store.read()).delivery_order == 1);
    check(value(runtime.mark_plugin_dispatched("event", "a")));
    check(value(runtime.acknowledge_plugin_event("event", "a")));
    check(value(store.read()).events.at("event").state == DeliveryState::acked);
    check(value(runtime.submit_action({"action", "key", now + std::chrono::minutes(1)})).result ==
          ActionResult::accepted);
    check(value(runtime.submit_action({"other", "key", now + std::chrono::minutes(1)})).result ==
          ActionResult::duplicate);
    {
      auto abandoned = value(store.begin());
      abandoned->snapshot().events.clear();
    }
    check(value(runtime.inbox_size()) == 1);
    auto first = value(store.begin());
    auto stale = value(store.begin());
    first->snapshot().delivery_order = 9;
    value(first->commit());
    stale->snapshot().delivery_order = 99;
    check(std::get<StorageError>(stale->commit()) == StorageError::Conflict);
    check(value(store.read()).delivery_order == 9);
    check(std::get<StorageError>(first->commit()) == StorageError::Conflict);
    now += std::chrono::hours(24 * 8);
    store.set_fail_commits(true);
    check(std::get<StorageError>(runtime.purge_expired()) == StorageError::Unavailable);
    check(value(runtime.inbox_size()) == 1);
    check(value(store.read()).deliveries.at("event").size() == 1);
    store.set_fail_commits(false);
    check(value(runtime.purge_expired()).count == 1);
    check(value(store.read()).deliveries.empty());
    check(value(runtime.session_status("session"))->last_sequence == 1);
    Runtime sampled({}, {}, [sample = 0.0]() mutable {
      const double result = sample;
      sample = 1.0;
      return result;
    });
    check(value(sampled.retry_delay_for_attempt(1)) == std::chrono::milliseconds(800));
    check(value(sampled.retry_delay_for_attempt(1)) == std::chrono::milliseconds(1200));
    std::cout << "Runtime read errors, atomic rollback, duplicate keys and conflicts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
