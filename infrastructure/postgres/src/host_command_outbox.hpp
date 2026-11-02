#pragma once

#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/ports/target/host_command_ports.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// PG implementation of the P08b-3 Runtime Host command outbox/observation inbox. Borrows the
// owning Runtime UoW: every method participates in that transaction and never commits.
class HostCommandOutbox final : public core::target::HostCommandOutbox {
public:
  HostCommandOutbox(SqlWork& sql) : sql_(sql) {}

  core::target::Outcome<core::target::PersistedHostCommand>
  insert(const core::target::HostCommandOutboxRecord& record) override;
  core::target::Outcome<std::vector<core::target::HostCommandOutboxRecord>>
  pending(const std::string& host_id, std::uint64_t connection_generation,
          std::size_t limit) override;
  core::target::Outcome<std::optional<core::target::HostCommandOutboxRecord>>
  find(const std::string& command_id) override;
  core::target::Status mark_dispatched(const std::string& command_id,
                                       std::uint64_t connection_generation,
                                       std::string_view runtime_epoch,
                                       core::target::TimePoint now) override;
  core::target::Status record_receipt(const std::string& command_id, bool persisted,
                                      std::optional<core::target::ExecutionOutcome> outcome,
                                      std::optional<std::string> process_marker,
                                      core::target::TimePoint now) override;
  core::target::Status mark_unknown(const std::string& command_id,
                                    core::target::TimePoint now) override;
  core::target::Outcome<bool>
  record_observation(const core::target::HostObservationRecord& observation) override;
  core::target::Outcome<std::vector<core::target::HostObservationRecord>>
  observations(const std::string& host_id, std::uint64_t connection_generation,
               std::size_t limit) override;

private:
  SqlWork& sql_;
};

} // namespace kiarabot::infrastructure::postgres::detail
