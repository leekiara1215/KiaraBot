#include "kiarabot/core/application/target/host_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace kiarabot::core::target {
namespace {

Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
Failure invalid() {
  return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure conflict() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}
Failure unavailable() {
  return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}

bool canonical_id(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
bool digest(std::string_view value) {
  if (value.size() != 64) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
  });
}
bool command_id(std::string_view value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return character > 32 && character < 127;
  });
}
bool token(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           character == '.' || character == ':';
  });
}
bool argument_value(std::string_view value) {
  if (value.size() > 1024) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return character >= 32 && character < 127;
  });
}
bool environment_value(std::string_view value) {
  if (value.empty() || value.size() > 4096) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return character != '\0' && character != '\n' && character != '\r';
  });
}

TimePoint now_of(const HostClock& clock) {
  return clock.now();
}
TimePoint terminal_retention(TimePoint now) {
  return now + kHostResolvedRetention;
}
TimePoint unresolved_retention(TimePoint now) {
  return now + kHostUnresolvedRetention;
}
bool terminal_state(ManagedExecutionState state) {
  return state == ManagedExecutionState::Exited;
}

} // namespace

HostService::HostService(HostServiceDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

HostService::~HostService() {
  stop();
}

Service HostService::service() const noexcept {
  return Service::Host;
}

ServiceReadiness HostService::readiness() const {
  std::lock_guard lock(mutex_);
  const bool registered = dependencies_.bridge.registered();
  return {journal_open_ && registered,
          {{Dependency::BusinessImplementation,
            started_ ? DependencyState::Available : DependencyState::NotImplemented},
           {Dependency::DatabaseSchema,
            journal_open_ ? DependencyState::Available : DependencyState::Unavailable},
           {Dependency::Runtime,
            registered ? DependencyState::Available : DependencyState::Unavailable}}};
}

Status HostService::start() {
  std::lock_guard lock(mutex_);
  if (started_) {
    return std::monostate{};
  }
  const auto opened = dependencies_.journal.open();
  if (const auto* failed = std::get_if<Failure>(&opened)) {
    return *failed;
  }
  journal_open_ = true;
  recover_locked();
  const auto connected = dependencies_.bridge.start(*this);
  if (const auto* failed = std::get_if<Failure>(&connected)) {
    dependencies_.journal.close();
    journal_open_ = false;
    return *failed;
  }
  started_ = true;
  return std::monostate{};
}

void HostService::stop() {
  std::lock_guard lock(mutex_);
  if (!started_ || stopped_) {
    return;
  }
  stopped_ = true;
  // Stop managed processes first so their final observations can still be delivered.
  const auto active = dependencies_.journal.active_executions();
  if (const auto* executions = std::get_if<std::vector<ManagedExecution>>(&active)) {
    for (const auto& execution : *executions) {
      (void)stop_instance_locked(execution.instance_id, execution.execution_generation,
                                 dependencies_.stop_grace, execution.command_id);
    }
  }
  flush_pending_locked();
  dependencies_.bridge.stop();
  if (journal_open_) {
    dependencies_.journal.close();
    journal_open_ = false;
  }
  started_ = false;
}

const HostPackage* HostService::package_for(std::string_view sha256) const {
  for (const auto& package : dependencies_.identity.packages) {
    if (package.sha256 == sha256) {
      return &package;
    }
  }
  return nullptr;
}

const LaunchProfile* HostService::profile_for(std::string_view name) const {
  for (const auto& profile : dependencies_.profiles) {
    if (profile.name == name) {
      return &profile;
    }
  }
  return nullptr;
}

std::string HostService::deployment_environment(std::string_view name) const {
  for (const auto& [key, value] : dependencies_.environment) {
    if (key == name) {
      return value;
    }
  }
  return {};
}

Outcome<std::string> HostService::credential_for(std::string_view instance_id) const {
  const std::string path = dependencies_.credential_directory + "/" + std::string(instance_id) +
                           ".bootstrap";
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  struct stat information {};
  std::string value;
  const bool valid = ::fstat(descriptor, &information) == 0 && S_ISREG(information.st_mode) &&
                     (information.st_mode & 0777) == 0600 &&
                     information.st_uid == ::geteuid() && information.st_size >= 32 &&
                     information.st_size <= 4096;
  if (valid) {
    value.resize(static_cast<std::size_t>(information.st_size));
    std::size_t offset = 0;
    while (offset < value.size()) {
      const auto read = ::read(descriptor, value.data() + offset, value.size() - offset);
      if (read <= 0) {
        break;
      }
      offset += static_cast<std::size_t>(read);
    }
    value.resize(offset);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
      value.pop_back();
    }
  }
  ::close(descriptor);
  if (!valid || !environment_value(value)) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return value;
}

