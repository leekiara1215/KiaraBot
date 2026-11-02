#pragma once

#include <memory>
#include <string>

#include "kiarabot/core/ports/target/host_ports.hpp"

namespace kiarabot::infrastructure::host {

// SQLite implementation of the Host-local three-table journal. Only the Host process uses it;
// it never decides authorization and never stores credentials or environment values.
std::unique_ptr<core::target::HostJournal> make_sqlite_host_journal(std::string path);

} // namespace kiarabot::infrastructure::host
