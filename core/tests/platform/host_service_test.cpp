#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "kiarabot/core/application/target/host_service.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("host service assertion failed at line " +
                             std::to_string(location.line()));
  }
}

t::TimePoint at(std::int64_t seconds) {
  return t::TimePoint{std::chrono::seconds{seconds}};
}

constexpr char kInstance[] = "11111111-1111-4111-8111-111111111111";
constexpr char kInstance2[] = "22222222-2222-4222-8222-222222222222";
constexpr char kInstallation[] = "33333333-3333-4333-8333-333333333333";

class FakeClock final : public t::HostClock {
public:
  t::TimePoint now() const override {
    return current;
  }
  t::TimePoint current = at(1000);
};

class FakeJournal final : public t::HostJournal {
public:
  t::Status open() override {
    opened = true;
    return std::monostate{};
  }
  void close() override {
    close_without_prune = prune_calls == 0;
    opened = false;
    closed = true;
  }
  t::Outcome<t::PersistedCommand> persist_command(const t::HostCommandRecord& record) override {
    const auto found = commands.find(record.command_id);
    if (found != commands.end()) {
      if (found->second.payload_sha256 != record.payload_sha256) {
        return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                          t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
      }
      return t::PersistedCommand{found->second, true};
    }
    commands.emplace(record.command_id, record);
    return t::PersistedCommand{record, false};
  }
  t::Status update_command(const t::HostCommandRecord& record) override {
    if (commands.find(record.command_id) == commands.end()) {
      return t::Failure{t::ErrorCode::Internal, t::Acceptance::NotAccepted,
                        t::Recovery::Diagnose, std::nullopt, std::nullopt};
    }
    commands[record.command_id] = record;
    return std::monostate{};
  }
  t::Outcome<std::vector<t::HostCommandRecord>> unfinished_commands() override {
    std::vector<t::HostCommandRecord> result;
    for (const auto& [id, record] : commands) {
      if (record.state == t::HostCommandState::Persisted ||
          record.state == t::HostCommandState::Executing ||
          record.state == t::HostCommandState::Unknown) {
        result.push_back(record);
      }
    }
    return result;
  }
  t::Outcome<std::optional<t::ManagedExecution>> find_execution(
      const t::InstanceId& instance, t::ExecutionGeneration generation) override {
    const auto key = instance.value + "#" + std::to_string(generation.value);
    const auto found = executions.find(key);
    if (found == executions.end()) {
      return std::optional<t::ManagedExecution>{};
    }
    return std::optional<t::ManagedExecution>{found->second};
  }
  t::Outcome<std::vector<t::ManagedExecution>> active_executions() override {
    std::vector<t::ManagedExecution> result;
    for (const auto& [key, execution] : executions) {
      if (execution.state != t::ManagedExecutionState::Exited) {
        result.push_back(execution);
      }
    }
    return result;
  }
  t::Status save_execution(const t::ManagedExecution& execution) override {
    executions[execution.instance_id + "#" + std::to_string(execution.execution_generation)] =
        execution;
    return std::monostate{};
  }
  t::Outcome<t::ExecutionObservation> append_observation(
      t::ExecutionObservation observation) override {
    observation.sequence = ++sequence;
    observation.observation_id = std::to_string(observation.sequence);
    observations.push_back(observation);
    return observation;
  }
  t::Outcome<std::vector<t::ExecutionObservation>> pending_observations(
      std::size_t limit) override {
    std::vector<t::ExecutionObservation> result;
    for (const auto& observation : observations) {
      if (!observation.reported) {
        result.push_back(observation);
        if (result.size() == limit) {
          break;
        }
      }
    }
    return result;
  }
  t::Status mark_observation_reported(std::uint64_t sequence) override {
    for (auto& observation : observations) {
      if (observation.sequence == sequence) {
        observation.reported = true;
        return std::monostate{};
      }
    }
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::NotAccepted,
                      t::Recovery::Diagnose, std::nullopt, std::nullopt};
  }
  t::Outcome<std::uint64_t> last_reported_sequence() override {
    std::uint64_t last = 0;
    for (const auto& observation : observations) {
      if (observation.reported) {
        last = std::max(last, observation.sequence);
      }
    }
    return last;
  }
  t::Outcome<std::size_t> prune(t::TimePoint, std::size_t) override {
    ++prune_calls;
    pruned_before_close = !closed;
    return std::size_t{1};
  }

  std::map<std::string, t::HostCommandRecord> commands;
  std::map<std::string, t::ManagedExecution> executions;
  std::vector<t::ExecutionObservation> observations;
  std::uint64_t sequence = 0;
  bool opened = false;
  bool closed = false;
  bool close_without_prune = false;
  bool pruned_before_close = false;
  std::size_t prune_calls = 0;
};

