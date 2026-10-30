#include <array>
#include <cassert>
#include <chrono>
#include <string>
#include <vector>

#include "kiarabot/core/domain/runtime_engine.hpp"
#include "kiarabot/core/runtime/runtime_state.hpp"

namespace {

using namespace std::chrono_literals;
using kiarabot::core::ActionRecord;
using kiarabot::core::ActionResult;
using kiarabot::core::ActionState;
using kiarabot::core::DeliveryState;
using kiarabot::core::EventResult;
using kiarabot::core::PluginState;
using kiarabot::core::RuntimeOptions;
using kiarabot::core::RuntimeTimePoint;
using kiarabot::core::StoreInsertResult;
using kiarabot::core::detail::RuntimeEngine;
using kiarabot::core::detail::RuntimeState;

void event_order_and_backpressure() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  RuntimeOptions options;
  options.queue_limit = 4;
  kiarabot::core::detail::RuntimeEngine runtime(
      store, options, [&] { return now; }, [] { return 0.5; });

  assert(runtime.ingest({"a1", "t1", "a", 1, true}) == EventResult::accepted);
  assert(runtime.ingest({"a1", "changed", "a", 1, true}) == EventResult::duplicate);
  assert(runtime.ingest({"a3", "t3", "a", 3, true}) == EventResult::accepted);
  assert(runtime.session_degraded("a"));
  assert(!runtime.session_degraded("b"));
  assert(runtime.ingest({"a2", "t2", "a", 2, true}) == EventResult::out_of_order);
  assert(runtime.ingest({"b1", "t4", "b", 1, true}) == EventResult::accepted);
  assert(runtime.ingest({"missing", "t5", "c", 0, false}) == EventResult::accepted);
  assert(runtime.session_degraded("c"));
  assert(runtime.ingest({"full", "t6", "b", 2, true}) == EventResult::rate_limited);
  assert(runtime.ingest({"a1", "t1", "a", 1, true}) == EventResult::duplicate);

  kiarabot::core::detail::RuntimeEngine restarted(
      store, options, [&] { return now; }, [] { return 0.5; });
  assert(restarted.session_degraded("a"));
  assert(restarted.session_degraded("c"));
  assert(!restarted.session_degraded("b"));
  assert(restarted.session_status("a")->last_sequence == 3);
  assert(restarted.mark_dispatched("a1"));
  assert(restarted.acknowledge_event("a1"));
  assert(restarted.mark_dispatched("a3"));
  assert(restarted.acknowledge_event("a3"));
  now += 7 * 24h;
  assert(restarted.purge_expired().count == 2);
  kiarabot::core::detail::RuntimeEngine after_purge(
      store, options, [&] { return now; }, [] { return 0.5; });
  assert(after_purge.session_degraded("a"));
  assert(after_purge.session_status("a")->last_sequence == 3);
  assert(after_purge.ingest({"late", "t", "a", 2, true}) == EventResult::out_of_order);
}

void retries_dead_letters_and_replay() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(
      store, {}, [&] { return now; }, [] { return 0.5; });
  const std::array delays{1s, 2s, 4s, 8s, 16s};
  assert(runtime.ingest({"retry", "trace", "", 0, false}) == EventResult::accepted);
  assert(runtime.mark_dispatched("retry"));
  assert(!runtime.mark_dispatched("retry"));
  for (const auto delay : delays) {
    assert(runtime.fail_event("retry", true));
    assert(store.find_event("retry")->state == DeliveryState::retrying);
    kiarabot::core::detail::RuntimeEngine restarted(
        store, {}, [&] { return now; }, [] { return 0.5; });
    assert(restarted.due_retries().empty());
    assert(runtime.due_retries().empty());
    now += delay - 1ms;
    assert(runtime.due_retries().empty());
    now += 1ms;
    assert(runtime.due_retries() == std::vector<std::string>{"retry"});
    assert(runtime.mark_dispatched("retry"));
  }
  assert(runtime.fail_event("retry", true));
  assert(runtime.dead_letters().size() == 1);
  assert(runtime.ingest({"fatal", "fatal-trace", "", 0, false}) == EventResult::accepted);
  assert(runtime.fail_event("fatal", false));
  assert(runtime.dead_letter_size() == 2);
  kiarabot::core::detail::RuntimeEngine restarted(
      store, {}, [&] { return now; }, [] { return 0.5; });
  assert(restarted.dead_letter_size() == 2);
  assert(runtime.replay_dead_letter("retry", "new-trace"));
  assert(store.find_event("retry")->record.trace_id == "new-trace");
  assert(store.find_event("retry")->attempts == 0);
  assert(store.find_event("retry")->state == DeliveryState::persisted);
  assert(!runtime.replay_dead_letter("retry", "another-trace"));
}

