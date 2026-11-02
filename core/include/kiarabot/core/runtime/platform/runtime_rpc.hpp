#pragma once

#include <functional>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/v1/runtime.grpc.pb.h"

namespace kiarabot::core::platform {
namespace target = kiarabot::core::target;

// Trusted outer-composition dependency. It authenticates the actual inbound mTLS peer and verifies
// the current Identity delegation/authorization chain for this Runtime method, returning a trusted
// typed AuthenticationContext. It must never infer the caller from request fields, and while the
// Identity chain is not yet wired the composition root supplies a fail-closed implementation.
// A missing/unavailable chain returns Unauthenticated/UpstreamUnavailable, never an allow.
using RuntimePeerAuthorizer = std::function<target::Outcome<target::AuthenticationContext>(
    grpc::ServerContext& transport, const v1::RequestContext& request)>;

struct RuntimeRpcOptions {
  RuntimePeerAuthorizer authorize;
  // Optional process-level deadline bound (shared shutdown budget); expired -> TIMEOUT.
  std::function<target::Outcome<target::TimePoint>()> invocation_deadline;
};

// Transport adapter for the P05 production unary surface: accounts, explicit bindings, plugin
// installation/instance/config, typed resource queries and operation queries. The nine P06
// internal_dispatcher unary methods (RegisterAdapter/Heartbeat/PublishEvent/RegisterPlugin/
// HeartbeatPlugin/ClaimEvent/DeliverEvent/AckDelivery/RenewDelivery) are deliberately NOT
// overridden here: they stay generated UNIMPLEMENTED until P06 Bridge handlers reuse the Runtime
// application interfaces. Only conversion and stable error mapping live here; all business rules
// stay in RuntimeResourceApplication.
target::Outcome<std::unique_ptr<v1::RuntimeService::Service>>
make_runtime_rpc(target::RuntimeResourceApplication& application, RuntimeRpcOptions options);

} // namespace kiarabot::core::platform
