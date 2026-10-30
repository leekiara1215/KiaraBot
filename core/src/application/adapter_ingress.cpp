#include "kiarabot/core/application/adapter_ingress.hpp"

#include <stdexcept>
#include <unordered_set>

#include "kiarabot/core/application/plugin_hub.hpp"
#include "kiarabot/core/domain/runtime_engine.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"
#include "kiarabot/core/runtime/runtime_state.hpp"

namespace kiarabot::core {
bool AdapterIngress::connected(const std::string& identity) {
  std::lock_guard lock(mutex_);
  return instances_.contains(identity);
}

namespace {
bool valid_id(const std::string& value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == ':' ||
                  ch == '/' || ch == '@';
         });
}
kiarabot::v1::Error error(kiarabot::v1::ErrorCode code, bool retryable = false) {
  kiarabot::v1::Error result;
  result.set_code(code);
  result.set_message(kiarabot::v1::ErrorCode_Name(code));
  result.set_retryable(retryable);
  return result;
}
std::string fingerprint(kiarabot::v1::EventEnvelope event) {
  event.mutable_source()->clear_adapter_instance_id();
  return request_digest(event);
}
std::string session_key(const kiarabot::v1::EventEnvelope& event) {
  std::string result;
  for (const auto* value : {&event.source().adapter_id(), &event.source().account_id(),
                            &event.subject().conversation_id()}) {
    result += std::to_string(value->size()) + ":" + *value;
  }
  return result;
}
std::optional<kiarabot::v1::ErrorCode> validate(const AdapterSession& session,
                                                const kiarabot::v1::EventEnvelope& event) {
  if (event.source().adapter_id() != session.identity ||
      event.source().adapter_instance_id() != session.instance ||
      std::find(session.accounts.begin(), session.accounts.end(), event.source().account_id()) ==
          session.accounts.end()) {
    return kiarabot::v1::PERMISSION_DENIED;
  }
  if (!valid_id(event.event_id()) || !valid_id(event.trace_id()) ||
      event.schema_version() != "kiarabot.v1" ||
      event.delivery_mode() != kiarabot::v1::AT_LEAST_ONCE || !event.has_occurred_at() ||
      event.occurred_at().seconds() < -62135596800LL ||
      event.occurred_at().seconds() > 253402300799LL || event.occurred_at().nanos() < 0 ||
      event.occurred_at().nanos() > 999999999 || event.ByteSizeLong() > 1000000 ||
      (!event.subject().conversation_id().empty() &&
       !valid_id(event.subject().conversation_id()))) {
    return kiarabot::v1::INVALID_ARGUMENT;
  }
  const kiarabot::v1::ConversationRef* conversation = nullptr;
  switch (event.payload_case()) {
  case kiarabot::v1::EventEnvelope::kMessageReceived:
    if (!valid_id(event.message_received().message_id()) ||
        !valid_id(event.message_received().sender_id()) ||
        event.subject().user_id() != event.message_received().sender_id()) {
      return kiarabot::v1::INVALID_ARGUMENT;
    }
    conversation = &event.message_received().conversation();
    break;
  case kiarabot::v1::EventEnvelope::kMessageSent:
    if (!valid_id(event.message_sent().message_id())) {
      return kiarabot::v1::INVALID_ARGUMENT;
    }
    conversation = &event.message_sent().conversation();
    break;
  case kiarabot::v1::EventEnvelope::kGroupMemberChanged:
    if (!valid_id(event.group_member_changed().group_id()) ||
        !valid_id(event.group_member_changed().user_id()) ||
        event.group_member_changed().group_id() != event.subject().group_id() ||
        event.group_member_changed().change_type() < kiarabot::v1::GroupMemberChanged::JOINED ||
        event.group_member_changed().change_type() > kiarabot::v1::GroupMemberChanged::DEMOTED) {
      return kiarabot::v1::INVALID_ARGUMENT;
    }
    break;
  case kiarabot::v1::EventEnvelope::kFriendRequest:
    if (!valid_id(event.friend_request().user_id())) {
      return kiarabot::v1::INVALID_ARGUMENT;
    }
    break;
  case kiarabot::v1::EventEnvelope::kAdapterStatusChanged:
    if (event.adapter_status_changed().adapter_id() != session.identity) {
      return kiarabot::v1::PERMISSION_DENIED;
    }
    if (event.adapter_status_changed().state() < kiarabot::v1::HEALTHY ||
        event.adapter_status_changed().state() > kiarabot::v1::UNHEALTHY) {
      return kiarabot::v1::INVALID_ARGUMENT;
    }
    break;
  case kiarabot::v1::EventEnvelope::kTimerTriggered:
  case kiarabot::v1::EventEnvelope::kPluginStatusChanged:
    return kiarabot::v1::PERMISSION_DENIED;
  default:
    return kiarabot::v1::INVALID_ARGUMENT;
  }
  if (conversation &&
      (!valid_id(conversation->id()) || conversation->id() != event.subject().conversation_id() ||
       conversation->account_id() != event.source().account_id() ||
       conversation->type() < kiarabot::v1::ConversationRef::PRIVATE ||
       conversation->type() > kiarabot::v1::ConversationRef::CHANNEL)) {
    return kiarabot::v1::INVALID_ARGUMENT;
  }
  return std::nullopt;
}
} // namespace

