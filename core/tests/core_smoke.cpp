#include "kiarabot/core/runtime.h"

#include <cassert>
#include <chrono>

int main() {
  using namespace std::chrono_literals;
  using kiarabot::core::ActionRecord;
  using kiarabot::core::ActionResult;
  using kiarabot::core::ActionState;
  using kiarabot::core::DeliveryState;
  using kiarabot::core::EventRecord;
  using kiarabot::core::EventResult;
  using kiarabot::core::InMemoryRuntimeStore;
  using kiarabot::core::PluginState;
  using kiarabot::core::Runtime;
  using kiarabot::core::RuntimeOptions;
  using kiarabot::core::RuntimeTimePoint;

  RuntimeTimePoint now{};
  InMemoryRuntimeStore store;
  RuntimeOptions options;
  options.queue_limit = 32;
  options.max_attempts = 3;
  options.retention = 24h;
  options.plugin_failure_after = 5min;
  Runtime runtime(store, options, [&now] { return now; }, [] { return 0.5; });

  assert(runtime.ingest({"event-1", "trace-1"}) == EventResult::accepted);
  assert(runtime.ingest({"event-1", "trace-1"}) == EventResult::duplicate);
  assert(runtime.ingest({"", "trace-2"}) == EventResult::invalid);
  assert(runtime.inbox_size() == 1);

  assert(runtime.ingest({"event-2", "trace-2", "conversation-1", 1, true}) ==
         EventResult::accepted);
  assert(runtime.ingest({"event-3", "trace-3", "conversation-1", 3, true}) ==
         EventResult::accepted);
  assert(runtime.sequence_gap_detected());
  assert(runtime.ingest({"event-4", "trace-4", "conversation-1", 2, true}) ==
         EventResult::out_of_order);

  assert(runtime.mark_dispatched("event-2"));
  assert(runtime.fail_event("event-2", true));
  assert(store.find_event("event-2")->state == DeliveryState::retrying);
  assert(runtime.due_retries().empty());
  now += 1s;
  assert(runtime.due_retries().size() == 1);
  assert(runtime.mark_dispatched("event-2"));
  assert(runtime.fail_event("event-2", true));
  now += 2s;
  assert(runtime.due_retries().size() == 1);
  assert(runtime.mark_dispatched("event-2"));
  assert(runtime.fail_event("event-2", true));
  now += 4s;
  assert(runtime.due_retries().size() == 1);
  assert(runtime.mark_dispatched("event-2"));
  assert(runtime.fail_event("event-2", true));
  assert(runtime.dead_letter_size() == 1);

  assert(runtime.mark_dispatched("event-1"));
  assert(runtime.acknowledge_event("event-1"));
  now += 25h;
  assert(runtime.purge_expired() == 2);

  const ActionRecord action{"action-1", "idem-1", now + 10s};
  assert(runtime.submit_action(action) == ActionResult::accepted);
  assert(runtime.submit_action(action) == ActionResult::duplicate);
  now += 11s;
  assert(runtime.expire_actions() == 1);
  assert(store.find_action_by_idempotency("idem-1")->state == ActionState::expired);

  assert(runtime.register_plugin("plugin-1"));
  assert(runtime.plugin_started("plugin-1"));
  assert(runtime.plugin_disconnected("plugin-1"));
  now += 5min;
  assert(runtime.update_plugin_recovery("plugin-1"));
  assert(store.find_plugin("plugin-1")->state == PluginState::failed);

  // The store port keeps inbox state across Runtime instances.
  Runtime restarted(store, options, [&now] { return now; });
  assert(restarted.ingest({"event-3", "trace-3", "conversation-1", 3, true}) ==
         EventResult::duplicate);
  assert(restarted.ingest({"event-5", "trace-5", "conversation-1", 2, true}) ==
         EventResult::out_of_order);
  return 0;
}
