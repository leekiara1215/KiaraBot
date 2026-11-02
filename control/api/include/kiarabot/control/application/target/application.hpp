#pragma once

#include <memory>

#include "kiarabot/control/target/readiness.hpp"
#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::control::target {

// Process-local readiness gate. Control has no authoritative business database; the composition
// root supplies observations for the loaded TLS material (credentials) and the live Identity
// channel (validation/revocation reachability). A null state keeps the historical unavailable
// placeholder for callers that have not composed dependencies.
std::unique_ptr<core::target::ServiceReadinessPort>
make_application(std::shared_ptr<ControlReadiness> readiness = {});

} // namespace kiarabot::control::target
