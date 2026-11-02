#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/host_command_records.hpp"

namespace kiarabot::core::target {

// P08b-3 Runtime outbox. All writes participate in the caller's Runtime UoW (no commit, no
// network). `insert` is idempotent by command_id: same payload returns duplicate=true, a
// different payload under the same command_id is CONFLICT. Pending rows are replayed after a
// Host reconnect with the exact current connection generation.
class HostCommandOutbox {
public:
  virtual ~HostCommandOutbox() = default;
  virtual Outcome<PersistedHostCommand> insert(const HostCommandOutboxRecord& record) = 0;
  virtual Outcome<std::vector<HostCommandOutboxRecord>>
  pending(const std::string& host_id, std::uint64_t connection_generation, std::size_t limit) = 0;
  virtual Outcome<std::optional<HostCommandOutboxRecord>> find(const std::string& command_id) = 0;
  virtual Status mark_dispatched(const std::string& command_id, std::uint64_t connection_generation,
                                 std::string_view runtime_epoch, TimePoint now) = 0;
  virtual Status record_receipt(const std::string& command_id, bool persisted,
                                std::optional<ExecutionOutcome> outcome,
                                std::optional<std::string> process_marker, TimePoint now) = 0;
  virtual Status mark_unknown(const std::string& command_id, TimePoint now) = 0;
  // Observation dedup key is (host_id, connection_generation, sequence); duplicate returns
  // duplicate=true without mutating stored facts.
  virtual Outcome<bool> record_observation(const HostObservationRecord& observation) = 0;
  virtual Outcome<std::vector<HostObservationRecord>>
  observations(const std::string& host_id, std::uint64_t connection_generation,
               std::size_t limit) = 0;
  // Maintenance: dispatched commands older than the timeout become Unknown (never "stopped");
  // receipted rows are pruned after the resolved retention and Unknown rows after the longer
  // unresolved retention. Bounded batch, no cross-service cascade.
  virtual Outcome<std::size_t> expire_dispatched(TimePoint now, std::chrono::seconds timeout) = 0;
  virtual Outcome<std::size_t> prune(TimePoint now, std::chrono::seconds resolved_retention,
                                     std::chrono::seconds unresolved_retention,
                                     std::size_t limit) = 0;
};

} // namespace kiarabot::core::target
