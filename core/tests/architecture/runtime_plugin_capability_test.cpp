#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "kiarabot/core/application/target/runtime_plugin_capability.hpp"

namespace target = kiarabot::core::target;

namespace {
void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
template <typename Value>
void rejected(const target::Outcome<Value>& result, target::ErrorCode expected,
              const char* scenario) {
  const auto* failure = std::get_if<target::Failure>(&result);
  check(failure && failure->code == expected, scenario);
}
target::PluginCapabilityProfile profile() {
  return {{"task.schedule", "task.read", "storage.namespace"},
          target::InstallationId{"registered-installation"},
          {"host.tasks.v1"},
          {"event.timer.triggered"},
          {"authenticated-instance"}};
}
void task_admission() {
  const auto registered = profile();
  const auto instance = registered.instance;
  for (const auto operation :
       {target::PluginTaskOperation::Create, target::PluginTaskOperation::Read,
        target::PluginTaskOperation::Cancel}) {
    check(std::holds_alternative<std::monostate>(
              target::admit_plugin_task(instance, registered, operation)),
          "registered grants admit each task operation");
  }
  auto altered = registered;
  altered.permissions = {"task.read"};
  rejected(target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Create),
           target::ErrorCode::PermissionDenied, "task.read never grants create");
  rejected(target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Cancel),
           target::ErrorCode::PermissionDenied, "task.read never grants cancel");
  altered.permissions = {"task.schedule"};
  rejected(target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Read),
           target::ErrorCode::PermissionDenied, "task.schedule never grants read");
  altered = registered;
  altered.subscriptions.clear();
  rejected(target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Create),
           target::ErrorCode::PermissionDenied, "create requires TimerTriggered subscription");
  check(std::holds_alternative<std::monostate>(
            target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Read)),
        "read does not require a subscription");
  check(std::holds_alternative<std::monostate>(
            target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Cancel)),
        "cancel does not require a subscription");
  altered = registered;
  altered.features.clear();
  rejected(target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Create),
           target::ErrorCode::CapabilityNotSupported, "unnegotiated task capability is rejected");
  altered.installation.reset();
  rejected(target::admit_plugin_task(instance, altered, target::PluginTaskOperation::Create),
           target::ErrorCode::PermissionDenied,
           "missing installation retains rejection precedence");
  rejected(target::admit_plugin_task({"another-instance"}, registered,
                                     target::PluginTaskOperation::Read),
           target::ErrorCode::Unauthenticated, "profile cannot authorize another instance");
  rejected(target::admit_plugin_task({}, registered, target::PluginTaskOperation::Read),
           target::ErrorCode::Unauthenticated, "missing authenticated identity fails closed");
}
void model_admission() {
  class Model final : public target::RuntimeModelPort {
  public:
    int calls = 0;
    target::RuntimeModelChatRequest received;
    target::Outcome<target::RuntimeModelChatReply>
    chat(const target::RuntimeModelChatRequest& request, target::TimePoint) override {
      ++calls;
      received = request;
      return target::RuntimeModelChatReply{"reply", 1, 2};
    }
    bool available() const noexcept override {
      return true;
    }
  } model;
  const auto registered = profile();
  target::RuntimeModelChatRequest request;
  request.instance = {"client-owner"};
  request.installation = {"client-installation"};
  request.request_id = "request";
  request.logical_call_id = "logical-call";
  request.model = "model";
  request.messages = {{"user", "hello"}};
  const target::TimePoint deadline{std::chrono::seconds{1000}};
  const auto invoke = [&](const std::vector<std::string>& grants,
                          const target::PluginCapabilityProfile& trusted) {
    return target::submit_plugin_model(model, registered.instance, grants, trusted, request,
                                       deadline);
  };
  rejected(invoke({}, registered), target::ErrorCode::PermissionDenied,
           "model requires a grant from the verified registration response");
  auto altered = registered;
  altered.installation.reset();
  rejected(invoke({"model.chat"}, altered), target::ErrorCode::PermissionDenied,
           "model requires the registered installation");
  altered = registered;
  altered.instance = {"another-instance"};
  rejected(invoke({"model.chat"}, altered), target::ErrorCode::Unauthenticated,
           "model rejects profile/session identity mismatch");
  check(model.calls == 0, "model admission refusals have no downstream side effects");
  const auto accepted = invoke({"model.chat"}, registered);
  check(std::holds_alternative<target::RuntimeModelChatReply>(accepted) && model.calls == 1 &&
            model.received.instance == registered.instance &&
            model.received.installation == *registered.installation &&
            model.received.logical_call_id == request.logical_call_id,
        "model derives owner/installation from trusted registration and preserves call identity");
  request.logical_call_id.clear();
  rejected(invoke({"model.chat"}, registered), target::ErrorCode::InvalidArgument,
           "invalid model content is rejected before downstream");
  check(model.calls == 1, "invalid model content cannot call downstream");
}

