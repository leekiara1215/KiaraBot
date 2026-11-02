#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "kiarabot/core/ports/target/host_ports.hpp"
#include "kiarabot/infrastructure/host/host_journal.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("host journal assertion failed at line " +
                             std::to_string(location.line()));
  }
}

t::TimePoint time(std::int64_t seconds) {
  return t::TimePoint{std::chrono::seconds{seconds}};
}

t::HostCommandRecord command(std::string id, std::string hash, t::HostCommandKind kind) {
  t::HostCommandRecord record;
  record.command_id = std::move(id);
  record.payload_sha256 = std::move(hash);
  record.kind = kind;
  record.instance_id = "instance-1";
  record.execution_generation = 1;
  record.receipt = "receipt";
  record.created_at = time(100);
  record.updated_at = time(100);
  record.retain_until = time(100) + t::kHostResolvedRetention;
  return record;
}

void check_commands(t::HostJournal& journal) {
  auto first = journal.persist_command(
      command("command-1", "hash-1", t::HostCommandKind::ApplyDesiredProcess));
  require(std::holds_alternative<t::PersistedCommand>(first));
  require(!std::get<t::PersistedCommand>(first).duplicate);
  auto duplicate = journal.persist_command(
      command("command-1", "hash-1", t::HostCommandKind::ApplyDesiredProcess));
  require(std::holds_alternative<t::PersistedCommand>(duplicate));
  require(std::get<t::PersistedCommand>(duplicate).duplicate);
  auto conflict = journal.persist_command(
      command("command-1", "hash-2", t::HostCommandKind::ApplyDesiredProcess));
  require(std::holds_alternative<t::Failure>(conflict));
  require(std::get<t::Failure>(conflict).code == t::ErrorCode::Conflict);
  auto unfinished = journal.unfinished_commands();
  require(std::holds_alternative<std::vector<t::HostCommandRecord>>(unfinished));
  require(std::get<std::vector<t::HostCommandRecord>>(unfinished).size() == 1);
  auto updated = command("command-1", "hash-1", t::HostCommandKind::ApplyDesiredProcess);
  updated.state = t::HostCommandState::Completed;
  updated.updated_at = time(200);
  require(std::holds_alternative<std::monostate>(journal.update_command(updated)));
  auto after = journal.unfinished_commands();
  require(std::holds_alternative<std::vector<t::HostCommandRecord>>(after));
  require(std::get<std::vector<t::HostCommandRecord>>(after).empty());
  auto unknown_update = updated;
  unknown_update.command_id = "missing";
  require(std::holds_alternative<t::Failure>(journal.update_command(unknown_update)));
}

void check_executions(t::HostJournal& journal) {
  t::ManagedExecution execution;
  execution.host_id = "host-1";
  execution.instance_id = "instance-1";
  execution.installation_id = "installation-1";
  execution.execution_generation = 1;
  execution.desired_version = 2;
  execution.pid = 321;
  execution.process_marker = "321:42";
  execution.state = t::ManagedExecutionState::Running;
  execution.outcome = t::ExecutionOutcome::NotStarted;
  execution.started_at = time(100);
  execution.observed_at = time(100);
  execution.command_id = "command-1";
  require(std::holds_alternative<std::monostate>(journal.save_execution(execution)));
  auto found = journal.find_execution(t::InstanceId{"instance-1"}, t::ExecutionGeneration{1});
  require(std::holds_alternative<std::optional<t::ManagedExecution>>(found));
  const auto& value = std::get<std::optional<t::ManagedExecution>>(found);
  require(value && value->pid == 321 && value->process_marker == "321:42");
  auto missing = journal.find_execution(t::InstanceId{"instance-1"}, t::ExecutionGeneration{9});
  require(std::holds_alternative<std::optional<t::ManagedExecution>>(missing));
  require(!std::get<std::optional<t::ManagedExecution>>(missing));
  auto active = journal.active_executions();
  require(std::holds_alternative<std::vector<t::ManagedExecution>>(active));
  require(std::get<std::vector<t::ManagedExecution>>(active).size() == 1);
  execution.state = t::ManagedExecutionState::Exited;
  execution.outcome = t::ExecutionOutcome::KnownSucceeded;
  execution.exit_code = 0;
  execution.exited_at = time(300);
  execution.observed_at = time(300);
  require(std::holds_alternative<std::monostate>(journal.save_execution(execution)));
  const auto finished_execution =
      journal.find_execution(t::InstanceId{"instance-1"}, t::ExecutionGeneration{1});
  require(std::holds_alternative<std::optional<t::ManagedExecution>>(finished_execution));
  const auto& finished_value = std::get<std::optional<t::ManagedExecution>>(finished_execution);
  require(finished_value && finished_value->outcome == t::ExecutionOutcome::KnownSucceeded);
  auto finished = journal.active_executions();
  require(std::get<std::vector<t::ManagedExecution>>(finished).empty());
}