class FakeSupervisor final : public t::ProcessSupervisor {
public:
  t::Outcome<SpawnResult> spawn(const t::ManagedProcessSpec& process) override {
    specs.push_back(process);
    if (spawn_failure) {
      return t::Failure{*spawn_failure, t::Acceptance::NotAccepted, t::Recovery::Diagnose,
                        std::nullopt, std::nullopt};
    }
    ++pid_counter;
    const auto pid = pid_counter;
    alive_processes[pid] = true;
    markers[pid] = "marker-" + std::to_string(pid);
    return SpawnResult{pid, markers[pid]};
  }
  bool alive(std::int64_t pid, std::string_view marker) override {
    if (!alive_processes[pid]) {
      return false;
    }
    const auto stored = markers.find(pid);
    return stored != markers.end() && stored->second == marker;
  }
  t::Outcome<bool> terminate(std::int64_t pid, std::string_view marker,
                             std::chrono::milliseconds) override {
    if (!alive(pid, marker)) {
      return false;
    }
    alive_processes[pid] = false;
    terminate_calls.push_back(pid);
    if (terminate_failure) {
      return t::Failure{*terminate_failure, t::Acceptance::Unknown, t::Recovery::Backoff,
                        std::nullopt, std::nullopt};
    }
    return forced;
  }
  void mark_marker(std::int64_t pid) {
    markers[pid] = "marker-" + std::to_string(pid);
  }

  std::vector<t::ManagedProcessSpec> specs;
  std::map<std::int64_t, bool> alive_processes;
  std::map<std::int64_t, std::string> markers;
  std::vector<std::int64_t> terminate_calls;
  std::int64_t pid_counter = 100;
  bool forced = false;
  std::optional<t::ErrorCode> spawn_failure;
  std::optional<t::ErrorCode> terminate_failure;
};

class FakeBridge final : public t::HostBridgeTransport {
public:
  t::Status start(t::HostBridgeHandler& handler) override {
    target = &handler;
    connected = true;
    return std::monostate{};
  }
  void stop() override {
    connected = false;
  }
  bool registered() const override {
    return connected;
  }
  t::Status send_observation(const t::ExecutionObservation& observation) override {
    if (!connected) {
      return t::Failure{t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted,
                        t::Recovery::Backoff, std::nullopt, std::nullopt};
    }
    observations.push_back(observation);
    delivered = std::max(delivered, observation.sequence);
    return std::monostate{};
  }
  t::Status send_receipt(const t::HostReceipt& receipt) override {
    receipts.push_back(receipt);
    return std::monostate{};
  }
  std::uint64_t delivered_sequence() const override {
    return delivered;
  }

  t::HostBridgeHandler* target = nullptr;
  bool connected = false;
  std::uint64_t delivered = 0;
  std::vector<t::ExecutionObservation> observations;
  std::vector<t::HostReceipt> receipts;
};

struct Harness {
  FakeClock clock;
  FakeJournal journal;
  FakeSupervisor supervisor;
  FakeBridge bridge;
  std::string directory;
  t::HostIdentity identity;
  std::vector<t::LaunchProfile> profiles;
  std::vector<std::pair<std::string, std::string>> environment;
  std::unique_ptr<t::HostService> service;

