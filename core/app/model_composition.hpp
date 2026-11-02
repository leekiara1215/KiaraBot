#pragma once

#include <memory>

#include "kiarabot/core/ports/target/model_ports.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace kiarabot::core::composition {
// Explicit test-only injection. Production never sets it and gets the reviewed libcurl HTTP
// provider; an injected fake is never selected by configuration or environment.
struct ModelCompositionOptions {
  std::unique_ptr<target::ModelProviderPort> provider_override;
};
// P11b Model composition root. Owns the typed PostgreSQL factory, deployment-injected root key,
// secret/digest adapters and the reviewed HTTP provider; authoritative Identity/Control caller
// verification is P11c and therefore remains fail closed here (readiness stays false).
target::Outcome<platform::ServiceComposition>
compose_model_service(const platform::ProcessConfig& config, ModelCompositionOptions options = {});
} // namespace kiarabot::core::composition
