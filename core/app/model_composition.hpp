#pragma once

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace kiarabot::core::composition {
// P11a Model composition root. Owns the typed PostgreSQL factory, deployment-injected root key,
// secret/digest adapters and a development fake provider; authoritative Identity/Control caller
// verification and the reviewed HTTP provider are P11c/P11b and therefore fail closed here.
target::Outcome<platform::ServiceComposition>
compose_model_service(const platform::ProcessConfig& config);
} // namespace kiarabot::core::composition
