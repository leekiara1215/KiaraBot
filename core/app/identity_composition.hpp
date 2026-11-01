#pragma once

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace kiarabot::core::composition {

// Exclusive service roots and immutable trusted materials only. Missing security keeps the
// explicitly unavailable P03 application; configured security never falls back after failure.
// Crypto/key/clock preparation finishes before acquiring the one private PG writer session.
target::Outcome<platform::ServiceComposition>
compose_identity_service(const platform::ProcessConfig& config);

} // namespace kiarabot::core::composition
