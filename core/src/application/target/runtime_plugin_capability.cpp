#include "kiarabot/core/application/target/runtime_plugin_capability.hpp"

#include <algorithm>
#include <string_view>

#include "kiarabot/core/application/target/runtime_bridge_application.hpp"

namespace kiarabot::core::target {
namespace {
Failure denied(ErrorCode code) {
  return {code, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
}
bool contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}
bool bound_to(const InstanceId& instance, const PluginCapabilityProfile& profile) {
  return !instance.value.empty() && profile.instance == instance;
}
} // namespace

Status admit_plugin_task(const InstanceId& authenticated_instance,
                         const PluginCapabilityProfile& profile, PluginTaskOperation operation) {
  if (!bound_to(authenticated_instance, profile)) {
    return denied(ErrorCode::Unauthenticated);
  }
  if (!profile.installation) {
    return denied(ErrorCode::PermissionDenied);
  }
  if (!contains(profile.features, "host.tasks.v1")) {
    return denied(ErrorCode::CapabilityNotSupported);
  }
  const auto permission = operation == PluginTaskOperation::Read ? "task.read" : "task.schedule";
  if (!contains(profile.permissions, permission)) {
    return denied(ErrorCode::PermissionDenied);
  }
  if (operation == PluginTaskOperation::Create &&
      !contains(profile.subscriptions, "event.timer.triggered")) {
    return denied(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}

Status admit_plugin_storage(const InstanceId& authenticated_instance,
                            const PluginCapabilityProfile& profile) {
  if (!bound_to(authenticated_instance, profile)) {
    return denied(ErrorCode::Unauthenticated);
  }
  if (!contains(profile.permissions, "storage.namespace")) {
    return denied(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}

Status admit_plugin_model(const std::vector<std::string>& granted_permissions) {
  if (!contains(granted_permissions, "model.chat")) {
    return denied(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}

Status admit_plugin_model_stream(const InstanceId& authenticated_instance,
                                 const PluginCapabilityProfile& profile,
                                 const std::vector<std::string>& granted_permissions) {
  if (!bound_to(authenticated_instance, profile)) {
    return denied(ErrorCode::Unauthenticated);
  }
  if (!contains(profile.features, "host.model.stream.v1") ||
      !contains(profile.features, "host.model.v1")) {
    return denied(ErrorCode::CapabilityNotSupported);
  }
  if (!contains(granted_permissions, "model.chat")) {
    return denied(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}

Outcome<RuntimeModelChatReply>
submit_plugin_model(RuntimeModelPort& model, const InstanceId& authenticated_instance,
                    const std::vector<std::string>& granted_permissions,
                    const PluginCapabilityProfile& profile, RuntimeModelChatRequest request,
                    TimePoint deadline) {
  if (const auto admitted = admit_plugin_model(granted_permissions);
      std::holds_alternative<Failure>(admitted)) {
    return std::get<Failure>(admitted);
  }
  if (!bound_to(authenticated_instance, profile)) {
    return denied(ErrorCode::Unauthenticated);
  }
  if (!profile.installation) {
    return denied(ErrorCode::PermissionDenied);
  }
  if (request.logical_call_id.empty() || request.model.empty() || request.messages.empty()) {
    return denied(ErrorCode::InvalidArgument);
  }
  request.instance = authenticated_instance;
  request.installation = *profile.installation;
  return model.chat(request, deadline);
}

Outcome<ResourceRef> plugin_storage_resource(const InstanceId& authenticated_instance,
                                             const PluginCapabilityProfile& profile,
                                             RequestedScopeKind requested) {
  if (!bound_to(authenticated_instance, profile)) {
    return denied(ErrorCode::Unauthenticated);
  }
  if (requested == RequestedScopeKind::InstallationSharedId) {
    return denied(ErrorCode::CapabilityNotSupported);
  }
  if (requested == RequestedScopeKind::InstallationShared) {
    if (!profile.installation) {
      return denied(ErrorCode::PermissionDenied);
    }
    return ResourceRef{ResourceKind::Installation, {profile.installation->value}};
  }
  return ResourceRef{ResourceKind::Instance, {authenticated_instance.value}};
}

Outcome<StorageCommandContext> plugin_storage_context(
    const BridgeAuthorization& authorization, const PluginCapabilityProfile& admitted_profile,
    RequestedScopeKind requested, PluginStorageMetadata metadata, TimePoint checked_at) {
  if (const auto admitted = admit_plugin_storage(authorization.fence.instance, admitted_profile);
      std::holds_alternative<Failure>(admitted)) {
    return std::get<Failure>(admitted);
  }
  auto resource =
      plugin_storage_resource(authorization.fence.instance, admitted_profile, requested);
  if (const auto* rejected = std::get_if<Failure>(&resource)) {
    return *rejected;
  }
  const auto& authorization_resource = std::get<ResourceRef>(resource);
  StorageCommandContext context{};
  context.authentication.subject = {authorization.fence.instance.value};
  context.authentication.direct_peer =
      PluginPeer{authorization.fence.instance, SubjectId{authorization.fence.instance.value}};
  context.authentication.audience = Service::Runtime;
  context.authentication.actors = {context.authentication.subject};
  context.authentication.scopes = {{authorization_resource, "storage.namespace"}};
  context.authentication.authorization_version = {authorization.authorization_version};
  context.authentication.verified_at = authorization.issued_at;
  context.authentication.expires_at = authorization.expires_at;
  context.request_id = std::move(metadata.request_id);
  context.trace_id = std::move(metadata.trace_id);
  context.method = std::move(metadata.method);
  context.idempotency_key = std::move(metadata.idempotency_key);
  context.deadline = metadata.deadline;
  context.instance = authorization.fence.instance;
  context.requested = requested;
  context.bridge_authorization = RuntimeAuthorizationDecision{true,
                                                              context.authentication.subject,
                                                              Permission::StorageNamespace,
                                                              {authorization_resource},
                                                              {authorization.authorization_version},
                                                              checked_at,
                                                              std::nullopt};
  return context;
}

Outcome<RuntimeBridgeApplication::BridgePluginProfile>
RuntimeBridgeApplication::task_profile(const InstanceId& instance, PluginTaskOperation operation) {
  auto found = plugin_profile(instance);
  if (const auto* rejected = std::get_if<Failure>(&found)) {
    return *rejected;
  }
  if (const auto admitted =
          admit_plugin_task(instance, std::get<BridgePluginProfile>(found), operation);
      std::holds_alternative<Failure>(admitted)) {
    return std::get<Failure>(admitted);
  }
  return found;
}

Outcome<RuntimeBridgeApplication::BridgePluginProfile>
RuntimeBridgeApplication::storage_profile(const InstanceId& instance) {
  auto found = plugin_profile(instance);
  if (const auto* rejected = std::get_if<Failure>(&found)) {
    return *rejected;
  }
  if (const auto admitted = admit_plugin_storage(instance, std::get<BridgePluginProfile>(found));
      std::holds_alternative<Failure>(admitted)) {
    return std::get<Failure>(admitted);
  }
  return found;
}

} // namespace kiarabot::core::target
