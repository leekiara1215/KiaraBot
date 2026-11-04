#pragma once

#include <memory>
#include <string>

#include "kiarabot/core/ports/target/host_maintenance.hpp"
#include "kiarabot/core/ports/target/host_ports.hpp"

namespace kiarabot::infrastructure::host {
struct HostMaintenanceStorageLimits {
  std::size_t change_capacity = core::target::kOperationObservationCapacity;
  std::size_t change_bytes = core::target::kOperationObservationBytes;
};
// Fault harness observes a completed local commit before the caller sees its response. Null in
// production. Never called with a transaction/connection lock held; no secret is passed.
using HostMaintenanceCommitCheckpoint = void (*)();
core::target::Outcome<std::unique_ptr<core::target::HostMaintenanceStore>>
make_sqlite_host_maintenance(std::string path, core::target::ResourceId host,
                             std::string initial_dataset_epoch, core::target::HostClock& clock,
                             HostMaintenanceStorageLimits limits = {},
                             HostMaintenanceCommitCheckpoint checkpoint_for_testing = nullptr);
} // namespace kiarabot::infrastructure::host
