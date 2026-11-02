#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// P08 Host-local durable facts. The Host owns process reality (L2); Runtime owns desired
// state/registration/config-application facts and only receives observations over HostBridge.
enum class HostCommandKind { ApplyDesiredProcess = 1, StopExecution = 2, DrainHost = 3 };
enum class HostCommandState { Persisted = 0, Executing = 1, Completed = 2, Failed = 3, Unknown = 4 };
enum class ManagedExecutionState { Starting = 0, Running = 1, Stopping = 2, Exited = 3, Unknown = 4 };

// Immutable deployment allowlist entry for one plugin package. `root` is a trusted deployment
// directory; `sha256` is the deployment-computed package tree digest.
struct HostPackage {
  std::string plugin_type_id;
  std::string root;
  std::string sha256;
  std::string launch_profile;
  std::vector<std::string> allowed_argument_names;
};

// Fixed interpreter/argv and environment allowlist for one launch profile. Only names are
// forwarded from Runtime; values come from deployment files, never from the wire.
struct LaunchProfile {
  std::string name;
  std::string executable;
  std::vector<std::string> fixed_arguments;
  std::vector<std::string> allowed_environment_names;
  std::string credential_environment;
};

// Stable Host identity projection sent in RegisterHost.
struct HostIdentity {
  std::string host_id;
  std::vector<HostPackage> packages;
  std::vector<std::string> capabilities;
};

// One apply command target. Arguments/environment names are the frozen `name=value`/allowlist
// forms; values are resolved by the Host from deployment configuration only.
struct HostProcessSpec {
  std::string instance_id;
  std::string installation_id;
  std::uint64_t execution_generation = 0;
  std::uint64_t desired_version = 0;
  DesiredState desired_state = DesiredState::Running;
  std::string package_sha256;
  std::string launch_profile;
  std::vector<std::string> arguments;
  std::vector<std::string> environment_names;
};

// Fully resolved process launch (outer composition only; never persisted).
struct ManagedProcessSpec {
  std::string executable;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
};

// One real process execution. `process_marker` binds pid + process start time so a reused pid is
// never mistaken for the same execution.
struct ManagedExecution {
  std::string host_id;
  std::string instance_id;
  std::string installation_id;
  std::uint64_t execution_generation = 0;
  std::uint64_t desired_version = 0;
  std::int64_t pid = 0;
  std::string process_marker;
  ManagedExecutionState state = ManagedExecutionState::Starting;
  ExecutionOutcome outcome = ExecutionOutcome::NotStarted;
  std::optional<int> exit_code;
  std::optional<int> exit_signal;
  TimePoint started_at;
  TimePoint exited_at;
  TimePoint observed_at;
  std::string command_id;
};

// Durable pending observation. `sequence` is Host-local monotonic and identifies the observation
// on one connection; Runtime deduplicates with host_id+connection_generation+sequence.
struct ExecutionObservation {
  std::uint64_t sequence = 0;
  std::string observation_id;
  ManagedExecution execution;
  std::string command_id;
  bool reported = false;
};

// Durable command receipt: persisted before any spawn/stop side effect and replayed on a repeated
// command_id with the same payload hash.
struct HostCommandRecord {
  std::string command_id;
  std::string payload_sha256;
  HostCommandKind kind = HostCommandKind::ApplyDesiredProcess;
  HostCommandState state = HostCommandState::Persisted;
  std::string instance_id;
  std::uint64_t execution_generation = 0;
  std::string receipt;
  std::optional<ManagedExecution> execution;
  TimePoint created_at;
  TimePoint updated_at;
  TimePoint retain_until;
};
struct PersistedCommand {
  HostCommandRecord record;
  bool duplicate = false;
};

// Durable command receipt returned to Runtime. `persisted` means the command/effect intent is
// durably accepted, never that the process started or the effect completed.
struct HostReceipt {
  std::string command_id;
  bool persisted = false;
  std::optional<ManagedExecution> execution;
};

// Host-local retention policy defaults from the frozen design.
inline constexpr auto kHostResolvedRetention = std::chrono::hours{24 * 7};
inline constexpr auto kHostUnresolvedRetention = std::chrono::hours{24 * 30};

} // namespace kiarabot::core::target
