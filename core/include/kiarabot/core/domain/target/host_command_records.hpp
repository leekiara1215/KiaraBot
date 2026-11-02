#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/host_records.hpp"

namespace kiarabot::core::target {

// Runtime-side durable Host command outbox. Runtime owns desired state and writes the exact
// command snapshot in the same UoW as the instance mutation; the HostBridge transport may replay
// the row after reconnect. A receipted command never fabricates a process stop: absence of a
// receipt keeps the projection UNKNOWN.
enum class HostCommandDeliveryState { Pending = 0, Dispatched = 1, Receipted = 2, Unknown = 3 };

struct HostCommandOutboxRecord {
  std::string command_id;
  std::string host_id;
  std::string instance_id;
  HostCommandKind kind = HostCommandKind::ApplyDesiredProcess;
  std::uint64_t execution_generation = 0;
  std::uint64_t desired_version = 0;
  DesiredState desired_state = DesiredState::Running;
  std::string package_sha256;
  std::string launch_profile;
  std::vector<std::string> arguments;
  std::vector<std::string> environment_names;
  // Canonical hash of the snapshot fields above; a replay with the same command_id must match.
  std::string payload_sha256;
  HostCommandDeliveryState state = HostCommandDeliveryState::Pending;
  std::uint64_t connection_generation = 0;
  std::string runtime_epoch;
  TimePoint created_at;
  TimePoint updated_at;
  TimePoint dispatched_at;
  TimePoint receipted_at;
  std::optional<bool> receipt_persisted;
  std::optional<ExecutionOutcome> receipt_outcome;
  std::optional<std::string> receipt_process_marker;
};

// One durable Host observation, deduplicated by (host_id, connection_generation, sequence).
// Runtime stores the projection only; the Host remains the owner of process reality.
struct HostObservationRecord {
  std::string host_id;
  std::uint64_t connection_generation = 0;
  std::uint64_t sequence = 0;
  std::string instance_id;
  std::uint64_t execution_generation = 0;
  ExecutionOutcome outcome = ExecutionOutcome::NotStarted;
  std::string process_marker;
  std::string command_id;
  TimePoint observed_at;
};

struct PersistedHostCommand {
  HostCommandOutboxRecord record;
  bool duplicate = false;
};

} // namespace kiarabot::core::target
