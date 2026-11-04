#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/wait.h>

#include "kiarabot/core/runtime/platform/host_clock.hpp"
#include "kiarabot/core/runtime/platform/host_process.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message = {},
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("host process assertion failed at line " +
                             std::to_string(location.line()) +
                             (message.empty() ? std::string{} : ": " + std::string{message}));
  }
}

t::ManagedProcessSpec sleep_process(std::string command) {
  t::ManagedProcessSpec process;
  process.executable = "/bin/sh";
  process.arguments = {"/bin/sh", "-c", std::move(command)};
  process.environment = {"PATH=/usr/bin:/bin"};
  return process;
}

struct TemporaryDirectory {
  std::filesystem::path root;
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "kiarabot-host-process-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    require(::mkdtemp(bytes.data()) != nullptr, "temporary host process directory");
    root = bytes.data();
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

t::ProcessSupervisor::SpawnResult spawn_or_fail(t::ProcessSupervisor& supervisor,
                                                std::string command) {
  auto spawned = supervisor.spawn(sleep_process(std::move(command)));
  require(std::holds_alternative<t::ProcessSupervisor::SpawnResult>(spawned),
          "fixed test process starts");
  return std::get<t::ProcessSupervisor::SpawnResult>(spawned);
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

// TERM must converge a normal execution even when the bounded grace is legally as short as
// 0/1/10ms. Darwin reports EPERM when SIGKILL reaches a process group whose leader already exited
// but was not yet reaped, so the short grace ordering is exactly where the old code failed.
void check_short_grace_stops() {
  const auto supervisor = t::make_posix_process_supervisor();
  for (const auto grace : {0ms, 1ms, 10ms}) {
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto child = spawn_or_fail(*supervisor, "exec sleep 30");
      const auto stopped = supervisor->terminate(child.pid, child.marker, grace);
      require(std::holds_alternative<bool>(stopped),
              "short grace never reports an unconfirmed stop for an owned child");
      require(!supervisor->alive(child.pid, child.marker));
    }
  }
}

void check_normal_term() {
  const auto supervisor = t::make_posix_process_supervisor();
  const auto child = spawn_or_fail(*supervisor, "exec sleep 30");
  const auto stopped = supervisor->terminate(child.pid, child.marker, 2s);
  require(std::holds_alternative<bool>(stopped) && !std::get<bool>(stopped),
          "TERM alone stops the execution without a forced kill");
  require(!supervisor->alive(child.pid, child.marker));
}

void check_grace_and_force() {
  TemporaryDirectory temporary;
  const auto ready = temporary.root / "term-ignored.ready";
  const auto supervisor = t::make_posix_process_supervisor();
  // The shell ignores TERM and stays alive until KILL. The fixed ready file is an explicit
  // handshake: the bounded stop must not start before the TERM ignore is actually installed.
  const auto child = spawn_or_fail(*supervisor, "trap '' TERM; : > '" + ready.string() +
                                                    "'; while true; do sleep 1; done");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!std::filesystem::exists(ready) && std::chrono::steady_clock::now() < deadline) {
    require(supervisor->alive(child.pid, child.marker), "trap fixture stays at its marker");
    std::this_thread::sleep_for(1ms);
  }
  require(std::filesystem::exists(ready), "TERM-ignoring fixture installed its trap");
  auto stopped = supervisor->terminate(child.pid, child.marker, 100ms);
  require(std::holds_alternative<bool>(stopped));
  require(std::get<bool>(stopped));
  require(!supervisor->alive(child.pid, child.marker));
}