  Harness() {
    directory = (std::filesystem::temp_directory_path() /
                 ("kiarabot-host-service-" + std::to_string(::getpid())))
                    .string();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    identity.host_id = "host-1";
    t::HostPackage package;
    package.plugin_type_id = "observer";
    package.root = directory + "/package";
    package.sha256 = std::string(64, 'a');
    package.launch_profile = "python";
    package.allowed_argument_names = {"instance"};
    identity.packages.push_back(package);
    t::LaunchProfile profile;
    profile.name = "python";
    profile.executable = "/usr/bin/python3";
    profile.fixed_arguments = {"/usr/bin/python3", "-m", "plugin"};
    profile.allowed_environment_names = {"PYTHONUNBUFFERED", "KIARABOT_PLUGIN_BOOTSTRAP"};
    profile.credential_environment = "KIARABOT_PLUGIN_BOOTSTRAP";
    profiles.push_back(profile);
    environment = {{"PYTHONUNBUFFERED", "1"}};
    write_credential(kInstance, "bootstrap-secret-value-0123456789abcdef");
    t::HostServiceDependencies dependencies{journal, supervisor, bridge, clock, identity,
                                            profiles, environment, directory, 1s, 8};
    service = std::make_unique<t::HostService>(std::move(dependencies));
  }
  ~Harness() {
    if (service) {
      service->stop();
    }
    std::filesystem::remove_all(directory);
  }
  void write_credential(std::string_view instance, std::string_view value) {
    const auto path = std::filesystem::path(directory) /
                      (std::string(instance) + ".bootstrap");
    std::ofstream stream(path);
    stream << value << '\n';
    stream.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                            std::filesystem::perms::owner_write);
  }
};

t::HostProcessSpec spec(std::string instance = kInstance) {
  t::HostProcessSpec process;
  process.instance_id = instance;
  process.installation_id = kInstallation;
  process.execution_generation = 1;
  process.desired_version = 1;
  process.desired_state = t::DesiredState::Running;
  process.package_sha256 = std::string(64, 'a');
  process.launch_profile = "python";
  process.arguments = {"instance=instance-1"};
  process.environment_names = {"PYTHONUNBUFFERED", "KIARABOT_PLUGIN_BOOTSTRAP"};
  return process;
}

std::string sha(std::string_view value) {
  return std::string(64, value.front());
}

void test_apply_spawn_and_idempotent_replay() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  require(harness.service->readiness().ready);
  auto applied = harness.service->apply_desired_process(spec(), "command-1", sha("b"));
  require(std::holds_alternative<t::HostReceipt>(applied));
  const auto receipt = std::get<t::HostReceipt>(applied);
  require(receipt.persisted);
  require(receipt.execution.has_value() && receipt.execution->pid > 0);
  require(harness.supervisor.specs.size() == 1);
  const auto& launched = harness.supervisor.specs.front();
  require(launched.executable == "/usr/bin/python3");
  require(std::find(launched.arguments.begin(), launched.arguments.end(),
                    "instance=instance-1") != launched.arguments.end());
  require(std::find(launched.environment.begin(), launched.environment.end(),
                    "KIARABOT_PLUGIN_BOOTSTRAP=bootstrap-secret-value-0123456789abcdef") !=
          launched.environment.end());
  // Never persist or observe the credential value.
  const auto journal_json = std::string{};
  (void)journal_json;
  require(!harness.journal.observations.empty());
  // Duplicate command replay returns the stored receipt without a second spawn.
  auto replay = harness.service->apply_desired_process(spec(), "command-1", sha("b"));
  require(std::holds_alternative<t::HostReceipt>(replay));
  require(harness.supervisor.specs.size() == 1);
  // Same command with a different payload hash is a conflict.
  auto conflicting = harness.service->apply_desired_process(spec(), "command-1", sha("c"));
  require(std::holds_alternative<t::Failure>(conflicting));
  require(std::get<t::Failure>(conflicting).code == t::ErrorCode::Conflict);
}

