#pragma once

#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/domain/target/bridge_rules.hpp"
#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/ports/target/runtime_model.hpp"

namespace kiarabot::core::target {

// A server-owned snapshot of the registered plugin. The instance is bound by the connection
// registry; neither this identity nor the installation/permissions come from a HostRequest.
struct PluginCapabilityProfile {
  std::vector<std::string> permissions;
  std::optional<InstallationId> installation;
  std::vector<std::string> features;
  std::vector<std::string> subscriptions;
  InstanceId instance;
};

enum class PluginTaskOperation { Create, Read, Cancel };

// These application admission checks do no I/O and cannot invoke downstream ports. Callers must
// supply the authenticated connection instance and the registry's snapshot for that instance.
// Task/Storage owners retain their independent authorization, validation and transaction checks.
Status admit_plugin_task(const InstanceId& authenticated_instance,
                         const PluginCapabilityProfile& profile, PluginTaskOperation operation);
Status admit_plugin_storage(const InstanceId& authenticated_instance,
                            const PluginCapabilityProfile& profile);

// Model grants retain their original source: the verified registration result on this connection,
// not fresh client declarations or a new authorization cache. Transport calls admission before wire
// request conversion, and the application entry repeats it before invoking the Model port.
Status admit_plugin_model(const std::vector<std::string>& granted_permissions);
Outcome<RuntimeModelChatReply>
submit_plugin_model(RuntimeModelPort& model, const InstanceId& authenticated_instance,
                    const std::vector<std::string>& granted_permissions,
                    const PluginCapabilityProfile& profile, RuntimeModelChatRequest request,
                    TimePoint deadline);

struct PluginStorageMetadata {
  std::string request_id;
  std::string trace_id;
  std::string method;
  std::string idempotency_key;
  TimePoint deadline;
};

// Convert an admitted registry snapshot and the already-verified connection into storage authority.
// Private/shared owner and the authorization scope are derived here, never supplied by a frame.
Outcome<ResourceRef> plugin_storage_resource(const InstanceId& authenticated_instance,
                                             const PluginCapabilityProfile& profile,
                                             RequestedScopeKind requested);
Outcome<StorageCommandContext> plugin_storage_context(
    const BridgeAuthorization& authorization, const PluginCapabilityProfile& admitted_profile,
    RequestedScopeKind requested, PluginStorageMetadata metadata, TimePoint checked_at);

} // namespace kiarabot::core::target