// A stop that cannot be tied to this supervisor's own child must stay unconfirmed; a refused
// signal must never be swallowed as "stopped" while the target is still live.
void check_unprovable_stops_are_refused() {
  const auto supervisor = t::make_posix_process_supervisor();
  const auto child = spawn_or_fail(*supervisor, "exec sleep 30");
  const auto mismatch = supervisor->terminate(child.pid, "1:1", 20ms);
  require(std::holds_alternative<t::Failure>(mismatch) &&
              std::get<t::Failure>(mismatch).code == t::ErrorCode::UpstreamUnavailable,
          "identity mismatch keeps the stop unconfirmed");
  require(supervisor->alive(child.pid, child.marker));
  const auto foreign = ::fork();
  require(foreign >= 0, "fork unowned probe child");
  if (foreign == 0) {
    ::pause();
    ::_exit(0);
  }
  const auto unowned = supervisor->terminate(static_cast<std::int64_t>(foreign), "1:1", 20ms);
  require(std::holds_alternative<t::Failure>(unowned), "unowned pid keeps the stop unconfirmed");
  require(::kill(foreign, 0) == 0, "refused stop does not signal an unowned process");
  static_cast<void>(::kill(foreign, SIGKILL));
  int status = 0;
  require(::waitpid(foreign, &status, 0) == foreign, "reap unowned probe child");
  const auto stopped = supervisor->terminate(child.pid, child.marker, 2s);
  require(std::holds_alternative<bool>(stopped) && !std::get<bool>(stopped) &&
              !supervisor->alive(child.pid, child.marker),
          "confirmed TERM stop converges only for the recorded execution");
}

// A composition root restarted after a Host crash is not the parent of an execution that
// survived. The opaque pid+start-time marker still identifies it, so a bounded TERM stop must
// converge from that identity fact even though no waitpid fact is available.
void check_adopted_execution_stop() {
  int channel[2] = {-1, -1};
  require(::pipe(channel) == 0, "adopted execution channel");
  const pid_t middle = ::fork();
  require(middle >= 0, "fork previous composition probe");
  if (middle == 0) {
    ::close(channel[0]);
    const auto owner = t::make_posix_process_supervisor();
    auto spawned = owner->spawn(sleep_process("exec sleep 10"));
    if (!std::holds_alternative<t::ProcessSupervisor::SpawnResult>(spawned)) {
      ::_exit(70);
    }
    const auto child = std::get<t::ProcessSupervisor::SpawnResult>(spawned);
    const std::string payload = std::to_string(child.pid) + "\n" + child.marker + "\n";
    const auto written = ::write(channel[1], payload.data(), payload.size());
    ::close(channel[1]);
    ::_exit(written == static_cast<ssize_t>(payload.size()) ? 0 : 71);
  }
  ::close(channel[1]);
  std::string payload;
  char buffer[128];
  for (;;) {
    const auto read_size = ::read(channel[0], buffer, sizeof(buffer));
    require(read_size >= 0, "read adopted execution identity");
    if (read_size == 0) {
      break;
    }
    payload.append(buffer, static_cast<std::size_t>(read_size));
  }
  ::close(channel[0]);
  int middle_status = 0;
  require(::waitpid(middle, &middle_status, 0) == middle && WIFEXITED(middle_status) &&
              WEXITSTATUS(middle_status) == 0,
          "previous composition published the adopted execution identity");
  const auto separator = payload.find('\n');
  require(separator != std::string::npos && payload.size() > separator + 2,
          "adopted execution identity is complete");
  const auto pid = std::stoll(payload.substr(0, separator));
  const auto marker = payload.substr(separator + 1, payload.size() - separator - 2);
  const auto restarted = t::make_posix_process_supervisor();
  require(::kill(static_cast<pid_t>(pid), 0) == 0, "adopted execution is live");
  require(restarted->alive(pid, marker), "adopted marker identifies the still-live execution");
  const auto stopped = restarted->terminate(pid, marker, 2s);
  require(std::holds_alternative<bool>(stopped) && !std::get<bool>(stopped),
          "marker-identified adopted execution converges without a waitpid fact");
  require(::kill(static_cast<pid_t>(pid), 0) != 0, "adopted execution exited after TERM");
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
  check_short_grace_stops();
  check_normal_term();
  check_grace_and_force();
  check_unprovable_stops_are_refused();
  check_adopted_execution_stop();
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