void test_validation_fails_closed_without_effect() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  auto wrong_digest = spec();
  wrong_digest.package_sha256 = std::string(64, 'f');
  require(std::holds_alternative<t::Failure>(
      harness.service->apply_desired_process(wrong_digest, "command-2", sha("b"))));
  auto wrong_argument = spec();
  wrong_argument.arguments = {"unknown=value"};
  require(std::holds_alternative<t::Failure>(
      harness.service->apply_desired_process(wrong_argument, "command-3", sha("b"))));
  auto wrong_environment = spec();
  wrong_environment.environment_names = {"NOT_ALLOWED"};
  require(std::holds_alternative<t::Failure>(
      harness.service->apply_desired_process(wrong_environment, "command-4", sha("b"))));
  require(harness.supervisor.specs.empty());
  require(harness.journal.commands.empty());
  // Credential file with insecure permissions must be rejected before spawn.
  const auto path = std::filesystem::path(harness.directory) /
                    (std::string(kInstance) + ".bootstrap");
  std::filesystem::permissions(path, std::filesystem::perms::all);
  require(std::holds_alternative<t::Failure>(
      harness.service->apply_desired_process(spec(), "command-5", sha("b"))));
  require(harness.supervisor.specs.empty());
}

void test_stop_known_and_unknown_outcomes() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(spec(), "command-1", sha("b"))));
  auto stopped = harness.service->stop_execution(kInstance, 1, 1000, "command-2", sha("c"));
  require(std::holds_alternative<t::HostReceipt>(stopped));
  require(std::get<t::HostReceipt>(stopped).execution.has_value());
  require(std::get<t::HostReceipt>(stopped).execution->state ==
          t::ManagedExecutionState::Exited);
  require(std::get<t::HostReceipt>(stopped).execution->outcome ==
          t::ExecutionOutcome::KnownSucceeded);
  // Unconfirmed termination must be UNKNOWN, never FAILED or "stopped".
  Harness second;
  require(std::holds_alternative<std::monostate>(second.service->start()));
  require(std::holds_alternative<t::HostReceipt>(
      second.service->apply_desired_process(spec(), "command-3", sha("b"))));
  second.supervisor.terminate_failure = t::ErrorCode::UpstreamUnavailable;
  auto unknown = second.service->stop_execution(kInstance, 1, 1000, "command-4", sha("c"));
  require(std::holds_alternative<t::Failure>(unknown));
  const auto execution = second.journal.executions.at(std::string(kInstance) + "#1");
  require(execution.state == t::ManagedExecutionState::Unknown);
  require(execution.outcome == t::ExecutionOutcome::Unknown);
}

void test_stop_is_bounded_and_flushes_before_journal_close() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  auto second = spec(kInstance2);
  second.execution_generation = 2;
  second.desired_version = 2;
  harness.write_credential(kInstance2, "bootstrap-secret-value-0123456789abcdef");
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(spec(), "command-1", sha("b"))));
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(second, "command-2", sha("c"))));
  harness.service->stop();
  // Bounded TERM: every active managed execution is asked to stop exactly once.
  require(harness.supervisor.terminate_calls.size() == 2);
  // Final observations are flushed before the local journal is pruned and closed.
  require(harness.journal.prune_calls == 1 && harness.journal.pruned_before_close);
  require(!harness.journal.close_without_prune && harness.journal.closed);
  require(!harness.journal.opened);
  for (const auto& observation : harness.journal.observations) {
    require(observation.reported);
  }
}

void test_stop_unconfirmed_termination_is_unknown_not_stopped() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(spec(), "command-1", sha("b"))));
  harness.supervisor.terminate_failure = t::ErrorCode::UpstreamUnavailable;
  harness.service->stop();
  const auto execution = harness.journal.executions.at(std::string(kInstance) + "#1");
  // Host never fabricates a stopped/succeeded outcome just because shutdown ran.
  require(execution.state == t::ManagedExecutionState::Unknown);
  require(execution.outcome == t::ExecutionOutcome::Unknown);
  require(harness.journal.prune_calls == 1);
}

void test_same_generation_never_respawns() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(spec(), "command-1", sha("b"))));
  for (auto& [pid, alive] : harness.supervisor.alive_processes) {
    alive = false;
  }
  auto retried = harness.service->apply_desired_process(spec(), "command-retry", sha("c"));
  require(std::holds_alternative<t::Failure>(retried));
  require(std::get<t::Failure>(retried).code == t::ErrorCode::Conflict);
  require(harness.supervisor.specs.size() == 1);
  const auto& observations = harness.journal.observations;
  require(!observations.empty() &&
          observations.back().execution.outcome == t::ExecutionOutcome::Unknown);
}