void jitter_bounds() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState low_store;
  kiarabot::core::detail::RuntimeState high_store;
  kiarabot::core::detail::RuntimeEngine low(
      low_store, {}, [&] { return now; }, [] { return 0.0; });
  kiarabot::core::detail::RuntimeEngine high(
      high_store, {}, [&] { return now; }, [] { return 1.0; });
  const std::array bases{1000ms, 2000ms, 4000ms, 8000ms, 16000ms};
  for (std::size_t attempt = 1; attempt <= bases.size(); ++attempt) {
    assert(low.retry_delay_for_attempt(attempt) == bases[attempt - 1] * 8 / 10);
    assert(high.retry_delay_for_attempt(attempt) == bases[attempt - 1] * 12 / 10);
  }
}

void actions_and_retention() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(
      store, {}, [&] { return now; }, [] { return 0.5; });
  const ActionRecord action{"action", "key", now + 1h};
  const auto accepted = runtime.submit_action(action);
  assert(accepted.result == ActionResult::accepted);
  assert(accepted.action->state == ActionState::accepted);
  assert(runtime.complete_action("key", ActionState::succeeded, "message-id"));
  const auto duplicate = runtime.submit_action({"different-id", "key", now + 2h});
  assert(duplicate.result == ActionResult::duplicate);
  assert(duplicate.action->record.action_id == "action");
  assert(duplicate.action->state == ActionState::succeeded);
  assert(duplicate.action->result == "message-id");

  assert(runtime.submit_action({"cancel", "cancel-key", now + 1h}) == ActionResult::accepted);
  assert(runtime.cancel_action("cancel-key"));
  assert(!runtime.complete_action("cancel-key", true));
  assert(runtime.submit_action({"cancel-2", "cancel-key", now + 1h}).action->state ==
         ActionState::cancelled);

  assert(runtime.submit_action({"expire", "expire-key", now + 1s}) == ActionResult::accepted);
  now += 1s;
  assert(!runtime.complete_action("expire-key", true));
  assert(runtime.find_action("expire-key")->state == ActionState::expired);
  assert(runtime.submit_action({"expire-2", "expire-key", now + 1h}).action->state ==
         ActionState::expired);

  assert(runtime.submit_action({"failed", "failed-key", now + 1h}) == ActionResult::accepted);
  assert(runtime.complete_action("failed-key", ActionState::failed, "", "UPSTREAM_UNAVAILABLE"));
  const auto failed = runtime.submit_action({"failed-again", "failed-key", now + 1h});
  assert(failed.action->state == ActionState::failed);
  assert(failed.action->error_code == "UPSTREAM_UNAVAILABLE");
  kiarabot::core::detail::RuntimeEngine restarted(
      store, {}, [&] { return now; }, [] { return 0.5; });
  assert(restarted.submit_action(action).action->result == "message-id");
  now += 7 * 24h;
  const auto purged = restarted.purge_expired();
  assert(!purged.storage_error && purged.count == 4);
  assert(!restarted.find_action("key").has_value());
}

