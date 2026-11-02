#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/host_records.hpp"

namespace kiarabot::core::target {

// Host-local time source (seconds/ms from system UTC). Injectable for lease/retention tests.
class HostClock {
public:
  virtual ~HostClock() = default;
  virtual TimePoint now() const = 0;
};

// Durable Host SQLite journal: command receipts, managed executions and pending observations.
// Every write is one local transaction; the journal never decides business authorization.
class HostJournal {
public:
  virtual ~HostJournal() = default;
  virtual Status open() = 0;
  virtual void close() = 0;
  // Persist a command before any process effect. Same command_id+hash returns the stored record
  // with duplicate=true; a different hash under the same command_id is CONFLICT.
  virtual Outcome<PersistedCommand> persist_command(const HostCommandRecord& record) = 0;
  virtual Status update_command(const HostCommandRecord& record) = 0;
  // Commands whose receipt was persisted but whose execution is not yet resolved. Used by crash
  // recovery to report needs-attention without replaying spawn for the same generation.
  virtual Outcome<std::vector<HostCommandRecord>> unfinished_commands() = 0;
  virtual Outcome<std::optional<ManagedExecution>> find_execution(
      const InstanceId& instance, ExecutionGeneration generation) = 0;
  // Every execution that is not yet KnownSucceeded/KnownFailed; recovery must resolve these.
  virtual Outcome<std::vector<ManagedExecution>> active_executions() = 0;
  virtual Status save_execution(const ManagedExecution& execution) = 0;
  // Assign the next Host-local observation sequence and append it durably.
  virtual Outcome<ExecutionObservation> append_observation(ExecutionObservation observation) = 0;
  virtual Outcome<std::vector<ExecutionObservation>> pending_observations(std::size_t limit) = 0;
  virtual Status mark_observation_reported(std::uint64_t sequence) = 0;
  virtual Outcome<std::uint64_t> last_reported_sequence() = 0;
  virtual Outcome<std::size_t> prune(TimePoint now, std::size_t limit) = 0;
};

// POSIX process ownership for the Host executable composition root. Borrows no environment;
// stdio is detached; a new process group is created for bounded TERM/KILL teardown.
class ProcessSupervisor {
public:
  struct SpawnResult {
    std::int64_t pid = 0;
    std::string marker;
  };
  virtual ~ProcessSupervisor() = default;
  virtual Outcome<SpawnResult> spawn(const ManagedProcessSpec& process) = 0;
  // True only when the pid currently refers to the recorded process (marker match).
  virtual bool alive(std::int64_t pid, std::string_view marker) = 0;
  // TERM (grace) then KILL. true = KILL was required. Outcome failure = stop could not be
  // confirmed (Runtime must keep process_outcome UNKNOWN, never "stopped").
  virtual Outcome<bool> terminate(std::int64_t pid, std::string_view marker,
                                  std::chrono::milliseconds grace) = 0;
};

// Inbound commands arrive only on the authenticated HostBridge stream; the Host never listens for
// HostService unary calls from Runtime.
class HostBridgeHandler {
public:
  virtual ~HostBridgeHandler() = default;
  virtual Outcome<HostReceipt> apply_desired_process(const HostProcessSpec& process,
                                                    std::string_view command_id,
                                                    std::string_view payload_sha256) = 0;
  virtual Outcome<HostReceipt> stop_execution(std::string_view instance_id,
                                              std::uint64_t execution_generation,
                                              std::uint32_t grace_ms,
                                              std::string_view command_id,
                                              std::string_view payload_sha256) = 0;
  virtual Outcome<HostReceipt> drain_host(std::uint32_t grace_ms, std::string_view command_id,
                                          std::string_view payload_sha256) = 0;
  // Called by the transport after registration and after a Runtime resync request so pending
  // observations can be replayed. Must not block indefinitely.
  virtual void bridge_ready() = 0;
  virtual void bridge_resync() = 0;
};

// Outbound HostBridge transport. Registration/heartbeat/observation/receipt frames all carry the
// current connection fence assigned in HostRegisterHostResponse; the client owns the 5s heartbeat.
class HostBridgeTransport {
public:
  virtual ~HostBridgeTransport() = default;
  virtual Status start(HostBridgeHandler& handler) = 0;
  virtual void stop() = 0;
  // True only after the Runtime accepted RegisterHost and returned the connection fence.
  virtual bool registered() const = 0;
  virtual Status send_observation(const ExecutionObservation& observation) = 0;
  virtual Status send_receipt(const HostReceipt& receipt) = 0;
  // Highest delivered observation sequence, reported in heartbeat cursors.
  virtual std::uint64_t delivered_sequence() const = 0;
};
} // namespace kiarabot::core::target