void storage_authority() {
  const auto registered = profile();
  target::BridgeAuthorization authorization;
  authorization.fence.instance = registered.instance;
  authorization.authorization_version = 7;
  authorization.issued_at = target::TimePoint{std::chrono::seconds{1000}};
  authorization.expires_at = authorization.issued_at + std::chrono::seconds{56};
  const auto now = authorization.issued_at + std::chrono::seconds{1};
  const target::PluginStorageMetadata metadata{"request", "trace", "plugin.storage.get",
                                               "idempotency", now + std::chrono::seconds{30}};
  const auto make_context = [&](const target::PluginCapabilityProfile& trusted,
                                target::RequestedScopeKind requested) {
    return target::plugin_storage_context(authorization, trusted, requested, metadata, now);
  };
  auto result = make_context(registered, target::RequestedScopeKind::DefaultPrivate);
  check(std::holds_alternative<target::StorageCommandContext>(result),
        "private storage context is admitted without gRPC");
  const auto& context = std::get<target::StorageCommandContext>(result);
  check(context.instance == registered.instance &&
            context.authentication.subject.value == registered.instance.value &&
            context.authentication.scopes.size() == 1 && context.bridge_authorization &&
            context.bridge_authorization->scope ==
                std::vector<target::ResourceRef>{
                    {target::ResourceKind::Instance, {registered.instance.value}}} &&
            context.authentication.authorization_version.value == 7 &&
            context.bridge_authorization->checked_at == now &&
            context.authentication.expires_at == authorization.expires_at,
        "private owner, scope, version and time come from the verified session");
  result = make_context(registered, target::RequestedScopeKind::InstallationShared);
  check(std::holds_alternative<target::StorageCommandContext>(result),
        "registered installation admits shared context");
  check(std::get<target::StorageCommandContext>(result).bridge_authorization->scope ==
            std::vector<target::ResourceRef>{
                {target::ResourceKind::Installation, {registered.installation->value}}},
        "shared authority uses server registered installation");
  auto altered = registered;
  altered.permissions.clear();
  rejected(make_context(altered, target::RequestedScopeKind::DefaultPrivate),
           target::ErrorCode::PermissionDenied, "storage needs storage.namespace");
  altered = registered;
  altered.instance = {"self-reported-instance"};
  rejected(make_context(altered, target::RequestedScopeKind::DefaultPrivate),
           target::ErrorCode::Unauthenticated,
           "storage profile/session identity mismatch is denied");
  altered = registered;
  altered.installation.reset();
  rejected(make_context(altered, target::RequestedScopeKind::InstallationShared),
           target::ErrorCode::PermissionDenied, "shared storage requires registered installation");
  check(std::holds_alternative<target::StorageCommandContext>(
            make_context(altered, target::RequestedScopeKind::DefaultPrivate)),
        "private storage does not require installation");
  rejected(make_context(registered, target::RequestedScopeKind::InstallationSharedId),
           target::ErrorCode::CapabilityNotSupported, "plugin cannot self-report a namespace id");
}
} // namespace

int main() {
  task_admission();
  model_admission();
  storage_authority();
  return 0;
}