Outcome<HostService::ResolvedProcess> HostService::resolve_locked(
    const HostProcessSpec& process) {
  if (!canonical_id(process.instance_id) || !canonical_id(process.installation_id) ||
      process.execution_generation == 0 || process.desired_version == 0 ||
      !digest(process.package_sha256) || !token(process.launch_profile, 128) ||
      process.arguments.size() > 64 || process.environment_names.size() > 64) {
    return invalid();
  }
  const auto* package = package_for(process.package_sha256);
  if (package == nullptr || package->launch_profile != process.launch_profile) {
    return failure(ErrorCode::CapabilityNotSupported);
  }
  const auto* profile = profile_for(process.launch_profile);
  if (profile == nullptr) {
    return failure(ErrorCode::CapabilityNotSupported);
  }
  std::vector<std::string> argument_names;
  for (const auto& argument : process.arguments) {
    const auto separator = argument.find('=');
    if (separator == std::string::npos || separator == 0 ||
        !argument_value(argument)) {
      return invalid();
    }
    const auto name = argument.substr(0, separator);
    if (!token(name, 128) ||
        std::find(package->allowed_argument_names.begin(), package->allowed_argument_names.end(),
                  name) == package->allowed_argument_names.end() ||
        std::find(argument_names.begin(), argument_names.end(), name) != argument_names.end()) {
      return failure(ErrorCode::CapabilityNotSupported);
    }
    argument_names.push_back(name);
  }
  std::vector<std::string> environment;
  environment.push_back("PATH=/usr/bin:/bin");
  std::vector<std::string> seen;
  for (const auto& name : process.environment_names) {
    if (!token(name, 128) ||
        std::find(profile->allowed_environment_names.begin(),
                  profile->allowed_environment_names.end(),
                  name) == profile->allowed_environment_names.end() ||
        std::find(seen.begin(), seen.end(), name) != seen.end()) {
      return failure(ErrorCode::CapabilityNotSupported);
    }
    seen.push_back(name);
    if (!profile->credential_environment.empty() && name == profile->credential_environment) {
      const auto credential = credential_for(process.instance_id);
      if (const auto* failed = std::get_if<Failure>(&credential)) {
        return *failed;
      }
      environment.push_back(name + "=" + std::get<std::string>(credential));
      continue;
    }
    const auto value = deployment_environment(name);
    if (!environment_value(value)) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    environment.push_back(name + "=" + value);
  }
  ManagedProcessSpec launch;
  launch.executable = profile->executable;
  launch.arguments = profile->fixed_arguments;
  launch.arguments.insert(launch.arguments.end(), process.arguments.begin(),
                          process.arguments.end());
  launch.environment = std::move(environment);
  return ResolvedProcess{std::move(launch), package, profile};
}

void HostService::mark_command_locked(HostCommandRecord& command, HostCommandState state,
                                      std::optional<ManagedExecution> execution) {
  command.state = state;
  command.execution = std::move(execution);
  command.updated_at = now_of(dependencies_.clock);
  command.retain_until = state == HostCommandState::Completed || state == HostCommandState::Failed
                             ? terminal_retention(command.updated_at)
                             : unresolved_retention(command.updated_at);
  (void)dependencies_.journal.update_command(command);
}

