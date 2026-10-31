#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>

#include "kiarabot/core/application/action_router.hpp"
#include "kiarabot/core/application/runtime.h"
#include "kiarabot/core/runtime/in_memory_runtime_store.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
namespace wire = kiarabot::v1;
using namespace std::chrono_literals;
using kiarabot::core::ActionRouter;
using kiarabot::core::AdapterSession;
using kiarabot::core::PluginSession;
using kiarabot::core::RuntimeTimePoint;
void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("Action assertion " + std::to_string(location.line()));
  }
}
template <typename T, typename E> T value(std::variant<T, E> result) {
  require(std::holds_alternative<T>(result));
  return std::get<T>(std::move(result));
}
wire::PluginActionRequest action(std::string id, std::string key, int deadline = 2000) {
  wire::PluginActionRequest request;
  request.set_request_id("request-" + id);
  auto* result = request.mutable_action();
  result->set_action_id(id);
  result->set_idempotency_key(key);
  result->set_trace_id("trace");
  result->mutable_deadline()->set_seconds(deadline);
  result->mutable_target_source()->set_adapter_id("adapter");
  result->mutable_target_source()->set_account_id("account");
  result->mutable_target()->set_conversation_id("123");
  auto* send = result->mutable_send_message();
  send->mutable_conversation()->set_id("123");
  send->mutable_conversation()->set_type(wire::ConversationRef::PRIVATE);
  send->mutable_conversation()->set_account_id("account");
  send->mutable_message()->set_plain_text("hello");
  return request;
}
wire::RegisterAdapterRequest registration() {
  wire::RegisterAdapterRequest result;
  auto* capability = result.add_capabilities();
  capability->set_name("action.send_message.text.v1");
  capability->set_version("1");
  capability->set_supported(true);
  return result;
}
wire::PluginActionQuery query(std::string id) {
  wire::PluginActionQuery result;
  result.set_request_id("query");
  result.set_trace_id("trace");
  result.mutable_deadline()->set_seconds(10000);
  result.set_action_id(id);
  return result;
}
wire::PluginActionCancel cancel(std::string id) {
  wire::PluginActionCancel result;
  result.set_request_id("cancel-" + id);
  result.set_trace_id("trace");
  result.mutable_deadline()->set_seconds(10000);
  result.set_action_id(id);
  return result;
}
void scenarios(kiarabot::core::RuntimeStore& store,
               kiarabot::core::InMemoryRuntimeStore* faults = nullptr) {
  auto now = RuntimeTimePoint(1000s);
  ActionRouter router(store, [&] { return now; });
  PluginSession plugin{"one", "instance", "fence", {}, {"message.send"}};
  PluginSession other{"two", "instance", "fence", {}, {"message.send"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  auto denied = plugin;
  denied.permissions.clear();
  require(router.submit(denied, action("denied", "denied")).error().code() ==
          wire::PERMISSION_DENIED);
  require(!value(router.next(adapter)));
  auto invalid = action("invalid", "invalid");
  invalid.mutable_action()->set_requested_by("spoofed");
  require(router.submit(plugin, invalid).error().code() == wire::PERMISSION_DENIED);
  invalid.mutable_action()->clear_requested_by();
  invalid.mutable_action()->mutable_delete_message()->set_message_id("message");
  require(router.submit(plugin, invalid).error().code() == wire::CAPABILITY_NOT_SUPPORTED);
  if (faults) {
    faults->set_fail_commits(true);
    require(router.submit(plugin, action("write-fail", "write-fail")).error().code() ==
            wire::UPSTREAM_UNAVAILABLE);
    faults->set_fail_commits(false);
    require(!value(router.next(adapter)));
    faults->set_fail_reads(true);
    require(router.query(plugin, query("missing")).error().code() == wire::UPSTREAM_UNAVAILABLE);
    faults->set_fail_reads(false);
  }
  require(router.submit(plugin, action("first", "shared")).result().status() == wire::ACCEPTED);
  require(router.submit(plugin, action("new-id", "shared")).result().action_id() == "first");
  auto conflict = action("new-id", "shared");
  conflict.mutable_action()->mutable_send_message()->mutable_message()->set_plain_text("changed");
  require(router.submit(plugin, conflict).error().code() == wire::CONFLICT);
  require(router.query(other, query("first")).error().code() == wire::NOT_FOUND);
  require(router.submit(other, action("other", "shared")).result().status() == wire::ACCEPTED);
  require(router.cancel(other, cancel("other")).result().status() == wire::CANCELLED);
  const auto sent = value(router.next(adapter));
  require(sent && sent->action_id() == "first" &&
          sent->target_source().adapter_instance_id() == adapter.instance);
  require(!value(router.next(adapter)));
  require(router.cancel(plugin, cancel("first")).error().code() == wire::CONFLICT);
  router.detach(adapter);
  adapter.instance = "replacement";
  router.attach(adapter, registration());
  require(value(router.next(adapter))->action_id() == "first");
  wire::ActionResult completed;
  completed.set_action_id("first");
  completed.set_status(wire::SUCCEEDED);
  completed.set_platform_message_id("platform-1");
  completed.mutable_completed_at()->set_seconds(1001);
  auto stale = adapter;
  stale.instance = "adapter-instance";
  require(std::get<wire::Error>(router.complete(stale, completed)).code() ==
          wire::PERMISSION_DENIED);
  value(router.complete(adapter, completed));
  value(router.complete(adapter, completed));
  auto found = router.query(plugin, query("first"));
  require(found.result().status() == wire::SUCCEEDED &&
          found.result().platform_message_id() == "platform-1");
  auto lookup = query("unused");
  lookup.set_idempotency_key("shared");
  require(router.query(plugin, lookup).result().action_id() == "first");
  require(router.submit(plugin, action("expired", "expired", 999)).result().status() ==
          wire::EXPIRED);
  require(router.cancel(plugin, cancel("expired")).result().status() == wire::EXPIRED);
  require(router.submit(plugin, action("late", "late", 1001)).result().status() == wire::ACCEPTED);
  require(value(router.next(adapter))->action_id() == "late");
  now += 2s;
  completed.set_action_id("late");
  value(router.complete(adapter, completed));
  require(router.query(plugin, query("late")).result().status() == wire::EXPIRED);
  require(router.cancel(plugin, cancel("late")).result().status() == wire::EXPIRED);
  auto overdue = query("first");
  overdue.mutable_deadline()->set_seconds(1);
  require(router.query(plugin, overdue).error().code() == wire::TIMEOUT);
  // Runtime operations preserve the extended routing metadata.
  kiarabot::core::Runtime runtime(store);
  value(runtime.ingest({"event", "trace", "", 0, false}));
  require(router.query(plugin, query("first")).result().platform_message_id() == "platform-1");
}

void capacity() {
  // Injected limits come from the composition root; the router enforces them
  // instead of a hard-coded constant.
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  kiarabot::core::RuntimeLimits limits;
  limits.action_limit = 2;
  limits.audit_limit = 1000;
  ActionRouter router(store, [&] { return now; }, limits);
  PluginSession plugin{"one", "instance", "fence", {}, {"message.send"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  require(router.submit(plugin, action("a1", "k1")).result().status() == wire::ACCEPTED);
  require(router.submit(plugin, action("a2", "k2")).result().status() == wire::ACCEPTED);
  require(router.submit(plugin, action("a3", "k3")).error().code() == wire::RATE_LIMITED);
  // A replayed idempotent submit is still answered while the capacity is full.
  require(router.submit(plugin, action("a1", "k1")).result().action_id() == "a1");
}

} // namespace
int main() try {
  kiarabot::core::InMemoryRuntimeStore store;
  scenarios(store, &store);
  capacity();
  // Actual SQLite persistence including reopening schema v3.
  const auto path = std::filesystem::temp_directory_path() /
                    ("kiarabot-action-" + kiarabot::core::random_instance_id() + ".db");
  {
    auto database = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    scenarios(*database);
  }
  {
    auto database = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    ActionRouter router(*database, [] { return RuntimeTimePoint(1100s); });
    PluginSession plugin{"one", "instance", "fence", {}, {"message.send"}};
    require(router.query(plugin, query("first")).result().platform_message_id() == "platform-1");
    const auto snapshot = value(database->read());
    require(std::any_of(snapshot.actions.begin(), snapshot.actions.end(), [](const auto& item) {
      return item.second.dispatched && !item.second.record.serialized_envelope.empty() &&
             !item.second.record.owner.empty();
    }));
  }
  std::filesystem::remove(path);
  std::cout << "Action authorization, scoped idempotency, dispatch, deadline, cancel and "
               "persistence passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
