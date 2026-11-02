#pragma once

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace kiarabot::core::composition {

// P10a Scheduler composition root: opens the Scheduler private PostgreSQL database, assembles the
// task/trigger application with the production IANA resolver and CSPRNG ids, registers the
// SchedulerService unary adapter, and runs the bounded due/expiry/prune maintenance sweep.
// The Identity delegation/current-authorization chain is not wired for Scheduler yet: the
// composition supplies a fail-closed authorizer, so readiness reports false and no command is
// authorized. A later integration replaces it with the real Identity client; fixtures use fakes.
target::Outcome<platform::ServiceComposition>
compose_scheduler_service(const platform::ProcessConfig& config);

} // namespace kiarabot::core::composition
