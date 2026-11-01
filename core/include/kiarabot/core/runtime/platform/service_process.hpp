#pragma once

#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::platform {

// Borrows the port until return, including shutdown of all sockets/threads/RPCs.
// --help: 0 without listening. --config PATH required otherwise. CLI/config errors: 2;
// certificate/bind/runtime failure: 1; synchronous SIGINT/SIGTERM shutdown: 0.
// Configuration never overrides a false readiness result. No business state is created.
int run_service(const target::ServiceReadinessPort& application, int argc, char* argv[]);

} // namespace kiarabot::core::platform