void check_observations(t::HostJournal& journal) {
  t::ExecutionObservation observation;
  observation.execution.host_id = "host-1";
  observation.execution.instance_id = "instance-1";
  observation.execution.execution_generation = 1;
  observation.execution.state = t::ManagedExecutionState::Exited;
  observation.execution.outcome = t::ExecutionOutcome::KnownFailed;
  observation.execution.pid = 321;
  observation.execution.process_marker = "321:42";
  observation.execution.observed_at = time(400);
  observation.command_id = "command-1";
  auto first = journal.append_observation(observation);
  require(std::holds_alternative<t::ExecutionObservation>(first));
  require(std::get<t::ExecutionObservation>(first).sequence == 1);
  auto second = journal.append_observation(observation);
  require(std::holds_alternative<t::ExecutionObservation>(second));
  require(std::get<t::ExecutionObservation>(second).sequence == 2);
  auto pending = journal.pending_observations(10);
  require(std::holds_alternative<std::vector<t::ExecutionObservation>>(pending));
  require(std::get<std::vector<t::ExecutionObservation>>(pending).size() == 2);
  require(std::holds_alternative<std::monostate>(journal.mark_observation_reported(1)));
  auto cursor = journal.last_reported_sequence();
  require(std::holds_alternative<std::uint64_t>(cursor));
  require(std::get<std::uint64_t>(cursor) == 1);
  auto remaining = journal.pending_observations(10);
  require(std::get<std::vector<t::ExecutionObservation>>(remaining).size() == 1);
  require(std::holds_alternative<t::Failure>(journal.pending_observations(0)));
}

void check_retention(t::HostJournal& journal) {
  auto old = command("command-old", "hash-old", t::HostCommandKind::StopExecution);
  old.state = t::HostCommandState::Completed;
  old.created_at = time(1);
  old.updated_at = time(1);
  old.retain_until = time(2);
  require(std::holds_alternative<t::PersistedCommand>(journal.persist_command(old)));
  auto removed = journal.prune(time(1'000'000), 10);
  require(std::holds_alternative<std::size_t>(removed));
  require(std::get<std::size_t>(removed) >= 1);
  auto unfinished = journal.unfinished_commands();
  require(std::get<std::vector<t::HostCommandRecord>>(unfinished).empty());
}

void run() {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("kiarabot-host-journal-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  const auto path = (directory / "host.sqlite").string();
  {
    auto journal = kiarabot::infrastructure::host::make_sqlite_host_journal(path);
    require(std::holds_alternative<std::monostate>(journal->open()));
    check_commands(*journal);
    check_executions(*journal);
    check_observations(*journal);
    check_retention(*journal);
    journal->close();
  }
  {
    // Reopen must keep durable state and observation sequence monotonic.
    auto journal = kiarabot::infrastructure::host::make_sqlite_host_journal(path);
    require(std::holds_alternative<std::monostate>(journal->open()));
    // The fulfilled (exited, 7-day) execution may already be pruned by the retention check;
    // the monotonic observation sequence and new durable state are the reopen assertions.
    const auto persisted = journal->find_execution(t::InstanceId{"instance-1"},
                                                   t::ExecutionGeneration{1});
    require(std::holds_alternative<std::optional<t::ManagedExecution>>(persisted));
    t::ExecutionObservation observation;
    observation.execution.instance_id = "instance-1";
    observation.execution.execution_generation = 1;
    observation.execution.state = t::ManagedExecutionState::Exited;
    observation.execution.outcome = t::ExecutionOutcome::KnownSucceeded;
    observation.execution.observed_at = time(500);
    auto appended = journal->append_observation(observation);
    require(std::holds_alternative<t::ExecutionObservation>(appended));
    require(std::get<t::ExecutionObservation>(appended).sequence > 2);
    journal->close();
  }
  std::filesystem::remove_all(directory);
}

} // namespace

int main() {
  try {
    run();
    std::cout << "host journal fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
