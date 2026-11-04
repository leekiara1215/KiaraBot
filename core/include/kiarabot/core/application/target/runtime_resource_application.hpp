#pragma once

#include "kiarabot/core/application/target/runtime_resources/account_adapter.hpp"
#include "kiarabot/core/application/target/runtime_resources/host_commands.hpp"
#include "kiarabot/core/application/target/runtime_resources/instances.hpp"
#include "kiarabot/core/application/target/runtime_resources/lifecycle.hpp"
#include "kiarabot/core/application/target/runtime_resources/timer.hpp"

namespace kiarabot::core::target {

// Compatibility facade; implementations delegate to independently owned narrow applications.
class RuntimeResourceApplication : public ServiceReadinessPort,
                                   public RuntimeAccountAdapterApplication,
                                   public RuntimeInstanceApplication,
                                   public RuntimeLifecycleApplication,
                                   public RuntimeHostCommandApplication,
                                   public RuntimeTimerApplication {
public:
  ~RuntimeResourceApplication() override = default;
};

struct RuntimeResourceDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeSecretPort& secrets;
  // Fail-closed composition adapter until the Identity current-authorization client is wired.
  RuntimeAuthorizationPort& authorization;
  // Trusted capability catalog this build actually supports; never request-controlled.
  std::vector<std::string> supported_capabilities;
  // P13a participant clients are wired by the composition when P08b-3/P10b clients exist. A null
  // pointer is fail-closed (participant BLOCKED/retryable), never a fake successful stop/cancel.
  HostLifecycleParticipantPort* host_lifecycle = nullptr;
  SchedulerLifecycleParticipantPort* scheduler_lifecycle = nullptr;
  std::map<ResourceKind, ResourceId> resource_catalogs{};
};

// Borrowed dependencies outlive every call; each call owns separate short UoWs.
Outcome<std::unique_ptr<RuntimeResourceApplication>>
make_runtime_resource_application(RuntimeResourceDependencies dependencies);

} // namespace kiarabot::core::target
