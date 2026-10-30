#include "kiarabot/core/plugin_hub.hpp"

#include <stdexcept>
#include <unordered_set>

#include "kiarabot/core/control_service.hpp"
#include "kiarabot/core/request_digest.hpp"
#include "kiarabot/core/runtime_engine.hpp"

namespace kiarabot::core {
bool PluginHub::connected(const std::string& identity) {
  std::lock_guard lock(mutex_);
  return active_.contains(identity);
}
namespace {
kiarabot::v1::Error error(kiarabot::v1::ErrorCode code, bool retryable = false) {
  kiarabot::v1::Error result;
  result.set_code(code);
  result.set_message(kiarabot::v1::ErrorCode_Name(code));
  result.set_retryable(retryable);
  return result;
}
kiarabot::v1::Error mapped(StorageError failure) {
  return error(failure == StorageError::Conflict  ? kiarabot::v1::CONFLICT
               : failure == StorageError::Corrupt ? kiarabot::v1::INTERNAL
                                                  : kiarabot::v1::UPSTREAM_UNAVAILABLE,
               failure != StorageError::Corrupt);
}
bool valid_id(const std::string& value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
         });
}
std::string subscription(const kiarabot::v1::EventEnvelope& event) {
  switch (event.payload_case()) {
  case kiarabot::v1::EventEnvelope::kMessageReceived:
    return "event.message.received";
  case kiarabot::v1::EventEnvelope::kMessageSent:
    return "event.message.sent";
  case kiarabot::v1::EventEnvelope::kGroupMemberChanged:
    return "event.group.member.changed";
  case kiarabot::v1::EventEnvelope::kFriendRequest:
    return "event.friend.request";
  case kiarabot::v1::EventEnvelope::kAdapterStatusChanged:
    return "event.adapter.status.changed";
  default:
    return {};
  }
}
std::string required_permission(const std::string& capability) {
  if (capability == "event.timer.triggered") {
    return "task.schedule";
  }
  if (capability == "event.message.received" || capability == "event.message.sent") {
    return "message.read";
  }
  if (capability == "event.group.member.changed") {
    return "member.read";
  }
  if (capability == "event.friend.request") {
    return "request.read";
  }
  if (capability == "event.adapter.status.changed") {
    return "adapter.read";
  }
  return {};
}
void replace_state(RuntimeSnapshot& target, const detail::RuntimeState& state) {
  auto updated = state.snapshot();
  updated.audit = std::move(target.audit);
  updated.commands = std::move(target.commands);
  target = std::move(updated);
}
PortStatus commit(RuntimeTransaction& transaction) {
  const auto result = transaction.commit();
  if (const auto* failure = std::get_if<StorageError>(&result)) {
    return mapped(*failure);
  }
  return std::monostate{};
}
} // namespace

