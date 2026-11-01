#include "kiarabot/core/application/action_router.hpp"

#include <array>

#include "kiarabot/core/runtime/request_digest.hpp"

namespace kiarabot::core {
namespace {
namespace wire = kiarabot::v1;
wire::Error error(wire::ErrorCode code, const std::string& request = {},
                  const std::string& trace = {}) {
  wire::Error result;
  result.set_code(code);
  result.set_message(wire::ErrorCode_Name(code));
  result.set_request_id(request);
  result.set_trace_id(trace);
  result.set_retryable(code == wire::UPSTREAM_UNAVAILABLE || code == wire::TIMEOUT ||
                       code == wire::RATE_LIMITED);
  return result;
}
wire::Error mapped(StorageError failure) {
  auto result = error(failure == StorageError::Corrupt    ? wire::INTERNAL
                      : failure == StorageError::Conflict ? wire::CONFLICT
                                                          : wire::UPSTREAM_UNAVAILABLE);
  result.set_retryable(failure != StorageError::Corrupt);
  return result;
}
bool id(const std::string& value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
         });
}
bool timestamp(const google::protobuf::Timestamp& value) {
  return value.seconds() > 0 && value.seconds() <= 9223372035LL && value.nanos() >= 0 &&
         value.nanos() < 1000000000;
}
RuntimeTimePoint time_point(const google::protobuf::Timestamp& value) {
  return RuntimeTimePoint(std::chrono::duration_cast<RuntimeTimePoint::duration>(
      std::chrono::seconds(value.seconds()) + std::chrono::nanoseconds(value.nanos())));
}
std::string key(const std::string& owner, const std::string& value) {
  return std::to_string(owner.size()) + ":" + owner + value;
}
void expire(ActionEntry& entry, RuntimeTimePoint now) {
  if (entry.state == ActionState::accepted && entry.record.deadline <= now) {
    entry.state = ActionState::expired;
    entry.updated_at = now;
    entry.error_code = "TIMEOUT";
  }
}
wire::ActionResult result_of(const ActionEntry& entry) {
  wire::ActionResult result;
  result.set_action_id(entry.record.action_id);
  switch (entry.state) {
  case ActionState::accepted:
    result.set_status(wire::ACCEPTED);
    break;
  case ActionState::succeeded:
    result.set_status(wire::SUCCEEDED);
    break;
  case ActionState::failed:
    result.set_status(wire::FAILED);
    break;
  case ActionState::expired:
    result.set_status(wire::EXPIRED);
    break;
  case ActionState::cancelled:
    result.set_status(wire::CANCELLED);
    break;
  }
  result.set_platform_message_id(entry.result);
  if (entry.state != ActionState::accepted) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(entry.updated_at.time_since_epoch());
    result.mutable_completed_at()->set_seconds(seconds.count());
    result.mutable_completed_at()->set_nanos(
        static_cast<int>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             entry.updated_at.time_since_epoch() - seconds)
                             .count()));
  }
  if (!entry.error_code.empty()) {
    wire::ErrorCode code = wire::INTERNAL;
    (void)wire::ErrorCode_Parse(entry.error_code, &code);
    *result.mutable_error() = error(code, entry.record.action_id);
  }
  return result;
}
wire::PluginActionResponse response(const std::string& request,
                                    PortResult<wire::ActionResult> value) {
  wire::PluginActionResponse result;
  result.set_request_id(request);
  if (const auto* failure = std::get_if<wire::Error>(&value)) {
    *result.mutable_error() = *failure;
    result.mutable_error()->set_request_id(request);
  } else {
    *result.mutable_result() = std::get<wire::ActionResult>(std::move(value));
  }
  return result;
}
bool permitted(const PluginSession& session, const std::string& permission) {
  return std::find(session.permissions.begin(), session.permissions.end(), permission) !=
         session.permissions.end();
}
// Action kind doubles as the adapter capability name. New payloads extend this
// mapping together with their permission and capability vocabulary entry.
std::string action_kind(const wire::ActionEnvelope& action) {
  switch (action.payload_case()) {
  case wire::ActionEnvelope::kSendMessage:
    return "action.send_message.text.v1";
  case wire::ActionEnvelope::kReactMessage:
    return "action.react_message.v1";
  case wire::ActionEnvelope::kDeleteMessage:
    return "action.delete_message.v1";
  case wire::ActionEnvelope::kMuteMember:
    return "action.mute_member.v1";
  case wire::ActionEnvelope::kKickMember:
    return "action.kick_member.v1";
  case wire::ActionEnvelope::kApproveRequest:
    return "action.approve_request.v1";
  default:
    return {};
  }
}
std::string required_permission(const wire::ActionEnvelope& action) {
  switch (action.payload_case()) {
  case wire::ActionEnvelope::kSendMessage:
    return "message.send";
  case wire::ActionEnvelope::kReactMessage:
    return "message.react";
  case wire::ActionEnvelope::kDeleteMessage:
    return "message.delete";
  case wire::ActionEnvelope::kMuteMember:
    return "member.mute";
  case wire::ActionEnvelope::kKickMember:
    return "member.kick";
  case wire::ActionEnvelope::kApproveRequest:
    return "request.approve";
  default:
    return {};
  }
}
// Platform-neutral reaction tokens mapped per adapter; unknown tokens are
// rejected before reaching the platform.
constexpr std::array<std::string_view, 5> kReactionTokens{"like", "laugh", "surprise", "sad",
                                                          "angry"};