HostReceipt HostService::receipt_locked(const HostCommandRecord& command) const {
  return HostReceipt{command.command_id, true, command.execution};
}

void HostService::publish_locked(const ManagedExecution& execution, std::string_view command_id) {
  ExecutionObservation observation;
  observation.execution = execution;
  observation.command_id = std::string(command_id);
  const auto stored = dependencies_.journal.append_observation(observation);
  if (const auto* value = std::get_if<ExecutionObservation>(&stored)) {
    if (dependencies_.bridge.registered() &&
        std::holds_alternative<std::monostate>(
            dependencies_.bridge.send_observation(*value))) {
      (void)dependencies_.journal.mark_observation_reported(value->sequence);
    }
  }
}

void HostService::flush_pending_locked() {
  if (!dependencies_.bridge.registered()) {
    return;
  }
  const auto batch = std::clamp<std::size_t>(dependencies_.observation_batch, 1, 256);
  for (;;) {
    const auto pending = dependencies_.journal.pending_observations(batch);
    const auto* observations = std::get_if<std::vector<ExecutionObservation>>(&pending);
    if (observations == nullptr || observations->empty()) {
      return;
    }
    for (const auto& observation : *observations) {
      if (!std::holds_alternative<std::monostate>(
              dependencies_.bridge.send_observation(observation))) {
        return;
      }
      (void)dependencies_.journal.mark_observation_reported(observation.sequence);
    }
    if (observations->size() < batch) {
      return;
    }
  }
}

void HostService::bridge_ready() {
  std::lock_guard lock(mutex_);
  flush_pending_locked();
}

void HostService::bridge_resync() {
  std::lock_guard lock(mutex_);
  flush_pending_locked();
}

void HostService::recover_locked() {
  const auto active = dependencies_.journal.active_executions();
  if (const auto* executions = std::get_if<std::vector<ManagedExecution>>(&active)) {
    for (const auto& stored : *executions) {
      ManagedExecution execution = stored;
      execution.observed_at = now_of(dependencies_.clock);
      if (!terminal_state(execution.state) &&
          dependencies_.supervisor.alive(execution.pid, execution.process_marker)) {
        // Adopt a still running process after Host restart; never spawn a second one.
        execution.state = ManagedExecutionState::Running;
        execution.outcome = ExecutionOutcome::NotStarted;
        (void)dependencies_.journal.save_execution(execution);
        publish_locked(execution, execution.command_id);
        continue;
      }
      // Process is gone without a confirmed exit: report UNKNOWN, never FAILED/stopped.
      execution.state = ManagedExecutionState::Exited;
      execution.outcome = ExecutionOutcome::Unknown;
      execution.exited_at = execution.observed_at;
      (void)dependencies_.journal.save_execution(execution);
      publish_locked(execution, execution.command_id);
    }
  }
  const auto unfinished = dependencies_.journal.unfinished_commands();
  if (const auto* commands = std::get_if<std::vector<HostCommandRecord>>(&unfinished)) {
    for (auto command : *commands) {
      mark_command_locked(command, HostCommandState::Unknown, command.execution);
    }
  }
}

