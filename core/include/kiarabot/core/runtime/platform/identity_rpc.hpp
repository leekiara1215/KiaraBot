#pragma once

#include <functional>
#include <memory>

#include <grpcpp/server_context.h>

#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/core/application/target/identity_delegation_application.hpp"
#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace kiarabot::core::platform {

// Composition-owned verifier: it must use real auth_context evidence AND current Identity
// registration/credential/SAN checks. CA validity alone never constructs an authorized peer.
// This synchronous callback must respect ServerContext's deadline; no caller identity claims.
using PeerAuthenticator =
    std::function<target::Outcome<target::VerifiedPeer>(grpc::ServerContext&)>;

// Trusted composition callback for this server's selected TLS-chain validity and current
// clock/lifecycle gate, never the remote Control peer certificate. Production must inject it;
// an empty callback is retained only for fixtures/source compatibility, never production.
using InvocationDeadline = std::function<target::Outcome<target::TimePoint>()>;

// Transport-only wrapper over the single authoritative Core authentication use case.
// Borrows application/clock until every service using the closure has stopped/Wait'ed.
target::Outcome<PeerAuthenticator>
make_identity_peer_authenticator(target::IdentityAdminApplication& application,
                                 target::SecurityClockPort& clock);

// Borrows application for the service lifetime; owns the verifier closure. No readiness or
// listener is enabled here. Optional issuer is borrowed for the same lifetime; absent means
// typed Unsupported for Issue. Six security RPCs call the pure application; other owner RPCs
// return a stable RpcFailure/CAPABILITY_NOT_SUPPORTED, never the legacy reference service.
target::Outcome<std::unique_ptr<v1::IdentityService::Service>>
make_identity_rpc(target::IdentitySecurityApplication& application, PeerAuthenticator authenticate,
                  target::IdentityDelegationApplication* issuer = nullptr,
                  InvocationDeadline invocation_deadline = {});

} // namespace kiarabot::core::platform
