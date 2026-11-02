#include <chrono>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>

#include "kiarabot/core/runtime/platform/host_clock.hpp"
#include "kiarabot/core/runtime/platform/host_process.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("host process assertion failed at line " +
                             std::to_string(location.line()));
  }
}

t::ManagedProcessSpec sleep_process(std::string command) {
  t::ManagedProcessSpec process;
  process.executable = "/bin/sh";
  process.arguments = {"/bin/sh", "-c", std::move(command)};
  process.environment = {"PATH=/usr/bin:/bin"};
  return process;
}

void check_lifecycle() {
  const auto supervisor = t::make_posix_process_supervisor();
  auto spawned = supervisor->spawn(sleep_process("sleep 30"));
  require(std::holds_alternative<t::ProcessSupervisor::SpawnResult>(spawned));
  const auto child = std::get<t::ProcessSupervisor::SpawnResult>(spawned);
  require(child.pid > 0);
  require(!child.marker.empty());
  require(supervisor->alive(child.pid, child.marker));
  // A different marker (simulated pid reuse) must never identify the same execution.
  require(!supervisor->alive(child.pid, "1:1"));
  auto stopped = supervisor->terminate(child.pid, child.marker, 2s);
  require(std::holds_alternative<bool>(stopped));
  require(!std::get<bool>(stopped));
  require(!supervisor->alive(child.pid, child.marker));
  // Stopping an already stopped execution is not a forced kill and not a failure.
  require(std::holds_alternative<bool>(supervisor->terminate(child.pid, child.marker, 1s)));
}

void check_grace_and_force() {
  const auto supervisor = t::make_posix_process_supervisor();
  // The shell ignores TERM and stays alive until KILL.
  auto spawned = supervisor->spawn(sleep_process("trap '' TERM; while true; do sleep 1; done"));
  require(std::holds_alternative<t::ProcessSupervisor::SpawnResult>(spawned));
  const auto child = std::get<t::ProcessSupervisor::SpawnResult>(spawned);
  // Let the shell install its TERM ignore before the bounded stop begins.
  std::this_thread::sleep_for(300ms);
  auto stopped = supervisor->terminate(child.pid, child.marker, 100ms);
  require(std::holds_alternative<bool>(stopped));
  require(std::get<bool>(stopped));
  require(!supervisor->alive(child.pid, child.marker));
}

void check_invalid_launch() {
  const auto supervisor = t::make_posix_process_supervisor();
  auto relative = supervisor->spawn(t::ManagedProcessSpec{"python", {}, {}});
  require(std::holds_alternative<t::Failure>(relative));
  require(std::get<t::Failure>(relative).code == t::ErrorCode::InvalidArgument);
  auto missing = supervisor->spawn(t::ManagedProcessSpec{"/missing/executable", {}, {}});
  require(std::holds_alternative<t::Failure>(missing));
  auto bad_environment = sleep_process("true");
  bad_environment.environment = {"NO_EQUALS"};
  require(std::holds_alternative<t::Failure>(supervisor->spawn(bad_environment)));
}

void check_clock() {
  const auto clock = t::make_system_host_clock();
  const auto first = clock->now();
  const auto second = clock->now();
  require(first.time_since_epoch().count() > 0);
  require(second >= first);
}

void run() {
  check_lifecycle();
  check_grace_and_force();
  check_invalid_launch();
  check_clock();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "host process fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