Outcome<HostReceipt> HostService::apply_desired_process(const HostProcessSpec& process,
                                                        std::string_view command,
                                                        std::string_view payload_sha256) {
  if (!command_id(command) || !digest(payload_sha256)) {
    return invalid();
  }
  std::lock_guard lock(mutex_);
  if (!journal_open_) {
    return unavailable();
  }
  if (process.desired_state != DesiredState::Running &&
      process.desired_state != DesiredState::Stopped) {
    return invalid();
  }
  std::optional<ResolvedProcess> resolved;
  if (process.desired_state == DesiredState::Running) {
    auto candidate = resolve_locked(process);
    if (const auto* failed = std::get_if<Failure>(&candidate)) {
      return *failed;
    }
    resolved = std::get<ResolvedProcess>(std::move(candidate));
  } else if (!canonical_id(process.instance_id) || !canonical_id(process.installation_id) ||
             process.execution_generation == 0 || process.desired_version == 0) {
    return invalid();
  }
  const auto now = now_of(dependencies_.clock);
  HostCommandRecord record;
  record.command_id = std::string(command);
  record.payload_sha256 = std::string(payload_sha256);
  record.kind = HostCommandKind::ApplyDesiredProcess;
  record.state = HostCommandState::Persisted;
  record.instance_id = process.instance_id;
  record.execution_generation = process.execution_generation;
  record.created_at = now;
  record.updated_at = now;
  record.retain_until = unresolved_retention(now);
  auto persisted = dependencies_.journal.persist_command(record);
  if (const auto* failed = std::get_if<Failure>(&persisted)) {
    return *failed;
  }
  auto stored = std::get<PersistedCommand>(std::move(persisted));
  if (stored.duplicate) {
    return receipt_locked(stored.record);
  }
  if (process.desired_state == DesiredState::Running) {
    return apply_running_locked(stored.record, process, *resolved);
  }
  return apply_stopped_locked(stored.record, process);
}

Outcome<HostReceipt> HostService::apply_running_locked(const HostCommandRecord& command,
                                                       const HostProcessSpec& process,
                                                       const ResolvedProcess& resolved) {
  const auto found = dependencies_.journal.find_execution(
      InstanceId{process.instance_id}, ExecutionGeneration{process.execution_generation});
  if (const auto* failed = std::get_if<Failure>(&found)) {
    return *failed;
  }
  if (const auto& existing = std::get<std::optional<ManagedExecution>>(found)) {
    if (!terminal_state(existing->state) &&
        dependencies_.supervisor.alive(existing->pid, existing->process_marker)) {
      auto updated = command;
      mark_command_locked(updated, HostCommandState::Completed, *existing);
      return HostReceipt{command.command_id, true, *existing};
    }
    // A prior execution for this exact generation is terminal or unknown: never respawn it.
    auto reconciled = *existing;
    if (!terminal_state(reconciled.state)) {
      reconciled.state = ManagedExecutionState::Exited;
      reconciled.outcome = ExecutionOutcome::Unknown;
      reconciled.observed_at = now_of(dependencies_.clock);
      reconciled.exited_at = reconciled.observed_at;
      (void)dependencies_.journal.save_execution(reconciled);
      publish_locked(reconciled, command.command_id);
    }
    auto failed_command = command;
    mark_command_locked(failed_command, HostCommandState::Failed, reconciled);
    return conflict();
  }
  // Another generation may still own this instance; a live one blocks a second process.
  const auto active = dependencies_.journal.active_executions();
  if (const auto* executions = std::get_if<std::vector<ManagedExecution>>(&active)) {
    for (const auto& other : *executions) {
      if (other.instance_id != process.instance_id) {
        continue;
      }
      if (!terminal_state(other.state) &&
          dependencies_.supervisor.alive(other.pid, other.process_marker)) {
        return conflict();
      }
      auto reconciled = other;
      reconciled.state = ManagedExecutionState::Exited;
      reconciled.outcome = ExecutionOutcome::Unknown;
      reconciled.observed_at = now_of(dependencies_.clock);
      reconciled.exited_at = reconciled.observed_at;
      (void)dependencies_.journal.save_execution(reconciled);
      publish_locked(reconciled, other.command_id);
    }
  }
  const auto spawned = dependencies_.supervisor.spawn(resolved.spec);
  if (const auto* failed = std::get_if<Failure>(&spawned)) {
    auto failed_command = command;
    mark_command_locked(failed_command, HostCommandState::Failed, std::nullopt);
    return *failed;
  }
  const auto child = std::get<ProcessSupervisor::SpawnResult>(spawned);
  ManagedExecution execution;
  execution.host_id = dependencies_.identity.host_id;
  execution.instance_id = process.instance_id;
  execution.installation_id = process.installation_id;
  execution.execution_generation = process.execution_generation;
  execution.desired_version = process.desired_version;
  execution.pid = child.pid;
  execution.process_marker = child.marker;
  execution.state = ManagedExecutionState::Running;
  execution.outcome = ExecutionOutcome::NotStarted;
  execution.started_at = now_of(dependencies_.clock);
  execution.observed_at = execution.started_at;
  execution.command_id = command.command_id;
  const auto saved = dependencies_.journal.save_execution(execution);
  if (const auto* failed = std::get_if<Failure>(&saved)) {
    return *failed;
  }
  publish_locked(execution, command.command_id);
  auto updated = command;
  mark_command_locked(updated, HostCommandState::Completed, execution);
  return HostReceipt{command.command_id, true, execution};
}

