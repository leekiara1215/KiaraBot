#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "kiarabot/core/request_digest.hpp"
#include "kiarabot/core/task_scheduler.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"
#include "kiarabot/infrastructure/time_zone_resolver.hpp"

namespace {
namespace wire = kiarabot::v1;
using namespace std::chrono_literals;
using kiarabot::core::InMemoryRuntimeStore;
using kiarabot::core::PluginSession;
using kiarabot::core::random_instance_id;
using kiarabot::core::Runtime;
using kiarabot::core::RuntimeStore;
using kiarabot::core::TaskScheduler;
using kiarabot::core::WallTime;
void check(bool ok, const std::source_location& at = std::source_location::current()) {
  if (!ok) {
    throw std::runtime_error("task assertion " + std::to_string(at.line()));
  }
}
template <typename T, typename E>
T value(std::variant<T, E> result,
        const std::source_location& at = std::source_location::current()) {
  check(std::holds_alternative<T>(result), at);
  return std::get<T>(std::move(result));
}
wire::TaskCreateRequest request(std::string id, std::int64_t seconds = 1001) {
  wire::TaskCreateRequest result;
  result.set_task_id(id);
  result.set_target_conversation("conversation");
  result.set_payload(std::string("x\0y", 3));
  result.mutable_run_at()->set_seconds(seconds);
  return result;
}
void zones() {
  kiarabot::infrastructure::TimeZoneResolver resolver;
  wire::LocalTaskTime local;
  local.set_year(2026);
  local.set_month(3);
  local.set_day(8);
  local.set_hour(2);
  local.set_minute(30);
  local.set_time_zone("America/New_York");
  check(std::get<wire::Error>(resolver.resolve(local)).code() ==
        wire::INVALID_ARGUMENT); // DST gap.
  local.set_month(11);
  local.set_day(1);
  local.set_hour(1);
  check(std::get<wire::Error>(resolver.resolve(local)).code() ==
        wire::INVALID_ARGUMENT); // DST fold.
  local.set_time_zone("Asia/Shanghai");
  local.set_month(1);
  local.set_day(1);
  local.set_hour(8);
  local.set_minute(0);
  check(value(resolver.resolve(local)) == WallTime(1767225600s));
  local.set_time_zone("UTC");
  local.set_hour(0);
  check(value(resolver.resolve(local)) == WallTime(1767225600s));
  local.set_month(2);
  local.set_day(30);
  check(std::holds_alternative<wire::Error>(resolver.resolve(local)));
  local.set_day(1);
  local.set_time_zone("../etc/passwd");
  check(std::holds_alternative<wire::Error>(resolver.resolve(local)));
  local.set_time_zone("Unknown/Zone");
  check(std::holds_alternative<wire::Error>(resolver.resolve(local)));
}
void scenarios(RuntimeStore& store, InMemoryRuntimeStore* faults = nullptr) {
  auto current = WallTime(1000s);
  auto timezone = std::make_shared<kiarabot::infrastructure::TimeZoneResolver>();
  TaskScheduler scheduler(store, timezone, [&] { return current; });
  Runtime runtime(store, {}, [&] { return current; });
  check(value(runtime.register_plugin("one", "v1", {"task.schedule"}, true)));
  check(value(runtime.plugin_started("one")));
  check(value(scheduler.create("one", request("done"))).state() == wire::SCHEDULED_TASK_PENDING);
  check(std::get<wire::Error>(scheduler.create("one", request("past", 999))).code() ==
        wire::INVALID_ARGUMENT);
  check(std::get<wire::Error>(scheduler.create("one", request("far", 9999999999LL))).code() ==
        wire::INVALID_ARGUMENT);
  check(std::get<wire::Error>(scheduler.create("one", request("done", 1002))).code() ==
        wire::CONFLICT);
  check(std::get<wire::Error>(scheduler.get("two", "done")).code() == wire::NOT_FOUND);
  check(std::get<wire::Error>(scheduler.cancel("two", "done")).code() == wire::NOT_FOUND);
  check(value(scheduler.claim_due(current, 32)).empty());
  current += 1s;
  const auto first = value(scheduler.claim_due(current, 32));
  check(first.size() == 1);
  check(value(scheduler.claim_due(current, 32)).empty());
  value(scheduler.recover());
  check(value(scheduler.get("one", "done")).state() == wire::SCHEDULED_TASK_RETRYING);
  const auto second = value(scheduler.claim_due(current, 32));
  check(second.size() == 1);
  check(first[0].claim_token != second[0].claim_token);
  check(std::get<wire::Error>(scheduler.finish("one", "done", first[0].claim_token, true, {}))
            .code() == wire::CONFLICT);
  if (faults) {
    faults->set_fail_commits(true);
    check(std::get<wire::Error>(scheduler.finish("one", "done", second[0].claim_token, true, {}))
              .code() == wire::UPSTREAM_UNAVAILABLE);
    faults->set_fail_commits(false);
    check(value(store.read()).events.empty());
    check(value(scheduler.get("one", "done")).state() == wire::SCHEDULED_TASK_RUNNING);
  }
  value(scheduler.finish("one", "done", second[0].claim_token, true, {}));
  check(value(scheduler.get("one", "done")).state() == wire::SCHEDULED_TASK_COMPLETED);
  check(std::get<wire::Error>(scheduler.cancel("one", "done")).code() == wire::CONFLICT);
  check(std::get<wire::Error>(scheduler.finish("one", "done", second[0].claim_token, true, {}))
            .code() == wire::CONFLICT);
  auto snapshot = value(store.read());
  check(snapshot.events.size() == 1);
  const auto& entry = snapshot.events.begin()->second;
  wire::EventEnvelope timer;
  check(timer.ParseFromString(entry.record.serialized_envelope));
  check(timer.timer_triggered().task_id() == "done" &&
        timer.timer_triggered().owner_plugin_id() == "one");
  check(timer.timer_triggered().payload() == std::string("x\0y", 3));
  check(timer.timer_triggered().scheduled_at().seconds() == 1001 &&
        timer.subject().conversation_id() == "conversation");
  check(snapshot.deliveries.at(timer.event_id()).size() == 1 &&
        snapshot.deliveries.at(timer.event_id()).contains("one"));
  value(scheduler.create("one", request("pending_cancel", 1002)));
  check(value(scheduler.cancel("one", "pending_cancel")).state() == wire::SCHEDULED_TASK_CANCELLED);
  check(value(scheduler.cancel("one", "pending_cancel")).state() == wire::SCHEDULED_TASK_CANCELLED);
  value(scheduler.create("one", request("running_cancel", 1002)));
  current += 1s;
  auto cancelling = value(scheduler.claim_due(current, 32));
  check(cancelling.size() == 1);
  check(value(scheduler.cancel("one", "running_cancel")).state() == wire::SCHEDULED_TASK_RUNNING);
  value(scheduler.finish("one", "running_cancel", cancelling[0].claim_token, true, {}));
  check(value(scheduler.get("one", "running_cancel")).state() == wire::SCHEDULED_TASK_CANCELLED);
  check(value(store.read()).events.size() == 1);
  value(scheduler.create("one", request("retry", 1003)));
  current += 1s;
  const auto retry = value(scheduler.claim_due(current, 32));
  check(retry.size() == 1);
  value(scheduler.finish("one", "retry", retry[0].claim_token, false, current + 1s));
  check(value(scheduler.cancel("one", "retry")).state() == wire::SCHEDULED_TASK_CANCELLED);
  current += 1s;
  value(scheduler.tick());
  check(value(store.read()).events.size() == 1);
  // Unrelated runtime commits preserve every task and fencing token.
  value(runtime.ingest({"ordinary", "trace", "", 0, false}));
  check(value(store.read()).tasks.at("one").size() == 4);
  // Bounded dispatch failure retries; missing owner cannot leak a timer to another plugin.
  value(scheduler.create("missing", request("fail", 1005)));
  for (int attempt = 0; attempt < 5; ++attempt) {
    current += 1s;
    value(scheduler.tick());
  }
  check(value(scheduler.get("missing", "fail")).state() == wire::SCHEDULED_TASK_FAILED);
  check(value(store.read()).events.size() == 2);
}
void host() {
  InMemoryRuntimeStore store;
  auto current = WallTime(1000s);
  TaskScheduler scheduler(store, std::make_shared<kiarabot::infrastructure::TimeZoneResolver>(),
                          [&] { return current; });
  PluginSession plugin{"one", "instance", "fence", {"event.timer.triggered"}, {"task.schedule"}};
  wire::HostRequest operation;
  operation.set_request_id("request");
  operation.set_trace_id("trace");
  operation.set_idempotency_key("1234567890123456");
  operation.mutable_deadline()->set_seconds(1100);
  *operation.mutable_task_create() = request("task");
  auto denied = plugin;
  denied.permissions.clear();
  check(scheduler.dispatch(denied, operation).error().code() == wire::PERMISSION_DENIED);
  check(value(store.read()).tasks.empty());
  auto no_subscription = plugin;
  no_subscription.subscriptions.clear();
  check(scheduler.dispatch(no_subscription, operation).error().code() ==
        wire::CAPABILITY_NOT_SUPPORTED);
  store.set_fail_commits(true);
  check(scheduler.dispatch(plugin, operation).error().code() == wire::UPSTREAM_UNAVAILABLE);
  store.set_fail_commits(false);
  check(value(store.read()).tasks.empty());
  check(scheduler.dispatch(plugin, operation).task_create().state() ==
        wire::SCHEDULED_TASK_PENDING);
  value(scheduler.cancel("one", "task"));
  operation.set_request_id("replay");
  check(scheduler.dispatch(plugin, operation).task_create().state() ==
        wire::SCHEDULED_TASK_PENDING); // Original receipt.
  operation.mutable_task_create()->set_payload("changed");
  check(scheduler.dispatch(plugin, operation).error().code() == wire::CONFLICT);
  operation.mutable_task_get()->set_task_id("task");
  check(scheduler.dispatch(plugin, operation).task_get().state() == wire::SCHEDULED_TASK_CANCELLED);
  current += 101s;
  check(scheduler.dispatch(plugin, operation).error().code() == wire::TIMEOUT);
}
} // namespace
int main() try {
  zones();
  host();
  InMemoryRuntimeStore memory;
  scenarios(memory, &memory);
  const auto path =
      (std::filesystem::temp_directory_path() / ("kiarabot-tasks-" + random_instance_id() + ".db"))
          .string();
  {
    auto store = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    scenarios(*store);
    TaskScheduler scheduler(*store, {}, [] { return WallTime(2000s); });
    value(scheduler.create("one", request("restart", 2001)));
    check(value(scheduler.claim_due(WallTime(2001s), 1)).size() == 1);
  }
  {
    auto store = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    TaskScheduler scheduler(*store, {}, [] { return WallTime(2002s); });
    check(value(scheduler.get("one", "restart")).state() == wire::SCHEDULED_TASK_RUNNING);
    value(scheduler.recover());
    check(value(scheduler.get("one", "restart")).state() == wire::SCHEDULED_TASK_RETRYING);
    value(scheduler.tick());
    check(value(scheduler.get("one", "restart")).state() == wire::SCHEDULED_TASK_COMPLETED);
    check(value(store->read()).events.size() == 3);
  }
  std::filesystem::remove(path);
  std::cout
      << "task UTC/IANA/DST, fencing, cancellation, retries, audit rollback and restart passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