void plugins_and_transactions() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(
      store, {}, [&] { return now; }, [] { return 0.5; });
  assert(runtime.register_plugin("plugin", "v1", {"read", "send"}));
  assert(runtime.plugin_started("plugin"));
  assert(runtime.plugin_disconnected("plugin"));
  assert(store.find_plugin("plugin")->state == PluginState::unavailable);
  now += 1min;
  assert(runtime.plugin_disconnected("plugin"));
  assert(!runtime.plugin_reconnected("plugin", "v2", {"read", "send"}));
  assert(!runtime.plugin_reconnected("plugin", "v1", {"read"}));
  assert(runtime.plugin_reconnected("plugin", "v1", {"send", "read"}));
  assert(store.find_plugin("plugin")->state == PluginState::running);
  assert(runtime.plugin_disconnected("plugin"));
  now += 5min;
  assert(runtime.update_plugin_recovery("plugin"));
  assert(store.find_plugin("plugin")->state == PluginState::failed);
  kiarabot::core::detail::RuntimeEngine restarted(
      store, {}, [&] { return now; }, [] { return 0.5; });
  assert(!restarted.plugin_reconnected("plugin", "v1", {"read", "send"}));

  const std::size_t commits = store.commit_count();
  assert(runtime.ingest({"committed", "trace", "session", 1, true}) == EventResult::accepted);
  assert(store.commit_count() == commits + 1);
  store.set_fail_event_writes(true);
  const std::size_t rollbacks = store.rollback_count();
  assert(runtime.ingest({"failed-write", "trace", "session", 2, true}) ==
         EventResult::storage_error);
  assert(store.rollback_count() == rollbacks + 1);
  assert(runtime.session_status("session")->last_sequence == 1);
  store.set_fail_event_writes(false);
  store.set_fail_session_writes(true);
  assert(runtime.ingest({"failed-session", "trace", "session", 2, true}) ==
         EventResult::storage_error);
  assert(!store.find_event("failed-session").has_value());
  assert(runtime.session_status("session")->last_sequence == 1);
  store.set_fail_session_writes(false);
  store.set_fail_commit(true);
  assert(runtime.ingest({"failed-commit", "trace", "session", 2, true}) ==
         EventResult::storage_error);
  assert(!store.find_event("failed-commit").has_value());
  assert(runtime.session_status("session")->last_sequence == 1);
  store.set_fail_commit(false);
  assert(runtime.ingest({"after-rollback", "trace", "session", 2, true}) == EventResult::accepted);

  store.set_fail_action_writes(true);
  assert(runtime.submit_action({"failed", "failed-key", now + 1h}).result ==
         ActionResult::storage_error);
  store.set_fail_action_writes(false);
  assert(runtime.submit_action({"expires", "expires-key", now + 1s}).result ==
         ActionResult::accepted);
  now += 1s;
  store.set_fail_action_writes(true);
  assert(runtime.expire_actions().storage_error);
  assert(runtime.find_action("expires-key")->state == ActionState::accepted);
  store.set_fail_action_writes(false);
  assert(runtime.expire_actions().count == 1);
  store.set_fail_plugin_writes(true);
  assert(!runtime.register_plugin("write-failure"));
}

