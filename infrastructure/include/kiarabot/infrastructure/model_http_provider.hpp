#pragma once

#include <memory>

#include "kiarabot/core/ports/target/model_ports.hpp"

namespace kiarabot::infrastructure {
// Reviewed P11b Model provider: libcurl HTTP Chat Completions adapter implementing the target
// ModelProviderPort. It is stateless per call (endpoint/model/timeout come from the pinned Model
// configuration, the secret is borrowed only for this call), never retries a possibly-billed call
// and never returns provider text/details as a failure. TLS verification and system trust roots
// stay enabled; environment proxies and redirects are disabled.
core::target::Outcome<std::unique_ptr<core::target::ModelProviderPort>> make_model_http_provider();
} // namespace kiarabot::infrastructure