std::string conversation_id(const wire::ActionEnvelope& action) {
  if (action.has_send_message()) {
    return action.send_message().conversation().id();
  }
  if (action.has_react_message()) {
    return action.react_message().conversation().id();
  }
  if (action.has_delete_message()) {
    return action.delete_message().conversation().id();
  }
  if (action.has_mute_member()) {
    return action.mute_member().group_id();
  }
  if (action.has_kick_member()) {
    return action.kick_member().group_id();
  }
  return {};
}
bool group_conversation(const wire::ActionEnvelope& action) {
  if (action.has_send_message()) {
    return action.send_message().conversation().type() == wire::ConversationRef::GROUP;
  }
  if (action.has_react_message()) {
    return action.react_message().conversation().type() == wire::ConversationRef::GROUP;
  }
  if (action.has_delete_message()) {
    return action.delete_message().conversation().type() == wire::ConversationRef::GROUP;
  }
  // Group management payloads are group-scoped by definition.
  return action.has_mute_member() || action.has_kick_member();
}
// Payloads that address an existing conversation share the same scope checks:
// group/private only, same account as the Action source, and a matching target.
wire::ErrorCode validate_conversation(const wire::ActionEnvelope& action,
                                      const wire::ConversationRef& conversation) {
  if (conversation.type() != wire::ConversationRef::PRIVATE &&
      conversation.type() != wire::ConversationRef::GROUP) {
    return wire::CAPABILITY_NOT_SUPPORTED;
  }
  if (!id(conversation.id()) || conversation.account_id() != action.target_source().account_id() ||
      action.target().conversation_id() != conversation.id()) {
    return wire::INVALID_ARGUMENT;
  }
  return wire::ERROR_CODE_UNSPECIFIED;
}
std::uint64_t utc_day(RuntimeTimePoint now) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::hours>(now.time_since_epoch()).count() / 24);
}
// Persistent daily budget for dangerous actions. The counter is part of the
// Action snapshot transaction, so a rejected request rolls it back; a restart
// does not reset it.
bool consume_daily_budget(RuntimeSnapshot& state, const std::string& kind,
                          const std::string& account, std::size_t limit, RuntimeTimePoint now) {
  if (limit == 0) {
    return true;
  }
  auto& entry = state.daily_budgets[kind + "|" + account];
  const auto day = utc_day(now);
  if (entry.day != day) {
    entry.day = day;
    entry.used = 0;
  }
  if (entry.used >= limit) {
    return false;
  }
  ++entry.used;
  return true;
}
wire::ErrorCode validate(const wire::ActionEnvelope& action) {
  if (!id(action.action_id()) || !id(action.idempotency_key()) || !id(action.trace_id()) ||
      !timestamp(action.deadline()) || !id(action.target_source().adapter_id()) ||
      !id(action.target_source().account_id())) {
    return wire::INVALID_ARGUMENT;
  }
  if (action.has_react_message()) {
    const auto& react = action.react_message();
    const auto conversation = validate_conversation(action, react.conversation());
    if (conversation != wire::ERROR_CODE_UNSPECIFIED) {
      return conversation;
    }
    if (!id(react.message_id())) {
      return wire::INVALID_ARGUMENT;
    }
    if (react.reaction().empty()) {
      return wire::INVALID_ARGUMENT;
    }
    if (std::find(kReactionTokens.begin(), kReactionTokens.end(), react.reaction()) ==
        kReactionTokens.end()) {
      return wire::CAPABILITY_NOT_SUPPORTED;
    }
    return wire::ERROR_CODE_UNSPECIFIED;
  }
  if (action.has_delete_message()) {
    const auto& deletion = action.delete_message();
    const auto conversation = validate_conversation(action, deletion.conversation());
    if (conversation != wire::ERROR_CODE_UNSPECIFIED) {
      return conversation;
    }
    if (!id(deletion.message_id())) {
      return wire::INVALID_ARGUMENT;
    }
    return wire::ERROR_CODE_UNSPECIFIED;
  }
  if (action.has_mute_member()) {
    const auto& mute = action.mute_member();
    if (!id(mute.group_id()) || !id(mute.user_id()) ||
        action.target().conversation_id() != mute.group_id()) {
      return wire::INVALID_ARGUMENT;
    }
    if (mute.duration_seconds() == 0 || mute.duration_seconds() > 2592000) {
      return wire::INVALID_ARGUMENT;
    }
    return wire::ERROR_CODE_UNSPECIFIED;
  }
  if (action.has_kick_member()) {
    const auto& kick = action.kick_member();
    if (!id(kick.group_id()) || !id(kick.user_id()) ||
        action.target().conversation_id() != kick.group_id() || kick.reason().size() > 256) {
      return wire::INVALID_ARGUMENT;
    }
    return wire::ERROR_CODE_UNSPECIFIED;
  }
  if (action.has_approve_request()) {
    const auto& approve = action.approve_request();
    // request_id echoes FriendRequest.request_id (adapter-scoped safe token).
    if (!id(approve.request_id()) || approve.reason().size() > 256) {
      return wire::INVALID_ARGUMENT;
    }
    return wire::ERROR_CODE_UNSPECIFIED;
  }
  if (!action.has_send_message()) {
    return action.payload_case() == wire::ActionEnvelope::PAYLOAD_NOT_SET
               ? wire::INVALID_ARGUMENT
               : wire::CAPABILITY_NOT_SUPPORTED;
  }
  const auto& send = action.send_message();
  if (send.reply() || !send.reply_to_message_id().empty() || send.message().segments_size() != 0 ||
      (send.conversation().type() != wire::ConversationRef::PRIVATE &&
       send.conversation().type() != wire::ConversationRef::GROUP)) {
    return wire::CAPABILITY_NOT_SUPPORTED;
  }
  if (!id(send.conversation().id()) ||
      send.conversation().account_id() != action.target_source().account_id() ||
      action.target().conversation_id() != send.conversation().id() ||
      send.message().plain_text().empty() || send.message().plain_text().size() > 65536) {
    return wire::INVALID_ARGUMENT;
  }
  return wire::ERROR_CODE_UNSPECIFIED;
}
} // namespace

