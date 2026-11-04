#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/operation_observation_application.hpp"
#include "kiarabot/core/application/target/scheduler_commands.hpp"
#include "kiarabot/core/application/target/scheduler_service.hpp"
#include "kiarabot/v1/scheduler.grpc.pb.h"

namespace kiarabot::core::platform {
namespace target = kiarabot::core::target;

// Trusted outer-composition seam: authenticates the actual inbound mTLS peer and verifies the
// Identity delegation/current-authorization chain, then derives the owner instance/installation
// from verified claims (never from request fields). The composition root currently supplies a
// fail-closed implementation until the Identity delegation client is wired for Scheduler; test
// fixtures inject a fake authorizer. A denial must return Unauthenticated/UpstreamUnavailable.
using SchedulerPeerAuthorizer = std::function<target::Outcome<target::SchedulerCommandContext>(
    grpc::ServerContext& transport, const v1::RequestContext& request, std::string_view method,
    const v1::ResourceScope* scope)>;

// P13h-1 deployment-authorized Runtime platform peer. Returned only after the real mTLS leaf and
// deployment allowlist matched; instance/installation/account are trusted config, never request
// fields. nullopt means "not a Runtime platform peer" and the legacy delegation authorizer runs.
struct SchedulerPlatformPeer {
  target::InstanceId instance;
  target::InstallationId installation;
  target::ResourceId account;
  bool task_read = false;
  bool task_schedule = false;
};
using SchedulerPlatformPeerAuthorizer =
    std::function<target::Outcome<std::optional<SchedulerPlatformPeer>>(
        grpc::ServerContext& transport, std::string_view method, const v1::ResourceScope* scope)>;

struct SchedulerRpcOptions {
  SchedulerPeerAuthorizer authorize;
  // Optional P13h-1 platform-peer path. Only ListTasks/CancelTask consult it; every other method
  // always uses authorize. A denied allowlisted peer never falls back to the delegation path.
  SchedulerPlatformPeerAuthorizer platform_peer;
  // Process-level deadline bound (shared shutdown budget); expired -> TIMEOUT.
  std::function<target::Outcome<target::TimePoint>()> invocation_deadline;
  // Dedicated Control read; never consults Runtime's lifecycle platform-peer path.
  target::OperationObservationApplication* observations = nullptr;
};

// Transport adapter for the frozen P10 SchedulerService surface: CreateTask/CancelTask/GetTask/
// ListTasks/GetOccurrence/GetOperation. Only wire conversion and stable error mapping live here;
// business rules, receipts and handoff state stay in SchedulerCommandHandler.
target::Outcome<std::unique_ptr<v1::SchedulerService::Service>>
make_scheduler_rpc(target::SchedulerCommandHandler& application, SchedulerRpcOptions options);

} // namespace kiarabot::core::platform