Outcome<HostReceipt> HostService::apply_stopped_locked(const HostCommandRecord& command,
                                                       const HostProcessSpec& process) {
  const auto found = dependencies_.journal.find_execution(
      InstanceId{process.instance_id}, ExecutionGeneration{process.execution_generation});
  if (const auto* failed = std::get_if<Failure>(&found)) {
    return *failed;
  }
  if (!std::get<std::optional<ManagedExecution>>(found)) {
    auto updated = command;
    mark_command_locked(updated, HostCommandState::Completed, std::nullopt);
    return HostReceipt{command.command_id, true, std::nullopt};
  }
  const auto stopped = stop_instance_locked(process.instance_id, process.execution_generation,
                                            dependencies_.stop_grace, command.command_id);
  if (const auto* failed = std::get_if<Failure>(&stopped)) {
    return *failed;
  }
  auto updated = command;
  mark_command_locked(updated, HostCommandState::Completed, std::nullopt);
  return std::get<HostReceipt>(stopped);
}

Outcome<HostReceipt> HostService::stop_instance_locked(std::string_view instance_id,
                                                       std::uint64_t execution_generation,
                                                       std::chrono::milliseconds grace,
                                                       std::string_view command) {
  auto found = dependencies_.journal.find_execution(InstanceId{std::string(instance_id)},
                                                    ExecutionGeneration{execution_generation});
  if (const auto* failed = std::get_if<Failure>(&found)) {
    return *failed;
  }
  if (const auto& current = std::get<std::optional<ManagedExecution>>(found)) {
    if (terminal_state(current->state)) {
      return HostReceipt{std::string(command), true, *current};
    }
    auto execution = *current;
    if (!dependencies_.supervisor.alive(execution.pid, execution.process_marker)) {
      execution.state = ManagedExecutionState::Exited;
      execution.outcome = ExecutionOutcome::Unknown;
      execution.observed_at = now_of(dependencies_.clock);
      execution.exited_at = execution.observed_at;
      (void)dependencies_.journal.save_execution(execution);
      publish_locked(execution, command);
      return HostReceipt{std::string(command), true, execution};
    }
    const auto terminated = dependencies_.supervisor.terminate(
        execution.pid, execution.process_marker, std::max(grace, std::chrono::milliseconds{0}));
    if (const auto* failed = std::get_if<Failure>(&terminated)) {
      execution.state = ManagedExecutionState::Unknown;
      execution.outcome = ExecutionOutcome::Unknown;
      execution.observed_at = now_of(dependencies_.clock);
      (void)dependencies_.journal.save_execution(execution);
      publish_locked(execution, command);
      return *failed;
    }
    execution.state = ManagedExecutionState::Exited;
    execution.outcome = ExecutionOutcome::KnownSucceeded;
    execution.observed_at = now_of(dependencies_.clock);
    execution.exited_at = execution.observed_at;
    (void)dependencies_.journal.save_execution(execution);
    publish_locked(execution, command);
    return HostReceipt{std::string(command), true, execution};
  }
  return HostReceipt{std::string(command), true, std::nullopt};
}