void restart_recovers_unacknowledged_dispatch() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine before(
      store, {}, [&] { return now; }, [] { return 0.5; });
  const std::string envelope("\x01\x00\x02", 3);
  assert(before.ingest({"inflight", "trace", "", 0, false, envelope}) == EventResult::accepted);
  assert(before.ingest({"waiting", "trace", "", 0, false}) == EventResult::accepted);
  assert(before.mark_dispatched("inflight"));

  kiarabot::core::detail::RuntimeEngine restarted(
      store, {}, [&] { return now; }, [] { return 0.5; });
  store.set_fail_event_writes(true);
  const auto failed_recovery = restarted.recover_inflight_events();
  assert(failed_recovery.storage_error && failed_recovery.count == 0);
  assert(store.find_event("inflight")->state == DeliveryState::dispatched);
  store.set_fail_event_writes(false);
  store.set_fail_commit(true);
  assert(restarted.recover_inflight_events().storage_error);
  assert(store.find_event("inflight")->state == DeliveryState::dispatched);
  store.set_fail_commit(false);
  const auto recovered = restarted.recover_inflight_events();
  assert(!recovered.storage_error && recovered.count == 1);
  assert(store.find_event("inflight")->state == DeliveryState::retrying);
  assert(store.find_event("inflight")->attempts == 1);
  assert(store.find_event("inflight")->record.serialized_envelope == envelope);
  assert(restarted.dispatchable_events() == std::vector<std::string>{"waiting"});
  assert(restarted.recover_inflight_events().count == 0);
  now += 1s;
  assert((restarted.dispatchable_events() == std::vector<std::string>{"inflight", "waiting"}));
  assert(restarted.mark_dispatched("inflight"));

  RuntimeOptions no_retries;
  no_retries.max_attempts = 0;
  kiarabot::core::detail::RuntimeEngine last_restart(
      store, no_retries, [&] { return now; }, [] { return 0.5; });
  assert(last_restart.recover_inflight_events().count == 1);
  assert(store.find_event("inflight")->state == DeliveryState::dead_letter);
  assert(last_restart.replay_dead_letter("inflight", "replay-trace"));
  assert(store.find_event("inflight")->record.serialized_envelope == envelope);
}

void expired_action_idempotency_and_transaction_isolation() {
  RuntimeTimePoint now{};
  kiarabot::core::detail::RuntimeState store;
  kiarabot::core::detail::RuntimeEngine runtime(
      store, {}, [&] { return now; }, [] { return 0.5; });
  const ActionRecord already_expired{"old-action", "same-key", now};
  store.set_fail_commit(true);
  assert(runtime.submit_action(already_expired).result == ActionResult::storage_error);
  assert(!runtime.find_action("same-key").has_value());
  store.set_fail_commit(false);
  const auto expired = runtime.submit_action(already_expired);
  assert(expired.result == ActionResult::expired);
  assert(expired.action->state == ActionState::expired);
  kiarabot::core::detail::RuntimeEngine restarted(
      store, {}, [&] { return now; }, [] { return 0.5; });
  const auto duplicate = restarted.submit_action({"new-action", "same-key", now + 1h});
  assert(duplicate.result == ActionResult::duplicate);
  assert(duplicate.action->record.action_id == "old-action");
  assert(duplicate.action->state == ActionState::expired);

  auto transaction = store.begin_transaction();
  assert(transaction);
  assert(!store.begin_transaction());
  assert(runtime.ingest({"nested", "trace", "", 0, false}) == EventResult::storage_error);
  transaction->rollback();
  assert(!store.find_event("nested").has_value());
  assert(runtime.ingest({"after", "trace", "", 0, false}) == EventResult::accepted);
  auto committed = store.begin_transaction();
  assert(store.insert_event(*store.find_event("after")) == StoreInsertResult::duplicate);
  assert(store.insert_action(*store.find_action_by_idempotency("same-key")) ==
         StoreInsertResult::duplicate);
  store.set_fail_action_writes(true);
  assert(store.insert_action(*store.find_action_by_idempotency("same-key")) ==
         StoreInsertResult::failed);
  store.set_fail_action_writes(false);
  assert(committed && committed->commit());
  committed->rollback();
  assert(store.find_event("after").has_value());
}

} // namespace

int main() {
  event_order_and_backpressure();
  retries_dead_letters_and_replay();
  jitter_bounds();
  actions_and_retention();
  plugins_and_transactions();
  restart_recovers_unacknowledged_dispatch();
  expired_action_idempotency_and_transaction_isolation();
  return 0;
}
