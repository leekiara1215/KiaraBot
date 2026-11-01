#pragma once

#include <memory>

#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::target {

std::unique_ptr<ServiceReadinessPort> make_runtime_application();

} // namespace kiarabot::core::target
