#include <cassert>
#include <chrono>
#include <string>
#include <vector>

#include "kiarabot/core/runtime_engine.hpp"

namespace {
using namespace std::chrono_literals;
using kiarabot::core::DeliveryState;
using kiarabot::core::EventResult;
using kiarabot::core::RuntimeOptions;
using kiarabot::core::RuntimeTimePoint;
using kiarabot::core::detail::RuntimeEngine;
using kiarabot::core::detail::RuntimeState;

void independent_delivery_and_order() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(store, {}, [&] { return now; }, [] { return 0.5; });
  assert(runtime.register_plugin("ordered", "v1", {}, true));
  assert(runtime.register_plugin("free"));
  assert(runtime.plugin_started("ordered"));
  assert(runtime.plugin_started("free"));

  assert(runtime.ingest({"first", "trace1", "session", 1, true, "envelope"}, {"ordered", "free"}) ==
         EventResult::accepted);
  assert(!runtime.mark_dispatched("first"));
  assert(!runtime.acknowledge_event("first"));
  assert(!runtime.fail_event("first", true));
  assert(runtime.ingest({"second", "trace2", "session", 2, true}, {"ordered", "free"}) ==
         EventResult::accepted);
  assert(runtime.ingest({"other", "trace3", "other-session", 1, true}, {"ordered"}) ==
         EventResult::accepted);
  assert(runtime.dispatchable_deliveries("ordered").size() == 2);
  assert(runtime.dispatchable_deliveries("free").size() == 2);
  assert(!runtime.mark_plugin_dispatched("second", "ordered"));
  assert(runtime.mark_plugin_dispatched("first", "ordered"));
  assert(runtime.mark_plugin_dispatched("other", "ordered"));
  assert(runtime.mark_plugin_dispatched("second", "free"));
  assert(runtime.mark_plugin_dispatched("first", "free"));
  assert(runtime.acknowledge_plugin_event("first", "free"));
  assert(runtime.fail_plugin_event("first", "ordered", true));
  assert(store.find_delivery("first", "free")->state == DeliveryState::acked);
  assert(store.find_delivery("first", "ordered")->state == DeliveryState::retrying);
  assert(!runtime.mark_plugin_dispatched("second", "ordered"));
  now += 1s;
  assert(runtime.mark_plugin_dispatched("first", "ordered"));
  assert(runtime.fail_plugin_event("first", "ordered", false));
  assert(runtime.plugin_dead_letters().size() == 1);
  assert(store.find_event("first")->state == DeliveryState::dead_letter);
  assert(runtime.mark_plugin_dispatched("second", "ordered"));
  assert(runtime.replay_plugin_dead_letter("first", "ordered", "replay-trace"));
  assert(store.find_delivery("first", "free")->state == DeliveryState::acked);
  assert(store.find_delivery("first", "ordered")->trace_id == "replay-trace");
  assert(store.find_event("first")->record.serialized_envelope == "envelope");
  assert(!runtime.mark_plugin_dispatched("first", "ordered"));
  assert(runtime.acknowledge_plugin_event("second", "ordered"));
  assert(runtime.mark_plugin_dispatched("first", "ordered"));
  assert(runtime.acknowledge_plugin_event("first", "ordered"));
  assert(store.find_event("first")->state == DeliveryState::acked);
  assert(runtime.acknowledge_plugin_event("second", "free"));
  assert(runtime.acknowledge_plugin_event("other", "ordered"));
  assert(runtime.pending_size() == 0);
  now += 7 * 24h;
  assert(runtime.purge_expired().count == 3);
  assert(!store.find_delivery("first", "free").has_value());
  assert(!store.find_delivery("first", "ordered").has_value());
}

void recovery_disconnect_and_jitter() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(store, {}, [&] { return now; }, [] { return 1.0; });
  assert(runtime.register_plugin("p", "v1", {}, true));
  assert(runtime.plugin_started("p"));
  assert(runtime.ingest({"event", "trace", "conversation", 1, true}, {"p"}) ==
         EventResult::accepted);
  assert(runtime.mark_plugin_dispatched("event", "p"));
  assert(runtime.plugin_disconnected("p"));
  assert(runtime.dispatchable_deliveries("p").empty());
  kiarabot::core::detail::RuntimeEngine restarted(
      store, {}, [&] { return now; }, [] { return 1.0; });
  store.set_fail_commit(true);
  assert(restarted.recover_plugin_inflight("p").storage_error);
  assert(store.find_delivery("event", "p")->state == DeliveryState::dispatched);
  store.set_fail_commit(false);
  assert(restarted.recover_plugin_inflight("p").count == 1);
  assert(store.find_delivery("event", "p")->next_attempt == now + 1200ms);
  assert(!restarted.plugin_reconnected("p", "v1", {}, false));
  assert(restarted.plugin_reconnected("p", "v1", {}, true));
  now += 1199ms;
  assert(restarted.dispatchable_deliveries("p").empty());
  now += 1ms;
  assert(restarted.mark_plugin_dispatched("event", "p"));
  assert(restarted.acknowledge_plugin_event("event", "p"));
  assert(restarted.recover_plugin_inflight("p").count == 0);
}

