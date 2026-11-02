#pragma once

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace kiarabot::core::composition {

// P05 Runtime composition root. Opens the Runtime private PostgreSQL logical database through the
// typed factory, assembles the resource application, and registers the production unary RPC
// adapter. The Identity current-authorization/peer chain is not wired in this increment: the
// composition supplies a fail-closed authorizer and a fail-closed RuntimeAuthorizationPort, so
// readiness reports false and no request is authorized. A later P04-integration commit replaces
// both with the real Identity client; test fakes never make the process ready.
target::Outcome<platform::ServiceComposition>
compose_runtime_service(const platform::ProcessConfig& config);

} // namespace kiarabot::core::composition