Outcome<HostReceipt> HostService::stop_execution(std::string_view instance_id,
                                                  std::uint64_t execution_generation,
                                                  std::uint32_t grace_ms,
                                                  std::string_view command,
                                                  std::string_view payload_sha256) {
  if (!canonical_id(instance_id) || execution_generation == 0 || !command_id(command) ||
      !digest(payload_sha256)) {
    return invalid();
  }
  std::lock_guard lock(mutex_);
  if (!journal_open_) {
    return unavailable();
  }
  const auto now = now_of(dependencies_.clock);
  HostCommandRecord record;
  record.command_id = std::string(command);
  record.payload_sha256 = std::string(payload_sha256);
  record.kind = HostCommandKind::StopExecution;
  record.state = HostCommandState::Persisted;
  record.instance_id = std::string(instance_id);
  record.execution_generation = execution_generation;
  record.created_at = now;
  record.updated_at = now;
  record.retain_until = unresolved_retention(now);
  auto persisted = dependencies_.journal.persist_command(record);
  if (const auto* failed = std::get_if<Failure>(&persisted)) {
    return *failed;
  }
  auto stored = std::get<PersistedCommand>(std::move(persisted));
  if (stored.duplicate) {
    return receipt_locked(stored.record);
  }
  const auto grace = std::chrono::milliseconds{
      std::min<std::uint32_t>(grace_ms == 0 ? 10'000 : grace_ms, 10'000)};
  const auto stopped =
      stop_instance_locked(instance_id, execution_generation, grace, command);
  if (const auto* failed = std::get_if<Failure>(&stopped)) {
    auto failed_command = stored.record;
    mark_command_locked(failed_command, HostCommandState::Unknown, std::nullopt);
    return *failed;
  }
  auto updated = stored.record;
  mark_command_locked(updated, HostCommandState::Completed, std::nullopt);
  return std::get<HostReceipt>(stopped);
}

Outcome<HostReceipt> HostService::drain_host(std::uint32_t grace_ms, std::string_view command,
                                             std::string_view payload_sha256) {
  if (!command_id(command) || !digest(payload_sha256)) {
    return invalid();
  }
  std::lock_guard lock(mutex_);
  if (!journal_open_) {
    return unavailable();
  }
  const auto now = now_of(dependencies_.clock);
  HostCommandRecord record;
  record.command_id = std::string(command);
  record.payload_sha256 = std::string(payload_sha256);
  record.kind = HostCommandKind::DrainHost;
  record.state = HostCommandState::Persisted;
  record.created_at = now;
  record.updated_at = now;
  record.retain_until = unresolved_retention(now);
  auto persisted = dependencies_.journal.persist_command(record);
  if (const auto* failed = std::get_if<Failure>(&persisted)) {
    return *failed;
  }
  auto stored = std::get<PersistedCommand>(std::move(persisted));
  if (stored.duplicate) {
    return receipt_locked(stored.record);
  }
  const auto grace = std::chrono::milliseconds{
      std::min<std::uint32_t>(grace_ms == 0 ? 10'000 : grace_ms, 10'000)};
  const auto active = dependencies_.journal.active_executions();
  if (const auto* executions = std::get_if<std::vector<ManagedExecution>>(&active)) {
    for (const auto& execution : *executions) {
      if (terminal_state(execution.state)) {
        continue;
      }
      (void)stop_instance_locked(execution.instance_id, execution.execution_generation, grace,
                                 command);
    }
  }
  flush_pending_locked();
  auto updated = stored.record;
  mark_command_locked(updated, HostCommandState::Completed, std::nullopt);
  // Drain receipt only means "durably accepted"; per-instance results travel as observations.
  return HostReceipt{std::string(command), true, std::nullopt};
}

} // namespace kiarabot::core::target
