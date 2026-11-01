#pragma once

#include <memory>

#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::control::target {

// A process-local application gate, with no authoritative business storage or transport objects.
// P02 is deliberately unavailable until Identity and owner RPC use cases are implemented.
std::unique_ptr<core::target::ServiceReadinessPort> make_application();

} // namespace kiarabot::control::target
