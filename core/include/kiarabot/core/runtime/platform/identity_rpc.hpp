#pragma once

#include <functional>
#include <memory>

#include <grpcpp/server_context.h>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace kiarabot::core::platform {

// Composition-owned verifier: it must use real auth_context evidence AND current Identity
// registration/credential/SAN checks. CA validity alone never constructs an authorized peer.
// This synchronous callback must respect ServerContext's deadline; no caller identity claims.
using PeerAuthenticator =
    std::function<target::Outcome<target::VerifiedPeer>(grpc::ServerContext&)>;

// Borrows application for the service lifetime; owns the verifier closure. No readiness or
// listener is enabled here. Six security RPCs call the pure application; other owner RPCs
// return a stable RpcFailure/CAPABILITY_NOT_SUPPORTED, never the legacy reference service.
target::Outcome<std::unique_ptr<v1::IdentityService::Service>>
make_identity_rpc(target::IdentitySecurityApplication& application, PeerAuthenticator authenticate);

} // namespace kiarabot::core::platform