AdapterIngress::AdapterIngress(RuntimeStore& store, std::vector<AdapterPolicy> policies,
                               RuntimeOptions options, std::shared_ptr<PluginHub> plugins)
    : store_(store), policies_(std::move(policies)), options_(options),
      plugins_(std::move(plugins)) {
  std::unordered_set<std::string> identities;
  for (const auto& policy : policies_) {
    if (!valid_id(policy.identity) || !identities.insert(policy.identity).second ||
        policy.accounts.empty() || policy.accounts.size() > 64) {
      throw std::invalid_argument("invalid adapter policy");
    }
    std::unordered_set<std::string> accounts;
    for (const auto& account : policy.accounts) {
      if (!valid_id(account) || !accounts.insert(account).second) {
        throw std::invalid_argument("invalid account policy");
      }
    }
  }
}
AdapterRegistration
AdapterIngress::register_adapter(const std::string& principal,
                                 const kiarabot::v1::RegisterAdapterRequest& request) {
  std::lock_guard lock(mutex_);
  AdapterRegistration result;
  result.response.set_required_protocol_version("kiarabot.v1");
  auto reject = [&](kiarabot::v1::ErrorCode code) {
    *result.response.mutable_error() = error(code);
    return result;
  };
  const auto policy = std::find_if(policies_.begin(), policies_.end(),
                                   [&](const auto& item) { return item.identity == principal; });
  if (principal != request.adapter_id() || policy == policies_.end()) {
    return reject(kiarabot::v1::PERMISSION_DENIED);
  }
  if (request.protocol_version() != "kiarabot.v1" || request.implementation().empty() ||
      request.implementation().size() > 128 || request.implementation_version().empty() ||
      request.implementation_version().size() > 128 || request.accounts_size() < 1 ||
      request.accounts_size() > 64 || request.capabilities_size() > 128) {
    return reject(kiarabot::v1::INVALID_ARGUMENT);
  }
  for (const auto& account : request.accounts()) {
    if (!account.enabled() || std::find(policy->accounts.begin(), policy->accounts.end(),
                                        account.account_id()) == policy->accounts.end()) {
      return reject(kiarabot::v1::PERMISSION_DENIED);
    }
    if (std::find(result.session.accounts.begin(), result.session.accounts.end(),
                  account.account_id()) != result.session.accounts.end()) {
      return reject(kiarabot::v1::INVALID_ARGUMENT);
    }
    result.session.accounts.push_back(account.account_id());
  }
  std::unordered_set<std::string> capabilities;
  for (const auto& capability : request.capabilities()) {
    if (!valid_id(capability.name()) || capability.version().empty() ||
        capability.version().size() > 128 || !capabilities.insert(capability.name()).second) {
      return reject(kiarabot::v1::INVALID_ARGUMENT);
    }
  }
  result.session.identity = principal;
  result.session.instance = random_instance_id();
  result.response.set_accepted(true);
  result.response.set_assigned_instance_id(result.session.instance);
  result.response.set_heartbeat_interval_seconds(30);
  instances_[principal] = result.session.instance;
  return result;
}

