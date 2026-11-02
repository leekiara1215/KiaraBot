#pragma once

#include <memory>

#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/ports/target/runtime_secrets.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// P13c real Host participant: writes a durable StopExecution command to the Runtime-owned
// host_command outbox and only reports Done after the HostBridge receipt proves a known terminal
// process outcome. Enqueue is idempotent by the deterministic command identity; Pending/Dispatched/
// Unknown/rejected receipts stay Blocked/UNKNOWN rather than pretending the process stopped.
Outcome<std::unique_ptr<HostLifecycleParticipantPort>>
make_runtime_host_lifecycle_participant(RuntimeUnitOfWorkFactory& transactions,
                                        RuntimeSecretPort& secrets);

} // namespace kiarabot::core::target
