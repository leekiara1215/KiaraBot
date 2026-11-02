#pragma once

#include "kiarabot/core/ports/target/host_ports.hpp"

namespace kiarabot::core::target {

// POSIX-only supervisor. The composition root owns the returned object for the whole process.
std::unique_ptr<ProcessSupervisor> make_posix_process_supervisor();

} // namespace kiarabot::core::target