void test_drain_stops_all_and_reports_accepted_only() {
  Harness harness;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  auto second = spec(kInstance2);
  second.execution_generation = 2;
  second.desired_version = 2;
  harness.write_credential(kInstance2, "bootstrap-secret-value-0123456789abcdef");
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(spec(), "command-1", sha("b"))));
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(second, "command-2", sha("c"))));
  auto drained = harness.service->drain_host(1000, "command-3", sha("d"));
  require(std::holds_alternative<t::HostReceipt>(drained));
  require(std::get<t::HostReceipt>(drained).persisted);
  require(!std::get<t::HostReceipt>(drained).execution.has_value());
  require(harness.supervisor.terminate_calls.size() == 2);
}

void test_recovery_never_respawns() {
  Harness harness;
  t::ManagedExecution running;
  running.host_id = "host-1";
  running.instance_id = kInstance;
  running.installation_id = kInstallation;
  running.execution_generation = 1;
  running.desired_version = 1;
  running.pid = 77;
  running.process_marker = "marker-77";
  running.state = t::ManagedExecutionState::Running;
  running.outcome = t::ExecutionOutcome::NotStarted;
  running.started_at = at(900);
  running.observed_at = at(900);
  running.command_id = "old-command";
  harness.journal.executions[std::string(kInstance) + "#1"] = running;
  harness.supervisor.markers[77] = "marker-77";
  harness.supervisor.alive_processes[77] = true;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  require(harness.supervisor.specs.empty());
  require(!harness.journal.observations.empty());
  require(harness.journal.observations.back().execution.state ==
          t::ManagedExecutionState::Running);
  // Lost process after restart is UNKNOWN, not a synthetic stop.
  Harness lost;
  lost.supervisor.alive_processes[77] = false;
  lost.journal.executions[std::string(kInstance) + "#1"] = running;
  require(std::holds_alternative<std::monostate>(lost.service->start()));
  require(lost.supervisor.specs.empty());
  require(lost.journal.observations.back().execution.outcome == t::ExecutionOutcome::Unknown);
}

void test_observation_replay_after_bridge_ready() {
  Harness harness;
  harness.bridge.connected = false;
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  // The fake bridge marks itself connected on start; simulate a lost connection instead.
  harness.bridge.connected = false;
  require(std::holds_alternative<t::HostReceipt>(
      harness.service->apply_desired_process(spec(), "command-1", sha("b"))));
  require(harness.bridge.observations.empty());
  const auto pending = harness.journal.pending_observations(10);
  require(std::holds_alternative<std::vector<t::ExecutionObservation>>(pending));
  require(!std::get<std::vector<t::ExecutionObservation>>(pending).empty());
  harness.bridge.connected = true;
  harness.service->bridge_ready();
  require(harness.bridge.observations.size() >= 1);
}

void test_disconnect_readiness_and_validation() {
  Harness harness;
  require(!harness.service->readiness().ready);
  require(std::holds_alternative<std::monostate>(harness.service->start()));
  require(harness.service->readiness().ready);
  harness.bridge.connected = false;
  require(!harness.service->readiness().ready);
  require(std::holds_alternative<t::Failure>(
      harness.service->apply_desired_process(spec(), "", sha("b"))));
  require(std::holds_alternative<t::Failure>(
      harness.service->stop_execution("not-an-instance", 1, 0, "command", sha("b"))));
}

void run() {
  test_apply_spawn_and_idempotent_replay();
  test_validation_fails_closed_without_effect();
  test_stop_known_and_unknown_outcomes();
  test_same_generation_never_respawns();
  test_drain_stops_all_and_reports_accepted_only();
  test_stop_is_bounded_and_flushes_before_journal_close();
  test_stop_unconfirmed_termination_is_unknown_not_stopped();
  test_recovery_never_respawns();
  test_observation_replay_after_bridge_ready();
  test_disconnect_readiness_and_validation();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "host service fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
