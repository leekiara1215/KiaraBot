#include "kiarabot/core/runtime/platform/host_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>

#if defined(__APPLE__)
#include <libproc.h>
#else
#include <fstream>
#include <iterator>
#endif

#include "kiarabot/core/runtime/platform/host_clock.hpp"

namespace kiarabot::core::target {
namespace {

Failure process_failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}

// Opaque marker = pid + process start time. Two processes can share a pid only when the earlier
// one exited, so marker equality proves "same execution" without trusting pid reuse.
std::optional<std::string> start_token(std::int64_t pid) {
  if (pid <= 0) {
    return std::nullopt;
  }
#if defined(__APPLE__)
  proc_bsdinfo info{};
  const int size = proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
  if (size != sizeof(info)) {
    return std::nullopt;
  }
  return std::to_string(info.pbi_start_tvsec) + "." + std::to_string(info.pbi_start_tvusec);
#else
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream stream(path);
  if (!stream) {
    return std::nullopt;
  }
  std::string content{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  const auto close = content.rfind(')');
  if (close == std::string::npos || close + 2 >= content.size()) {
    return std::nullopt;
  }
  // Fields after comm start with field 3 (state); starttime is field 22 => index 19.
  std::vector<std::string_view> fields;
  std::string_view tail(content.data() + close + 2, content.size() - close - 2);
  std::size_t offset = 0;
  while (offset < tail.size()) {
    const auto separator = tail.find(' ', offset);
    const auto end = separator == std::string_view::npos ? tail.size() : separator;
    fields.push_back(tail.substr(offset, end - offset));
    offset = end + 1;
  }
  if (fields.size() <= 19) {
    return std::nullopt;
  }
  return std::string(fields[19]);
#endif
}

std::string marker_for(std::int64_t pid) {
  const auto token = start_token(pid);
  return token ? std::to_string(pid) + ":" + *token : std::string{};
}

class PosixSupervisor final : public ProcessSupervisor {
public:
  Outcome<SpawnResult> spawn(const ManagedProcessSpec& process) override {
    if (process.executable.empty() || process.executable.front() != '/' ||
        process.executable.find('\0') != std::string::npos) {
      return process_failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    std::vector<char*> argv;
    std::vector<char*> envp;
    for (const auto& argument : process.arguments) {
      if (argument.find('\0') != std::string::npos) {
        return process_failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    for (const auto& variable : process.environment) {
      if (variable.find('\0') != std::string::npos || variable.find('=') == std::string::npos ||
          variable.find('=') == 0) {
        return process_failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      envp.push_back(const_cast<char*>(variable.c_str()));
    }
    argv.push_back(nullptr);
    envp.push_back(nullptr);
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_t files;
    if (posix_spawnattr_init(&attributes) != 0) {
      return process_failure(ErrorCode::Internal);
    }
    if (posix_spawn_file_actions_init(&files) != 0) {
      posix_spawnattr_destroy(&attributes);
      return process_failure(ErrorCode::Internal);
    }
    sigset_t empty;
    sigset_t defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    const bool configured =
        posix_spawnattr_setflags(&attributes, flags) == 0 &&
        posix_spawnattr_setpgroup(&attributes, 0) == 0 &&
        posix_spawnattr_setsigmask(&attributes, &empty) == 0 &&
        posix_spawnattr_setsigdefault(&attributes, &defaults) == 0 &&
        posix_spawn_file_actions_addopen(&files, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
        posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
        posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
    pid_t child = -1;
    const bool started = configured && posix_spawn(&child, process.executable.c_str(), &files,
                                                   &attributes, argv.data(), envp.data()) == 0;
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (!started || child <= 0) {
      return process_failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
    }
    const std::string marker = marker_for(child);
    if (marker.empty()) {
      kill(-child, SIGKILL);
      waitpid(child, nullptr, 0);
      return process_failure(ErrorCode::Internal);
    }
    return SpawnResult{static_cast<std::int64_t>(child), marker};
  }

  bool alive(std::int64_t pid, std::string_view marker) override {
    if (pid <= 0 || marker.empty()) {
      return false;
    }
    // Reap a child that exited spontaneously before deciding liveness. Without this, a zombie
    // keeps its pid/marker visible and a later stop marks the execution exited but leaves an
    // unreaped child (and the client process tree never fully disappears).
    reap(pid);
    const std::string current = marker_for(pid);
    return !current.empty() && current == marker;
  }

  Outcome<bool> terminate(std::int64_t pid, std::string_view marker,
                          std::chrono::milliseconds grace) override {
    if (!alive(pid, marker)) {
      return false;
    }
    reap(pid);
    if (!alive(pid, marker)) {
      return false;
    }
    const int term_result = kill(-static_cast<pid_t>(pid), SIGTERM);
    const int term_errno = term_result == 0 ? 0 : errno;
    if (term_result != 0 && term_errno != ESRCH) {
      std::cerr << "owned fixture signal diagnostic stage=TERM errno=" << term_errno << '\n';
      return process_failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
    }
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < deadline) {
      reap(pid);
      if (!alive(pid, marker)) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const int kill_result = kill(-static_cast<pid_t>(pid), SIGKILL);
    const int kill_errno = kill_result == 0 ? 0 : errno;
    if (kill_result != 0 && kill_errno != ESRCH) {
      std::cerr << "owned fixture signal diagnostic stage=KILL errno=" << kill_errno
                << " same_execution_alive=" << alive(pid, marker) << '\n';
      return process_failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
    }
    const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (std::chrono::steady_clock::now() < kill_deadline) {
      reap(pid);
      if (!alive(pid, marker)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    // Cannot confirm termination: Runtime must keep process_outcome UNKNOWN.
    return process_failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
  }

private:
  static void reap(std::int64_t pid) {
    int status = 0;
    pid_t result;
    do {
      result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
  }
};

class SystemClock final : public HostClock {
public:
  TimePoint now() const override {
    return std::chrono::time_point_cast<TimePoint::duration>(std::chrono::system_clock::now());
  }
};

} // namespace

std::unique_ptr<ProcessSupervisor> make_posix_process_supervisor() {
  return std::make_unique<PosixSupervisor>();
}

std::unique_ptr<HostClock> make_system_host_clock() {
  return std::make_unique<SystemClock>();
}

} // namespace kiarabot::core::target