PluginHub::PluginHub(RuntimeStore& store, std::vector<PluginPolicy> policies, RuntimeClock clock)
    : store_(store), policies_(std::move(policies)), clock_(std::move(clock)) {
  std::unordered_set<std::string> ids;
  for (const auto& policy : policies_) {
    if (!valid_id(policy.identity) || !ids.insert(policy.identity).second) {
      throw std::invalid_argument("invalid plugin policy");
    }
    for (const auto& permission : policy.permissions) {
      if (!valid_id(permission)) {
        throw std::invalid_argument("invalid plugin permission");
      }
    }
  }
}
PluginRegistration PluginHub::attach(const std::string& principal,
                                     const kiarabot::v1::RegisterPluginRequest& request) {
  std::lock_guard lock(mutex_);
  PluginRegistration result;
  result.response.set_required_api_version("kiarabot.plugin.v1");
  auto reject = [&](kiarabot::v1::Error failure) {
    *result.response.mutable_error() = std::move(failure);
    return result;
  };
  try {
    const auto policy = std::find_if(policies_.begin(), policies_.end(), [&](const auto& entry) {
      return entry.identity == principal;
    });
    const auto& manifest = request.manifest();
    if (manifest.id() != principal || policy == policies_.end()) {
      return reject(error(kiarabot::v1::PERMISSION_DENIED));
    }
    if (!valid_id(request.instance_id()) || manifest.api_version() != "kiarabot.plugin.v1" ||
        manifest.name().empty() || manifest.name().size() > 128 || !valid_id(manifest.version()) ||
        manifest.entrypoint().empty() || manifest.entrypoint().size() > 256 ||
        manifest.permissions_size() > 128 || manifest.capabilities_size() > 128) {
      return reject(error(kiarabot::v1::INVALID_ARGUMENT));
    }
    if (manifest.runtime() != "python-rpc" || manifest.isolation() != "process" ||
        !manifest.dependencies().empty() ||
        (!manifest.config_schema().empty() && manifest.config_schema() != "{}")) {
      return reject(error(kiarabot::v1::CAPABILITY_NOT_SUPPORTED));
    }
    if (active_.contains(principal)) {
      return reject(error(kiarabot::v1::CONFLICT, true));
    }
    const auto fence = random_instance_id();
    std::vector<std::string> permissions(manifest.permissions().begin(),
                                         manifest.permissions().end());
    std::sort(permissions.begin(), permissions.end());
    AuthorizedCommands commands(store_, {{"plugin/" + principal, "plugin.connect", principal}},
                                clock_);
    CommandContext context{"plugin/" + principal,  "plugin.connect", principal, fence, fence, fence,
                           request_digest(request)};
    const auto outcome = commands.execute(context, [&](RuntimeSnapshot& snapshot) -> PortStatus {
      if (!control_plugin_enabled(snapshot, principal)) {
        return error(kiarabot::v1::PLUGIN_UNAVAILABLE);
      }
      for (const auto& permission : permissions) {
        if (std::find(policy->permissions.begin(), policy->permissions.end(), permission) ==
            policy->permissions.end()) {
          return error(kiarabot::v1::PERMISSION_DENIED);
        }
      }
      if (std::adjacent_find(permissions.begin(), permissions.end()) != permissions.end()) {
        return error(kiarabot::v1::INVALID_ARGUMENT);
      }
      std::unordered_set<std::string> subscriptions;
      for (const auto& capability : manifest.capabilities()) {
        const auto required = required_permission(capability);
        if (required.empty()) {
          return error(kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
        }
        if (!std::binary_search(permissions.begin(), permissions.end(), required)) {
          return error(kiarabot::v1::PERMISSION_DENIED);
        }
        if (!subscriptions.insert(capability).second) {
          return error(kiarabot::v1::INVALID_ARGUMENT);
        }
      }
      const auto previous = snapshot.plugins.find(principal);
      if (previous != snapshot.plugins.end() &&
          (previous->second.api_version != manifest.api_version() ||
           previous->second.permissions != permissions ||
           previous->second.ordered != manifest.ordered())) {
        return error(kiarabot::v1::CONFLICT);
      }
      detail::RuntimeState state(snapshot);
      detail::RuntimeEngine engine(state, {}, clock_);
      if (previous != snapshot.plugins.end()) {
        if (previous->second.state != PluginState::failed &&
            !engine.plugin_disconnected(principal)) {
          return error(kiarabot::v1::INTERNAL);
        }
        if (previous->second.state == PluginState::unavailable) {
          engine.update_plugin_recovery(principal);
        }
        if (engine.recover_plugin_inflight(principal).storage_error) {
          return error(kiarabot::v1::INTERNAL);
        }
      }
      if (!engine.register_plugin(principal, manifest.api_version(), permissions,
                                  manifest.ordered())) {
        return error(kiarabot::v1::CONFLICT);
      }
      if (state.snapshot().plugins.at(principal).state == PluginState::ready &&
          !engine.plugin_started(principal)) {
        return error(kiarabot::v1::INTERNAL);
      }
      replace_state(snapshot, state);
      snapshot.namespaces["$control.manifests"][principal] = {control_json(manifest).dump(), {}};
      return std::monostate{};
    });
    if (const auto* failure = std::get_if<kiarabot::v1::Error>(&outcome)) {
      return reject(*failure);
    }
    result.session = {principal,
                      request.instance_id(),
                      fence,
                      {manifest.capabilities().begin(), manifest.capabilities().end()},
                      permissions};
    if (configuration_ &&
        std::find(request.supported_features().begin(), request.supported_features().end(),
                  "plugin.config.v1") != request.supported_features().end()) {
      result.session.config_enabled = true;
      configuration_->attach(principal, fence);
      result.response.add_enabled_features("plugin.config.v1");
    }
    active_[principal] = result.session;
    result.response.set_accepted(true);
    for (const auto& permission : permissions) {
      result.response.add_granted_permissions(permission);
    }
    return result;
  } catch (...) {
    return reject(error(kiarabot::v1::INTERNAL));
  }
}

void PluginHub::set_configuration(std::shared_ptr<PluginConfigDelivery> configuration) {
  configuration_ = std::move(configuration);
}
PortResult<std::optional<kiarabot::v1::PluginConfig>>
PluginHub::next_config(const PluginSession& session) {
  std::lock_guard lock(mutex_);
  if (!current_locked(session)) {
    return error(kiarabot::v1::PERMISSION_DENIED);
  }
  if (!session.config_enabled) {
    return std::optional<kiarabot::v1::PluginConfig>{};
  }
  return configuration_->next(session.identity, session.fence);
}
PortStatus PluginHub::acknowledge_config(const PluginSession& session,
                                         const kiarabot::v1::PluginConfigAck& ack) {
  std::lock_guard lock(mutex_);
  if (!current_locked(session)) {
    return error(kiarabot::v1::PERMISSION_DENIED);
  }
  if (!session.config_enabled) {
    return error(kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
  }
  return configuration_->acknowledge(session.identity, session.fence, ack);
}

bool PluginHub::current_locked(const PluginSession& session) const {
  const auto found = active_.find(session.identity);
  return found != active_.end() && found->second.fence == session.fence;
}
bool PluginHub::current(const PluginSession& session) {
  std::lock_guard lock(mutex_);
  return current_locked(session);
}
std::vector<std::string> PluginHub::targets(const RuntimeSnapshot& snapshot,
                                            const kiarabot::v1::EventEnvelope& event) {
  std::lock_guard lock(mutex_);
  std::vector<std::string> result;
  const auto capability = subscription(event);
  for (const auto& [id, session] : active_) {
    const auto plugin = snapshot.plugins.find(id);
    if (control_plugin_enabled(snapshot, id) && plugin != snapshot.plugins.end() &&
        plugin->second.state == PluginState::running &&
        std::find(session.subscriptions.begin(), session.subscriptions.end(), capability) !=
            session.subscriptions.end()) {
      result.push_back(id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

PortResult<std::optional<kiarabot::v1::EventEnvelope>>
PluginHub::next(const PluginSession& session) try {
  std::lock_guard lock(mutex_);
  if (!current_locked(session)) {
    return error(kiarabot::v1::PERMISSION_DENIED);
  }
  auto begun = store_.begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return mapped(*failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  if (!transaction) {
    return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, true);
  }
  auto& snapshot = transaction->snapshot();
  if (!control_plugin_enabled(snapshot, session.identity)) {
    return error(kiarabot::v1::PLUGIN_UNAVAILABLE);
  }
  if (session.config_enabled && !configuration_->ready(session.identity, session.fence)) {
    return std::optional<kiarabot::v1::EventEnvelope>{};
  }
  for (const auto& [event_id, deliveries] : snapshot.deliveries) {
    const auto delivery = deliveries.find(session.identity);
    if (delivery != deliveries.end() && delivery->second.state == DeliveryState::dispatched) {
      return std::optional<kiarabot::v1::EventEnvelope>{};
    }
  }
  detail::RuntimeState state(snapshot);
  detail::RuntimeEngine engine(state, {}, clock_);
  const auto candidates = engine.dispatchable_deliveries(session.identity);
  if (candidates.empty()) {
    return std::optional<kiarabot::v1::EventEnvelope>{};
  }
  const auto& candidate = candidates.front();
  const auto event = snapshot.events.find(candidate.event_id);
  kiarabot::v1::EventEnvelope envelope;
  if (event == snapshot.events.end() ||
      !envelope.ParseFromString(event->second.record.serialized_envelope)) {
    return error(kiarabot::v1::INTERNAL);
  }
  envelope.set_trace_id(candidate.trace_id);
  if (!engine.mark_plugin_dispatched(candidate.event_id, session.identity)) {
    return error(kiarabot::v1::CONFLICT, true);
  }
  replace_state(snapshot, state);
  const auto committed = commit(*transaction);
  if (const auto* failure = std::get_if<kiarabot::v1::Error>(&committed)) {
    return *failure;
  }
  return std::optional<kiarabot::v1::EventEnvelope>{std::move(envelope)};
} catch (...) {
  return error(kiarabot::v1::INTERNAL);
}

PortStatus PluginHub::acknowledge(const PluginSession& session,
                                  const kiarabot::v1::PluginEventAck& ack) try {
  std::lock_guard lock(mutex_);
  if (!current_locked(session)) {
    return error(kiarabot::v1::PERMISSION_DENIED);
  }
  if (ack.event_id().empty() || ack.event_id().size() > 128 ||
      (ack.accepted() && ack.has_error()) ||
      (!ack.accepted() &&
       (!ack.has_error() || !kiarabot::v1::ErrorCode_IsValid(ack.error().code()) ||
        ack.error().code() == kiarabot::v1::ERROR_CODE_UNSPECIFIED))) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  auto begun = store_.begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return mapped(*failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  if (!transaction) {
    return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, true);
  }
  auto& snapshot = transaction->snapshot();
  const auto event = snapshot.deliveries.find(ack.event_id());
  if (event == snapshot.deliveries.end() || !event->second.contains(session.identity)) {
    return error(kiarabot::v1::NOT_FOUND);
  }
  const auto delivery = event->second.at(session.identity);
  if (ack.accepted() && delivery.state == DeliveryState::acked) {
    return std::monostate{};
  }
  if (delivery.state != DeliveryState::dispatched) {
    return error(kiarabot::v1::CONFLICT);
  }
  if (snapshot.audit.size() >= 100000) {
    return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, true);
  }
  detail::RuntimeState state(snapshot);
  detail::RuntimeEngine engine(state, {}, clock_);
  const bool changed =
      ack.accepted()
          ? engine.acknowledge_plugin_event(ack.event_id(), session.identity)
          : engine.fail_plugin_event(ack.event_id(), session.identity, ack.error().retryable());
  if (!changed) {
    return error(kiarabot::v1::INTERNAL);
  }
  replace_state(snapshot, state);
  snapshot.audit.push_back(
      {"plugin/" + session.identity, "event.ack", ack.event_id(), ack.event_id(), delivery.trace_id,
       clock_ ? clock_() : std::chrono::system_clock::now(), true,
       ack.accepted() ? "OK" : kiarabot::v1::ErrorCode_Name(ack.error().code())});
  return commit(*transaction);
} catch (...) {
  return error(kiarabot::v1::INTERNAL);
}

PortStatus PluginHub::detach(const PluginSession& session) try {
  std::lock_guard lock(mutex_);
  if (!current_locked(session)) {
    return std::monostate{};
  }
  active_.erase(session.identity);
  if (session.config_enabled) {
    configuration_->detach(session.identity, session.fence);
  }
  auto begun = store_.begin();
  if (const auto* failure = std::get_if<StorageError>(&begun)) {
    return mapped(*failure);
  }
  auto transaction = std::get<std::unique_ptr<RuntimeTransaction>>(std::move(begun));
  if (!transaction) {
    return error(kiarabot::v1::UPSTREAM_UNAVAILABLE, true);
  }
  detail::RuntimeState state(transaction->snapshot());
  detail::RuntimeEngine engine(state, {}, clock_);
  if (!engine.plugin_disconnected(session.identity) ||
      engine.recover_plugin_inflight(session.identity).storage_error) {
    return error(kiarabot::v1::INTERNAL);
  }
  replace_state(transaction->snapshot(), state);
  return commit(*transaction);
} catch (...) {
  return error(kiarabot::v1::INTERNAL);
}
} // namespace kiarabot::core
