#pragma once

#include <memory>

#include "kiarabot/core/ports/target/host_ports.hpp"

namespace kiarabot::core::target {

std::unique_ptr<HostClock> make_system_host_clock();

} // namespace kiarabot::core::target
