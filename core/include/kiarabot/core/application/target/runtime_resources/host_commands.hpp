#pragma once

#include "kiarabot/core/application/target/runtime_resources/types.hpp"

namespace kiarabot::core::target {

// Narrow Runtime resource use cases. Each call owns its local UoW; borrowed ports outlive calls.
class RuntimeHostCommandApplication {
public:
  virtual ~RuntimeHostCommandApplication() = default;

  virtual Outcome<std::vector<HostCommandOutboxRecord>>
  host_pending_commands(const std::string& host_id, std::uint64_t connection_generation,
                        std::size_t limit) = 0;
  virtual Status host_command_dispatched(const std::string& command_id,
                                         std::uint64_t connection_generation,
                                         std::string_view runtime_epoch) = 0;
  virtual Status host_command_receipt(const std::string& command_id,
                                      std::uint64_t connection_generation, bool persisted,
                                      std::optional<ExecutionOutcome> outcome,
                                      std::optional<std::string> process_marker) = 0;
  virtual Outcome<bool> host_observation(const HostObservationRecord& observation) = 0;
  virtual Outcome<HostCommandMaintenance> host_command_maintenance() = 0;
};

} // namespace kiarabot::core::target