void transaction_failures_and_backpressure() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  RuntimeOptions options;
  options.queue_limit = 1;
  kiarabot::core::detail::RuntimeEngine runtime(
      store, options, [&] { return now; }, [] { return 0.5; });
  assert(runtime.register_plugin("p", {}, {}, true));
  assert(runtime.register_plugin("q"));
  assert(runtime.plugin_started("p"));
  assert(runtime.plugin_started("q"));
  assert(runtime.ingest({"unknown", "t", "s", 1, true}, {"missing"}) == EventResult::invalid);
  assert(runtime.ingest({"duplicate-target", "t", "s", 1, true}, {"p", "p"}) ==
         EventResult::invalid);
  store.set_fail_delivery_writes(true);
  assert(runtime.ingest({"lost", "t", "s", 1, true}, {"p", "q"}) == EventResult::storage_error);
  assert(!store.find_event("lost").has_value());
  assert(!store.find_session("s").has_value());
  store.set_fail_delivery_writes(false);
  store.set_fail_commit(true);
  assert(runtime.ingest({"lost", "t", "s", 1, true}, {"p", "q"}) == EventResult::storage_error);
  assert(!store.find_event("lost").has_value());
  assert(store.deliveries().empty());
  store.set_fail_commit(false);
  assert(runtime.ingest({"first", "t", "s", 1, true}, {"p", "q"}) == EventResult::accepted);
  assert(store.find_delivery("first", "p")->order == 1);
  assert(runtime.ingest({"first", "t", "s", 1, true}, {"p", "q"}) == EventResult::duplicate);
  assert(runtime.ingest({"overflow", "t", "s", 2, true}, {"p"}) == EventResult::rate_limited);
  store.set_fail_event_writes(true);
  assert(!runtime.mark_plugin_dispatched("first", "p"));
  assert(store.find_delivery("first", "p")->state == DeliveryState::persisted);
  store.set_fail_event_writes(false);
  assert(runtime.mark_plugin_dispatched("first", "p"));
  assert(runtime.fail_plugin_event("first", "p", false));
  assert(runtime.mark_plugin_dispatched("first", "q"));
  assert(runtime.acknowledge_plugin_event("first", "q"));
  assert(runtime.ingest({"second", "t", "s", 2, true}, {"q"}) == EventResult::accepted);
  assert(!runtime.replay_plugin_dead_letter("first", "p", "new"));
  assert(runtime.mark_plugin_dispatched("second", "q"));
  assert(runtime.acknowledge_plugin_event("second", "q"));
  assert(runtime.replay_plugin_dead_letter("first", "p", "new"));
  assert(store.find_delivery("first", "q")->state == DeliveryState::acked);
}

void retry_exhaustion_and_retention() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(store, {}, [&] { return now; }, [] { return 0.5; });
  assert(runtime.register_plugin("p"));
  assert(runtime.register_plugin("q"));
  assert(runtime.plugin_started("p"));
  assert(runtime.plugin_started("q"));
  assert(runtime.ingest({"event", "trace", "session", 1, true}, {"p", "q"}) ==
         EventResult::accepted);
  assert(runtime.mark_plugin_dispatched("event", "p"));
  for (const auto delay : {1s, 2s, 4s, 8s, 16s}) {
    assert(runtime.fail_plugin_event("event", "p", true));
    assert(store.find_delivery("event", "p")->state == DeliveryState::retrying);
    assert(!runtime.mark_plugin_dispatched("event", "p"));
    now += delay;
    assert(runtime.mark_plugin_dispatched("event", "p"));
  }
  assert(runtime.fail_plugin_event("event", "p", true));
  assert(store.find_delivery("event", "p")->state == DeliveryState::dead_letter);
  assert(store.find_delivery("event", "q")->attempts == 0);
  assert(store.find_event("event")->state != DeliveryState::dead_letter);
  now += 8 * 24h;
  assert(runtime.purge_expired().count == 0);
  assert(store.find_event("event").has_value());
  assert(runtime.mark_plugin_dispatched("event", "q"));
  assert(runtime.acknowledge_plugin_event("event", "q"));
  assert(store.find_event("event")->state == DeliveryState::dead_letter);
  now += 7 * 24h;
  assert(runtime.purge_expired().count == 1);
  assert(!store.find_delivery("event", "p").has_value());
  assert(!store.find_delivery("event", "q").has_value());
}

} // namespace

int main() {
  independent_delivery_and_order();
  recovery_disconnect_and_jitter();
  transaction_failures_and_backpressure();
  retry_exhaustion_and_retention();
}