kiarabot::v1::AdapterEventAck AdapterIngress::ingest(const AdapterSession& session,
                                                     const kiarabot::v1::EventEnvelope& event) {
  std::lock_guard lock(mutex_);
  kiarabot::v1::AdapterEventAck response;
  response.set_event_id(event.event_id());
  response.set_trace_id(event.trace_id());
  auto reject = [&](kiarabot::v1::Error failure) {
    response.set_status(kiarabot::v1::AdapterEventAck::REJECTED);
    failure.set_request_id(event.event_id());
    failure.set_trace_id(event.trace_id());
    if (failure.retryable()) {
      response.set_retry_after_ms(1000);
    }
    *response.mutable_error() = std::move(failure);
    return response;
  };
  try {
    const auto active = instances_.find(session.identity);
    if (active == instances_.end() || active->second != session.instance) {
      return reject(error(kiarabot::v1::CONFLICT));
    }
    if (const auto failure = validate(session, event)) {
      return reject(error(*failure));
    }
    std::vector<PermissionGrant> grants;
    for (const auto& policy : policies_) {
      for (const auto& account : policy.accounts) {
        grants.push_back({"adapter/" + policy.identity, "event.publish", account});
      }
    }
    AuthorizedCommands commands(store_, std::move(grants), {});
    const auto digest = fingerprint(event);
    CommandContext context{"adapter/" + session.identity,
                           "event.publish",
                           event.source().account_id(),
                           event.event_id(),
                           event.trace_id(),
                           event.event_id(),
                           digest};
    bool created = false;
    const auto result = commands.execute(context, [&](RuntimeSnapshot& snapshot) -> PortStatus {
      const auto previous = snapshot.events.find(event.event_id());
      if (previous != snapshot.events.end()) {
        kiarabot::v1::EventEnvelope original;
        if (!original.ParseFromString(previous->second.record.serialized_envelope)) {
          return error(kiarabot::v1::INTERNAL);
        }
        return fingerprint(original) == digest ? PortStatus(std::monostate{})
                                               : PortStatus(error(kiarabot::v1::CONFLICT));
      }
      detail::RuntimeState state(snapshot);
      detail::RuntimeEngine engine(state, options_);
      const auto accepted =
          engine.ingest({event.event_id(), event.trace_id(), session_key(event), event.sequence(),
                         event.sequence() != 0, event.SerializeAsString()},
                        plugins_ ? plugins_->targets(snapshot, event) : std::vector<std::string>{});
      if (accepted != EventResult::accepted) {
        return error(accepted == EventResult::rate_limited   ? kiarabot::v1::RATE_LIMITED
                     : accepted == EventResult::out_of_order ? kiarabot::v1::CONFLICT
                     : accepted == EventResult::invalid      ? kiarabot::v1::INVALID_ARGUMENT
                                                             : kiarabot::v1::INTERNAL);
      }
      auto updated = state.snapshot();
      updated.audit = std::move(snapshot.audit);
      updated.commands = std::move(snapshot.commands);
      snapshot = std::move(updated);
      created = true;
      return std::monostate{};
    });
    if (const auto* failure = std::get_if<kiarabot::v1::Error>(&result)) {
      return reject(*failure);
    }
    response.set_status(created ? kiarabot::v1::AdapterEventAck::PERSISTED
                                : kiarabot::v1::AdapterEventAck::DUPLICATE);
    return response;
  } catch (...) {
    return reject(error(kiarabot::v1::INTERNAL));
  }
}

bool AdapterIngress::current(const AdapterSession& session) {
  std::lock_guard lock(mutex_);
  const auto active = instances_.find(session.identity);
  return active != instances_.end() && active->second == session.instance;
}
void AdapterIngress::disconnect(const AdapterSession& session) {
  std::lock_guard lock(mutex_);
  const auto active = instances_.find(session.identity);
  if (active != instances_.end() && active->second == session.instance) {
    instances_.erase(active);
  }
}
} // namespace kiarabot::core
