#include <algorithm>
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
wire::PluginActionRequest group_action(std::string id, std::string key,
                                       const std::string& conversation, int deadline = 2000) {
  auto request = action(std::move(id), std::move(key), deadline);
  auto* send = request.mutable_action()->mutable_send_message();
  send->mutable_conversation()->set_id(conversation);
  send->mutable_conversation()->set_type(wire::ConversationRef::GROUP);
  request.mutable_action()->mutable_target()->set_conversation_id(conversation);
  return request;
}
wire::PluginActionRequest react_action(std::string id, std::string key,
                                       std::string message_id = "9001",
                                       std::string reaction = "like", int deadline = 2000) {
  auto request = action(std::move(id), std::move(key), deadline);
  auto* envelope = request.mutable_action();
  envelope->clear_send_message();
  auto* react = envelope->mutable_react_message();
  react->set_message_id(std::move(message_id));
  react->set_reaction(std::move(reaction));
  react->mutable_conversation()->set_id("123");
  react->mutable_conversation()->set_type(wire::ConversationRef::PRIVATE);
  react->mutable_conversation()->set_account_id("account");
  return request;
}
wire::PluginActionRequest delete_action(std::string id, std::string key,
                                        std::string message_id = "9001",
                                        std::string conversation = "123", int deadline = 2000) {
  auto request = action(std::move(id), std::move(key), deadline);
  auto* envelope = request.mutable_action();
  envelope->clear_send_message();
  auto* deletion = envelope->mutable_delete_message();
  deletion->set_message_id(std::move(message_id));
  deletion->mutable_conversation()->set_id(std::move(conversation));
  deletion->mutable_conversation()->set_type(wire::ConversationRef::PRIVATE);
  deletion->mutable_conversation()->set_account_id("account");
  return request;
}
wire::PluginActionRequest mute_action(std::string id, std::string key, std::string group_id = "123",
                                      std::string user_id = "456", std::uint32_t duration = 600) {
  auto request = action(std::move(id), std::move(key), 1000000);
  auto* envelope = request.mutable_action();
  envelope->clear_send_message();
  auto* mute = envelope->mutable_mute_member();
  mute->set_group_id(group_id);
  mute->set_user_id(std::move(user_id));
  mute->set_duration_seconds(duration);
  envelope->mutable_target()->set_conversation_id(std::move(group_id));
  return request;
}
wire::PluginActionRequest kick_action(std::string id, std::string key, std::string group_id = "123",
                                      std::string user_id = "456", std::string reason = "spam") {
  auto request = action(std::move(id), std::move(key), 1000000);
  auto* envelope = request.mutable_action();
  envelope->clear_send_message();
  auto* kick = envelope->mutable_kick_member();
  kick->set_group_id(group_id);
  kick->set_user_id(std::move(user_id));
  kick->set_reason(std::move(reason));
  envelope->mutable_target()->set_conversation_id(std::move(group_id));
  return request;
}
// Unit tests exercise routing and persistence, not deployment quota; quota has
// its own scenario with the production defaults.
kiarabot::core::ActionQuotaPolicy permissive_quota() {
  kiarabot::core::ActionQuotaPolicy policy;
  policy.account_limit = 1000;
  policy.group_limit = 1000;
  policy.private_limit = 1000;
  return policy;
}
wire::RegisterAdapterRequest registration() {
  wire::RegisterAdapterRequest result;
  auto* capability = result.add_capabilities();
  capability->set_name("action.send_message.text.v1");
  capability->set_version("1");
  capability->set_supported(true);
  return result;
}
wire::RegisterAdapterRequest react_registration() {
  auto result = registration();
  auto* capability = result.add_capabilities();
  capability->set_name("action.react_message.v1");
  capability->set_version("1");
  capability->set_supported(true);
  return result;
}
wire::RegisterAdapterRequest delete_registration() {
  auto result = registration();
  auto* capability = result.add_capabilities();
  capability->set_name("action.delete_message.v1");
  capability->set_version("1");
  capability->set_supported(true);
  return result;
}
wire::RegisterAdapterRequest moderation_registration() {
  auto result = registration();
  for (const auto* name : {"action.mute_member.v1", "action.kick_member.v1"}) {
    auto* capability = result.add_capabilities();
    capability->set_name(name);
    capability->set_version("1");
    capability->set_supported(true);
  }
  return result;
}
wire::RegisterAdapterRequest approve_registration() {
  auto result = registration();
  auto* capability = result.add_capabilities();
  capability->set_name("action.approve_request.v1");
  capability->set_version("1");
  capability->set_supported(true);
  return result;
}
wire::PluginActionRequest approve_action(std::string id, std::string key,
                                         std::string request_id = "request-1",
                                         bool approve_value = true, std::string reason = "ok") {
  auto request = action(std::move(id), std::move(key), 1000000);
  auto* envelope = request.mutable_action();
  envelope->clear_send_message();
  auto* approve = envelope->mutable_approve_request();
  approve->set_request_id(std::move(request_id));
  approve->set_approve(approve_value);
  approve->set_reason(std::move(reason));
  return request;
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
  ActionRouter router(
      store, [&] { return now; }, {}, permissive_quota());
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
  // ScheduleTask is deprecated and must stay rejected.
  invalid.mutable_action()->mutable_schedule_task()->set_task_id("task");
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
  ActionRouter router(
      store, [&] { return now; }, limits, permissive_quota());
  PluginSession plugin{"one", "instance", "fence", {}, {"message.send"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  require(router.submit(plugin, action("a1", "k1")).result().status() == wire::ACCEPTED);
  require(router.submit(plugin, action("a2", "k2")).result().status() == wire::ACCEPTED);
  require(router.submit(plugin, action("a3", "k3")).error().code() == wire::RATE_LIMITED);
  // A replayed idempotent submit is still answered while the capacity is full.
  require(router.submit(plugin, action("a1", "k1")).result().action_id() == "a1");
}

void quota() {
  // Production defaults from doc/17: 10 per account per minute, 3 per group per
  // minute, 1 per private conversation per 10 seconds.
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  ActionRouter router(store, [&] { return now; });
  PluginSession plugin{"one", "instance", "fence", {}, {"message.send"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  // An already expired action is stored as EXPIRED and never counts as a
  // platform-bound attempt.
  require(router.submit(plugin, action("quota-expired", "quota-expired", 999)).result().status() ==
          wire::EXPIRED);
  for (int index = 0; index < 3; ++index) {
    const auto id = "group-" + std::to_string(index);
    require(router.submit(plugin, group_action(id, id, "room-1")).result().status() ==
            wire::ACCEPTED);
  }
  const auto limited = router.submit(plugin, group_action("group-4", "group-4", "room-1"));
  require(limited.error().code() == wire::RATE_LIMITED && limited.error().retryable());
  // Another conversation has its own window.
  require(router.submit(plugin, group_action("room-2", "room-2", "room-2")).result().status() ==
          wire::ACCEPTED);
  // Idempotent replay returns the original result without consuming quota.
  require(
      router.submit(plugin, group_action("group-0", "group-0", "room-1")).result().action_id() ==
      "group-0");
  // Private conversations are limited to one attempt per ten seconds.
  require(router.submit(plugin, action("private-1", "private-1")).result().status() ==
          wire::ACCEPTED);
  require(router.submit(plugin, action("private-2", "private-2")).error().code() ==
          wire::RATE_LIMITED);
  now += 11s;
  require(router.submit(plugin, action("private-3", "private-3")).result().status() ==
          wire::ACCEPTED);
  // Fill the account window (ten accepted attempts since t=1000): three group
  // sends, one other-group send, two private sends, four more one-off groups.
  for (int index = 3; index <= 6; ++index) {
    const auto id = "room-" + std::to_string(index);
    require(router.submit(plugin, group_action(id, id, id)).result().status() == wire::ACCEPTED);
  }
  require(router.submit(plugin, group_action("room-7", "room-7", "room-7")).error().code() ==
          wire::RATE_LIMITED);
  now += 61s;
  require(router.submit(plugin, group_action("room-8", "room-8", "room-8")).result().status() ==
          wire::ACCEPTED);
}

void budgets() {
  // Daily budgets are persisted in the Action snapshot transaction and reset on
  // the next UTC day; they are independent of the process-local rate windows.
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  auto policy = permissive_quota();
  policy.daily_limits["action.send_message.text.v1"] = 2;
  ActionRouter router(
      store, [&] { return now; }, {}, policy);
  PluginSession plugin{"one", "instance", "fence", {}, {"message.send"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  require(router.submit(plugin, action("budget-1", "budget-1", 1000000)).result().status() ==
          wire::ACCEPTED);
  require(router.submit(plugin, action("budget-2", "budget-2", 1000000)).result().status() ==
          wire::ACCEPTED);
  const auto limited = router.submit(plugin, action("budget-3", "budget-3", 1000000));
  require(limited.error().code() == wire::RATE_LIMITED && limited.error().retryable());
  {
    const auto snapshot = value(store.read());
    require(snapshot.daily_budgets.at("action.send_message.text.v1|account").used == 2);
    // Action audits carry the platform target for account/conversation scoping.
    const auto found =
        std::find_if(snapshot.audit.rbegin(), snapshot.audit.rend(), [](const auto& entry) {
          return entry.permission == "message.send" && entry.outcome == "OK";
        });
    require(found != snapshot.audit.rend() && found->target_account_id == "account" &&
            found->target_conversation_id == "123");
  }
  now += std::chrono::hours(24);
  require(router.submit(plugin, action("budget-4", "budget-4", 1000000)).result().status() ==
          wire::ACCEPTED);
}

void reactions() {
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  ActionRouter router(
      store, [&] { return now; }, {}, permissive_quota());
  PluginSession sender{"one", "instance", "fence", {}, {"message.send"}};
  PluginSession reactor{"reactor", "instance", "fence", {}, {"message.send", "message.react"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  // message.send does not imply message.react.
  require(router.submit(sender, react_action("denied", "denied")).error().code() ==
          wire::PERMISSION_DENIED);
  // An adapter without the react capability cannot accept the payload.
  require(router.submit(reactor, react_action("no-capability", "no-capability")).error().code() ==
          wire::CAPABILITY_NOT_SUPPORTED);
  // Unknown reaction token is rejected before reaching the platform.
  require(router.submit(reactor, react_action("bad-token", "bad-token", "9001", "party"))
              .error()
              .code() == wire::CAPABILITY_NOT_SUPPORTED);
  require(router.submit(reactor, react_action("empty-token", "empty-token", "9001", ""))
              .error()
              .code() == wire::INVALID_ARGUMENT);
  require(
      router.submit(reactor, react_action("empty-message", "empty-message", "")).error().code() ==
      wire::INVALID_ARGUMENT);
  auto channel = react_action("channel", "channel");
  channel.mutable_action()->mutable_react_message()->mutable_conversation()->set_type(
      wire::ConversationRef::CHANNEL);
  require(router.submit(reactor, channel).error().code() == wire::CAPABILITY_NOT_SUPPORTED);
  router.attach(adapter, react_registration());
  require(router.submit(reactor, react_action("react-1", "react-1")).result().status() ==
          wire::ACCEPTED);
  const auto sent = value(router.next(adapter));
  require(sent && sent->action_id() == "react-1" && sent->has_react_message() &&
          sent->react_message().reaction() == "like");
  wire::ActionResult completed;
  completed.set_action_id("react-1");
  completed.set_status(wire::SUCCEEDED);
  completed.mutable_completed_at()->set_seconds(1001);
  value(router.complete(adapter, completed));
  require(router.query(reactor, query("react-1")).result().status() == wire::SUCCEEDED &&
          router.query(reactor, query("react-1")).result().platform_message_id().empty());
  // Idempotent replay still returns the stored result instead of re-dispatching.
  require(router.submit(reactor, react_action("react-1", "react-1")).result().action_id() ==
          "react-1");
  require(!value(router.next(adapter)));
}

void deletions() {
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  ActionRouter router(
      store, [&] { return now; }, {}, permissive_quota());
  PluginSession sender{"one", "instance", "fence", {}, {"message.send"}};
  PluginSession deleter{"deleter", "instance", "fence", {}, {"message.send", "message.delete"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  // message.send does not imply message.delete.
  require(router.submit(sender, delete_action("denied", "denied")).error().code() ==
          wire::PERMISSION_DENIED);
  // An adapter without the delete capability cannot accept the payload.
  require(router.submit(deleter, delete_action("no-capability", "no-capability")).error().code() ==
          wire::CAPABILITY_NOT_SUPPORTED);
  router.attach(adapter, delete_registration());
  // Conversation type, ID and target consistency are validated before dispatch.
  auto channel = delete_action("channel", "channel");
  channel.mutable_action()->mutable_delete_message()->mutable_conversation()->set_type(
      wire::ConversationRef::CHANNEL);
  require(router.submit(deleter, channel).error().code() == wire::CAPABILITY_NOT_SUPPORTED);
  require(
      router.submit(deleter, delete_action("empty-message", "empty-message", "")).error().code() ==
      wire::INVALID_ARGUMENT);
  auto mismatched = delete_action("mismatched", "mismatched");
  mismatched.mutable_action()->mutable_target()->set_conversation_id("999");
  require(router.submit(deleter, mismatched).error().code() == wire::INVALID_ARGUMENT);
  require(router.submit(deleter, delete_action("delete-1", "delete-1")).result().status() ==
          wire::ACCEPTED);
  const auto sent = value(router.next(adapter));
  require(sent && sent->action_id() == "delete-1" && sent->has_delete_message() &&
          sent->delete_message().message_id() == "9001");
  wire::ActionResult completed;
  completed.set_action_id("delete-1");
  completed.set_status(wire::SUCCEEDED);
  completed.mutable_completed_at()->set_seconds(1001);
  value(router.complete(adapter, completed));
  require(router.query(deleter, query("delete-1")).result().status() == wire::SUCCEEDED &&
          router.query(deleter, query("delete-1")).result().platform_message_id().empty());
  require(router.submit(deleter, delete_action("delete-1", "delete-1")).result().action_id() ==
          "delete-1");
  require(!value(router.next(adapter)));
}

void moderation() {
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  auto policy = permissive_quota();
  policy.allowed_conversations["action.mute_member.v1"] = {"123"};
  policy.allowed_conversations["action.kick_member.v1"] = {"123"};
  policy.daily_limits["action.kick_member.v1"] = 1;
  ActionRouter router(
      store, [&] { return now; }, {}, policy);
  PluginSession sender{"one", "instance", "fence", {}, {"message.send"}};
  PluginSession moderator{"mod", "instance", "fence", {}, {"member.mute", "member.kick"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  // message.send does not imply member.mute / member.kick.
  require(router.submit(sender, mute_action("denied", "denied")).error().code() ==
          wire::PERMISSION_DENIED);
  // The adapter must negotiate the management capabilities.
  require(router.submit(moderator, mute_action("no-capability", "no-capability")).error().code() ==
          wire::CAPABILITY_NOT_SUPPORTED);
  router.attach(adapter, moderation_registration());
  // Invalid payloads are rejected before any platform side effect.
  require(router.submit(moderator, mute_action("zero", "zero", "123", "456", 0)).error().code() ==
          wire::INVALID_ARGUMENT);
  require(router.submit(moderator, mute_action("too-long", "too-long", "123", "456", 2592001))
              .error()
              .code() == wire::INVALID_ARGUMENT);
  auto mismatched = mute_action("mismatched", "mismatched");
  mismatched.mutable_action()->mutable_target()->set_conversation_id("999");
  require(router.submit(moderator, mismatched).error().code() == wire::INVALID_ARGUMENT);
  require(router
              .submit(moderator, kick_action("long-reason", "long-reason", "123", "456",
                                             std::string(257, 'x')))
              .error()
              .code() == wire::INVALID_ARGUMENT);
  // Dangerous kinds honour the conversation allowlist (fail closed).
  require(
      router.submit(moderator, mute_action("other-group", "other-group", "999")).error().code() ==
      wire::PERMISSION_DENIED);
  require(router.submit(moderator, mute_action("mute-1", "mute-1")).result().status() ==
          wire::ACCEPTED);
  const auto muted = value(router.next(adapter));
  require(muted && muted->action_id() == "mute-1" && muted->has_mute_member() &&
          muted->mute_member().duration_seconds() == 600);
  wire::ActionResult completed;
  completed.set_action_id("mute-1");
  completed.set_status(wire::SUCCEEDED);
  completed.mutable_completed_at()->set_seconds(1001);
  value(router.complete(adapter, completed));
  require(router.query(moderator, query("mute-1")).result().status() == wire::SUCCEEDED &&
          router.query(moderator, query("mute-1")).result().platform_message_id().empty());
  // The kick daily budget allows one attempt per UTC day and denies the next.
  require(router.submit(moderator, kick_action("kick-1", "kick-1")).result().status() ==
          wire::ACCEPTED);
  require(router.submit(moderator, kick_action("kick-2", "kick-2")).error().code() ==
          wire::RATE_LIMITED);
  {
    const auto snapshot = value(store.read());
    require(snapshot.daily_budgets.at("action.kick_member.v1|account").used == 1);
    // Management audits carry the target conversation for investigation.
    const auto found =
        std::find_if(snapshot.audit.rbegin(), snapshot.audit.rend(), [](const auto& entry) {
          return entry.permission == "member.mute" && entry.outcome == "OK";
        });
    require(found != snapshot.audit.rend() && found->target_account_id == "account" &&
            found->target_conversation_id == "123");
  }
}

void approvals() {
  kiarabot::core::InMemoryRuntimeStore store;
  auto now = RuntimeTimePoint(1000s);
  ActionRouter router(
      store, [&] { return now; }, {}, permissive_quota());
  PluginSession sender{"one", "instance", "fence", {}, {"message.send"}};
  PluginSession approver{"approver", "instance", "fence", {}, {"request.approve"}};
  AdapterSession adapter{"adapter", "adapter-instance", {"account"}};
  router.attach(adapter, registration());
  // message.send does not imply request.approve.
  require(router.submit(sender, approve_action("denied", "denied")).error().code() ==
          wire::PERMISSION_DENIED);
  // The adapter must negotiate the approval capability.
  require(
      router.submit(approver, approve_action("no-capability", "no-capability")).error().code() ==
      wire::CAPABILITY_NOT_SUPPORTED);
  router.attach(adapter, approve_registration());
  require(router.submit(approver, approve_action("empty", "empty", "")).error().code() ==
          wire::INVALID_ARGUMENT);
  require(router
              .submit(approver,
                      approve_action("reason", "reason", "request-1", true, std::string(257, 'x')))
              .error()
              .code() == wire::INVALID_ARGUMENT);
  require(router.submit(approver, approve_action("approve-1", "approve-1")).result().status() ==
          wire::ACCEPTED);
  const auto sent = value(router.next(adapter));
  require(sent && sent->action_id() == "approve-1" && sent->has_approve_request() &&
          sent->approve_request().request_id() == "request-1" && sent->approve_request().approve());
  wire::ActionResult completed;
  completed.set_action_id("approve-1");
  completed.set_status(wire::SUCCEEDED);
  completed.mutable_completed_at()->set_seconds(1001);
  value(router.complete(adapter, completed));
  require(router.query(approver, query("approve-1")).result().status() == wire::SUCCEEDED &&
          router.query(approver, query("approve-1")).result().platform_message_id().empty());
  require(router.submit(approver, approve_action("approve-1", "approve-1")).result().action_id() ==
          "approve-1");
  require(!value(router.next(adapter)));
}

} // namespace
int main() try {
  kiarabot::core::InMemoryRuntimeStore store;
  scenarios(store, &store);
  capacity();
  quota();
  budgets();
  reactions();
  deletions();
  moderation();
  approvals();
  // Actual SQLite persistence including reopening schema v3.
  const auto path = std::filesystem::temp_directory_path() /
                    ("kiarabot-action-" + kiarabot::core::random_instance_id() + ".db");
  {
    auto database = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    scenarios(*database);
  }
  {
    auto database = value(kiarabot::infrastructure::SqliteRuntimeStore::open(path));
    ActionRouter router(
        *database, [] { return RuntimeTimePoint(1100s); }, {}, permissive_quota());
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