ActionRouter::ActionRouter(RuntimeStore& store, RuntimeClock clock, RuntimeLimits limits,
                           ActionQuotaPolicy quota)
    : store_(store), clock_(std::move(clock)), limits_(limits), quota_(quota) {}
RuntimeTimePoint ActionRouter::now() const {
  return clock_ ? clock_() : std::chrono::system_clock::now();
}
void ActionRouter::attach(const AdapterSession& session,
                          const wire::RegisterAdapterRequest& request) {
  std::lock_guard lock(mutex_);
  AdapterRoute route;
  route.session = session;
  for (const auto& capability : request.capabilities()) {
    if (capability.supported() && capability.version() == "1") {
      route.capabilities.insert(capability.name());
    }
  }
  adapters_[session.identity] = std::move(route);
}
void ActionRouter::detach(const AdapterSession& session) {
  std::lock_guard lock(mutex_);
  const auto found = adapters_.find(session.identity);
  if (found != adapters_.end() && found->second.session.instance == session.instance) {
    adapters_.erase(found);
  }
}
PortResult<wire::ActionResult> ActionRouter::lookup(const std::string& owner,
                                                    const std::string& selector, bool by_key) {
  auto begun = store_.begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return mapped(*failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  if (!transaction) {
    return error(wire::UPSTREAM_UNAVAILABLE);
  }
  auto& state = transaction->snapshot();
  auto found =
      by_key
          ? state.actions.find(key(owner, selector))
          : std::find_if(state.actions.begin(), state.actions.end(), [&](const auto& item) {
              return item.second.record.owner == owner && item.second.record.action_id == selector;
            });
  if (found == state.actions.end() || found->second.record.owner != owner) {
    return error(wire::NOT_FOUND);
  }
  auto& entry = found->second;
  const auto instant = now();
  if (entry.state != ActionState::accepted &&
      entry.updated_at + std::chrono::hours(168) <= instant) {
    return error(wire::NOT_FOUND);
  }
  if (entry.state == ActionState::accepted && entry.record.deadline <= instant) {
    expire(entry, instant);
    if (state.audit.size() >= limits_.audit_limit) {
      return error(wire::UPSTREAM_UNAVAILABLE);
    }
    state.audit.push_back({"core", "action.expire", entry.record.action_id, entry.record.action_id,
                           entry.record.action_id, instant, true, "OK"});
    const auto committed = transaction->commit();
    if (const auto* failure = std::get_if<StorageError>(&committed)) {
      return mapped(*failure);
    }
  }
  return result_of(entry);
}
wire::PluginActionResponse ActionRouter::submit(const PluginSession& session,
                                                const wire::PluginActionRequest& request) {
  std::lock_guard lock(mutex_);
  auto respond = [&](PortResult<wire::ActionResult> value) {
    auto output = response(request.request_id(), std::move(value));
    if (output.has_error()) {
      output.mutable_error()->set_trace_id(request.action().trace_id());
    }
    return output;
  };
  auto action = request.action();
  if (!id(request.request_id())) {
    return respond(error(wire::INVALID_ARGUMENT));
  }
  if (!action.requested_by().empty() && action.requested_by() != session.identity) {
    return respond(error(wire::PERMISSION_DENIED));
  }
  action.set_requested_by(session.identity);
  const auto invalid = validate(action);
  if (invalid != wire::ERROR_CODE_UNSPECIFIED) {
    return respond(error(invalid));
  }
  action.mutable_target_source()->clear_adapter_instance_id();
  auto canonical = action;
  canonical.clear_action_id();
  canonical.clear_trace_id();
  const auto fingerprint = request_digest(canonical);
  const auto permission = required_permission(action);
  std::vector<PermissionGrant> grants;
  if (permitted(session, permission)) {
    grants.push_back({"plugin/" + session.identity, permission, "actions"});
  }
  AuthorizedCommands commands(store_, grants, clock_, limits_.audit_limit);
  CommandContext context{"plugin/" + session.identity,
                         permission,
                         "actions",
                         request.request_id(),
                         action.trace_id(),
                         action.idempotency_key(),
                         fingerprint,
                         action.target_source().account_id(),
                         conversation_id(action)};
  const auto outcome = commands.execute(context, [&](RuntimeSnapshot& state) -> PortStatus {
    const auto scoped = key(session.identity, action.idempotency_key());
    if (auto existing = state.actions.find(scoped); existing != state.actions.end()) {
      return existing->second.record.fingerprint == fingerprint ? PortStatus(std::monostate{})
                                                                : PortStatus(error(wire::CONFLICT));
    }
    if (std::any_of(state.actions.begin(), state.actions.end(), [&](const auto& item) {
          return item.second.record.action_id == action.action_id();
        })) {
      return error(wire::CONFLICT);
    }
    if (state.actions.size() >= limits_.action_limit) {
      return error(wire::RATE_LIMITED);
    }
    if (time_point(action.deadline()) > now()) {
      const auto adapter = adapters_.find(action.target_source().adapter_id());
      if (adapter == adapters_.end()) {
        return error(wire::UPSTREAM_UNAVAILABLE);
      }
      const auto kind = action_kind(action);
      if (adapter->second.capabilities.find(kind) == adapter->second.capabilities.end()) {
        return error(wire::CAPABILITY_NOT_SUPPORTED);
      }
      const auto& accounts = adapter->second.session.accounts;
      if (std::find(accounts.begin(), accounts.end(), action.target_source().account_id()) ==
          accounts.end()) {
        return error(wire::PERMISSION_DENIED);
      }
      // Dangerous action kinds may be restricted to an explicit conversation
      // allowlist; unknown kinds are unrestricted by this policy.
      if (!quota_.conversation_allowed(kind, conversation_id(action))) {
        return error(wire::PERMISSION_DENIED);
      }
      if (!quota_.allow(kind, action.target_source().account_id(), conversation_id(action),
                        group_conversation(action), now())) {
        return error(wire::RATE_LIMITED);
      }
      if (!consume_daily_budget(state, kind, action.target_source().account_id(),
                                quota_.daily_limit(kind), now())) {
        return error(wire::RATE_LIMITED);
      }
    }
    ActionEntry entry;
    entry.record = {
        action.action_id(),         scoped,     time_point(action.deadline()), session.identity,
        action.SerializeAsString(), fingerprint};
    entry.updated_at = now();
    expire(entry, entry.updated_at);
    state.actions.emplace(scoped, std::move(entry));
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<wire::Error>(&outcome)) {
    return respond(*failure);
  }
  return respond(lookup(session.identity, action.idempotency_key(), true));
}
wire::PluginActionResponse ActionRouter::query(const PluginSession& session,
                                               const wire::PluginActionQuery& request) {
  std::lock_guard lock(mutex_);
  auto respond = [&](PortResult<wire::ActionResult> value) {
    auto output = response(request.request_id(), std::move(value));
    if (output.has_error()) {
      output.mutable_error()->set_trace_id(request.trace_id());
    }
    return output;
  };
  const bool by_key = request.selector_case() == wire::PluginActionQuery::kIdempotencyKey;
  const auto& selector = by_key ? request.idempotency_key() : request.action_id();
  if (!id(request.request_id()) || !id(request.trace_id()) || !id(selector) ||
      !timestamp(request.deadline())) {
    return respond(error(wire::INVALID_ARGUMENT));
  }
  if (time_point(request.deadline()) <= now()) {
    return respond(error(wire::TIMEOUT));
  }
  return respond(lookup(session.identity, selector, by_key));
}
wire::PluginActionResponse ActionRouter::cancel(const PluginSession& session,
                                                const wire::PluginActionCancel& request) {
  std::lock_guard lock(mutex_);
  auto respond = [&](PortResult<wire::ActionResult> value) {
    auto output = response(request.request_id(), std::move(value));
    if (output.has_error()) {
      output.mutable_error()->set_trace_id(request.trace_id());
    }
    return output;
  };
  if (!id(request.request_id()) || !id(request.trace_id()) || !id(request.action_id()) ||
      !timestamp(request.deadline())) {
    return respond(error(wire::INVALID_ARGUMENT));
  }
  if (time_point(request.deadline()) <= now()) {
    return respond(error(wire::TIMEOUT));
  }
  const auto existing = lookup(session.identity, request.action_id(), false);
  if (std::holds_alternative<wire::Error>(existing) ||
      std::get<wire::ActionResult>(existing).status() != wire::ACCEPTED) {
    return respond(existing);
  }
  AuthorizedCommands commands(
      store_, {{"plugin/" + session.identity, "action.cancel", request.action_id()}}, clock_,
      limits_.audit_limit);
  CommandContext context{"plugin/" + session.identity, "action.cancel",    request.action_id(),
                         request.request_id(),         request.trace_id(), request.request_id(),
                         request_digest(request)};
  const auto outcome = commands.execute(context, [&](RuntimeSnapshot& state) -> PortStatus {
    for (auto& [unused, entry] : state.actions) {
      if (entry.record.owner == session.identity && entry.record.action_id == request.action_id()) {
        expire(entry, now());
        if (entry.state != ActionState::accepted) {
          return std::monostate{};
        }
        if (entry.dispatched) {
          return error(wire::CONFLICT);
        }
        entry.state = ActionState::cancelled;
        entry.updated_at = now();
        return std::monostate{};
      }
    }
    return error(wire::NOT_FOUND);
  });
  if (const auto* failure = std::get_if<wire::Error>(&outcome)) {
    return respond(*failure);
  }
  return respond(lookup(session.identity, request.action_id(), false));
}
PortResult<std::optional<wire::ActionEnvelope>> ActionRouter::next(const AdapterSession& session) {
  std::lock_guard lock(mutex_);
  auto route = adapters_.find(session.identity);
  if (route == adapters_.end() || route->second.session.instance != session.instance) {
    return error(wire::PERMISSION_DENIED);
  }
  if (route->second.capabilities.empty()) {
    return std::optional<wire::ActionEnvelope>{};
  }
  auto loaded = store_.read();
  if (const auto* failure = std::get_if<StorageError>(&loaded)) {
    return mapped(*failure);
  }
  for (const auto& [scoped, entry] : std::get<RuntimeSnapshot>(loaded).actions) {
    if (entry.state != ActionState::accepted || entry.record.owner.empty()) {
      continue;
    }
    if (entry.record.deadline <= now()) {
      const auto expired = lookup(entry.record.owner, entry.record.action_id, false);
      if (const auto* failure = std::get_if<wire::Error>(&expired)) {
        return *failure;
      }
      continue;
    }
    if (route->second.sent.contains(entry.record.action_id)) {
      continue;
    }
    wire::ActionEnvelope action;
    if (!action.ParseFromString(entry.record.serialized_envelope)) {
      return error(wire::INTERNAL);
    }
    if (action.target_source().adapter_id() != session.identity) {
      continue;
    }
    if (route->second.capabilities.find(action_kind(action)) == route->second.capabilities.end()) {
      continue;
    }
    if (std::find(session.accounts.begin(), session.accounts.end(),
                  action.target_source().account_id()) == session.accounts.end()) {
      continue;
    }
    action.mutable_target_source()->set_adapter_instance_id(session.instance);
    AuthorizedCommands commands(store_, {{"core", "action.dispatch", entry.record.action_id}},
                                clock_, limits_.audit_limit);
    CommandContext context{"core",
                           "action.dispatch",
                           entry.record.action_id,
                           entry.record.action_id,
                           action.trace_id(),
                           session.instance,
                           request_digest(action),
                           action.target_source().account_id(),
                           conversation_id(action)};
    const auto outcome = commands.execute(context, [&](RuntimeSnapshot& state) -> PortStatus {
      auto& current = state.actions.at(scoped);
      expire(current, now());
      if (current.state == ActionState::accepted) {
        current.dispatched = true;
      }
      return std::monostate{};
    });
    if (const auto* failure = std::get_if<wire::Error>(&outcome)) {
      return *failure;
    }
    const auto latest = lookup(entry.record.owner, entry.record.action_id, false);
    if (const auto* failure = std::get_if<wire::Error>(&latest)) {
      return *failure;
    }
    if (std::get<wire::ActionResult>(latest).status() != wire::ACCEPTED) {
      continue;
    }
    route->second.sent.insert(entry.record.action_id);
    if (entry.record.deadline <= now()) {
      continue;
    }
    return std::optional<wire::ActionEnvelope>(std::move(action));
  }
  return std::optional<wire::ActionEnvelope>{};
}
PortStatus ActionRouter::complete(const AdapterSession& session, const wire::ActionResult& result) {
  std::lock_guard lock(mutex_);
  const auto route = adapters_.find(session.identity);
  if (route == adapters_.end() || route->second.session.instance != session.instance) {
    return error(wire::PERMISSION_DENIED);
  }
  if (!id(result.action_id()) ||
      (result.status() != wire::ACCEPTED && result.status() != wire::SUCCEEDED &&
       result.status() != wire::FAILED && result.status() != wire::EXPIRED) ||
      (result.status() == wire::FAILED &&
       (!result.has_error() || !wire::ErrorCode_IsValid(result.error().code()) ||
        result.error().code() == wire::ERROR_CODE_UNSPECIFIED)) ||
      (result.status() == wire::ACCEPTED && (result.has_error() || result.has_completed_at())) ||
      (result.status() != wire::ACCEPTED &&
       (!result.has_completed_at() || !timestamp(result.completed_at()))) ||
      (result.status() == wire::SUCCEEDED && result.has_error()) ||
      result.platform_message_id().size() > 256) {
    return error(wire::INVALID_ARGUMENT);
  }
  AuthorizedCommands commands(
      store_, {{"adapter/" + session.identity, "action.complete", result.action_id()}}, clock_,
      limits_.audit_limit);
  CommandContext context{"adapter/" + session.identity,
                         "action.complete",
                         result.action_id(),
                         result.action_id(),
                         result.action_id(),
                         request_digest(result),
                         request_digest(result)};
  return commands.execute(context, [&](RuntimeSnapshot& state) -> PortStatus {
    for (auto& [unused, entry] : state.actions) {
      if (entry.record.action_id != result.action_id()) {
        continue;
      }
      wire::ActionEnvelope action;
      if (!action.ParseFromString(entry.record.serialized_envelope)) {
        return error(wire::INTERNAL);
      }
      if (!entry.dispatched || action.target_source().adapter_id() != session.identity ||
          std::find(session.accounts.begin(), session.accounts.end(),
                    action.target_source().account_id()) == session.accounts.end()) {
        return error(wire::PERMISSION_DENIED);
      }
      expire(entry, now());
      if (entry.state != ActionState::accepted || result.status() == wire::ACCEPTED) {
        return std::monostate{};
      }
      if (result.status() == wire::EXPIRED && now() < entry.record.deadline) {
        return error(wire::INVALID_ARGUMENT);
      }
      entry.state = result.status() == wire::SUCCEEDED ? ActionState::succeeded
                    : result.status() == wire::EXPIRED ? ActionState::expired
                                                       : ActionState::failed;
      entry.result = result.platform_message_id();
      entry.error_code = entry.state == ActionState::expired ? "TIMEOUT"
                         : result.has_error() ? wire::ErrorCode_Name(result.error().code())
                                              : "";
      entry.updated_at = now();
      return std::monostate{};
    }
    return error(wire::NOT_FOUND);
  });
}
} // namespace kiarabot::core
