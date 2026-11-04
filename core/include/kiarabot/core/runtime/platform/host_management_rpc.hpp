#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/host.grpc.pb.h>

#include "kiarabot/core/application/target/host_management.hpp"
#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {

// Verified caller of one HostManagement owner call: actual inbound mTLS leaf + service-verified
// Control delegation proof. The transport never accepts self-reported identity from the wire.
struct HostManagementPeer {
  target::AuthenticationContext authentication;
  target::TimePoint deadline;
};

// Extracts the real inbound leaf, binds the request proof to it and returns the decoded Control
// delegation context. Absent/failed delegation must be a stable Unauthenticated failure. Online
// current-authority verification remains in the application authorization port.
using HostManagementPeerAuthorizer = std::function<target::Outcome<HostManagementPeer>(
    grpc::ServerContext&, const v1::RequestContext&, std::string_view method,
    const v1::ResourceRef& scope_resource)>;

struct HostManagementRpcDependencies {
  target::HostManagementApplicationPort& application;
  HostManagementPeerAuthorizer authorize;
  // Test hooks; production composition leaves both empty.
  std::function<target::Outcome<target::TimePoint>()> invocation_deadline;
};

// HOST-04 HostManagementService owner mTLS transport. Maps typed wire calls to the HOST-02
// application only: no cross-database access, no invented progress and no HostService
// spawn/stop exposure. Every call authenticates a registered Control peer with a fresh Identity
// delegation for the exact RESOURCE_HOST; stored operation/resource identity decides visibility.
target::Outcome<std::unique_ptr<v1::HostManagementService::Service>>
make_host_management_rpc(HostManagementRpcDependencies dependencies);

} // namespace kiarabot::core::platform
