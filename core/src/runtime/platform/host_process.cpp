#include "kiarabot/core/runtime/platform/host_process.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <mutex>
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

constexpr auto kExitPollInterval = std::chrono::milliseconds{10};
constexpr auto kForcedExitTimeout = std::chrono::seconds{2};

Failure process_failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}

// The stop effect is unproven: the owned child may still run or may have been taken over, so the
// Runtime must keep process_outcome UNKNOWN instead of recording a stopped execution.
Failure unconfirmed_stop() {
  return {ErrorCode::UpstreamUnavailable, Acceptance::Unknown, Recovery::Backoff, std::nullopt,
          std::nullopt};
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
      static_cast<void>(kill(-child, SIGKILL));
      static_cast<void>(waitpid(child, nullptr, 0));
      return process_failure(ErrorCode::Internal);
    }
    std::lock_guard lock(mutex_);
    tracked_[child] = TrackedExecution{marker, false};
    return SpawnResult{static_cast<std::int64_t>(child), marker};
  }

  bool alive(std::int64_t pid, std::string_view marker) override {
    if (pid <= 0 || marker.empty()) {
      return false;
    }
    std::lock_guard lock(mutex_);
    auto tracked = tracked_.find(pid);
    if (tracked != tracked_.end() && tracked->second.marker == marker) {
      // Reap a child that exited spontaneously before deciding liveness. Without this, a zombie
      // keeps its pid/marker visible and a later stop marks the execution exited but leaves an
      // unreaped child (and the client process tree never fully disappears).
      if (tracked->second.exited || reap_locked(pid) == ReapStatus::Reaped) {
        return false;
      }
      return same_execution(pid, marker);
    }
    // A pid this supervisor never spawned (for example a journal left by an earlier composition)
    // can only be identified by its opaque marker; never reap a child this object does not own.
    return same_execution(pid, marker);
  }

  Outcome<bool> terminate(std::int64_t pid, std::string_view marker,
                          std::chrono::milliseconds grace) override {
    if (pid <= 0 || marker.empty()) {
      return process_failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    std::lock_guard lock(mutex_);
    const auto tracked = tracked_.find(pid);
    const bool owned = tracked != tracked_.end() && tracked->second.marker == marker;
    if (owned) {
      if (tracked->second.exited) {
        // waitpid already proved the owned child exited; a repeated stop is not a forced kill.
        return false;
      }
      if (reap_locked(pid) == ReapStatus::Reaped) {
        return false;
      }
    } else if (!same_execution(pid, marker)) {
      // Neither an owned child nor a currently marker-identical execution (for example a marker
      // mismatch, a reused pid or a foreign pid). The stop cannot be confirmed.
      return unconfirmed_stop();
    }
    const int term_result = kill(-static_cast<pid_t>(pid), SIGTERM);
    if (term_result != 0) {
      // A refused group signal is never evidence of success by itself. The refusal can race with
      // the owned child already being in its exit transition, so only a bounded wait for the real
      // waitpid exit fact (or the adopted-execution identity fact) may converge.
      if (await_stop_confirmed_locked(pid, marker, owned)) {
        return false;
      }
      return unconfirmed_stop();
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::max(grace, std::chrono::milliseconds{0});
    while (std::chrono::steady_clock::now() < deadline) {
      if (stop_confirmed_locked(pid, marker, owned)) {
        return false;
      }
      std::this_thread::sleep_for(kExitPollInterval);
    }
    const int kill_result = kill(-static_cast<pid_t>(pid), SIGKILL);
    if (kill_result != 0) {
      // On Darwin a process group whose leader already exited (or is in its exit transition)
      // rejects SIGKILL with EPERM. That refusal proves nothing on its own: the owned child must
      // be reaped by waitpid before a non-forced stop may be reported. A child that stays live
      // keeps the stop unconfirmed even when the signal was refused.
      if (await_stop_confirmed_locked(pid, marker, owned)) {
        return false;
      }
      return unconfirmed_stop();
    }
    const auto forced_deadline = std::chrono::steady_clock::now() + kForcedExitTimeout;
    while (std::chrono::steady_clock::now() < forced_deadline) {
      if (stop_confirmed_locked(pid, marker, owned)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    // Cannot confirm termination from our own child: Runtime must keep process_outcome UNKNOWN.
    return unconfirmed_stop();
  }

private:
  enum class ReapStatus { Running, Reaped, Detached };

  struct TrackedExecution {
    std::string marker;
    bool exited = false;
  };

  static bool same_execution(std::int64_t pid, std::string_view marker) {
    const std::string current = marker_for(pid);
    return !current.empty() && current == marker;
  }

  // Waits a bounded time for the stop evidence of a refused or raced signal. Refusal is still
  // never success by itself; this only gives the exit transition time to become observable.
  bool await_stop_confirmed_locked(std::int64_t pid, std::string_view marker, bool owned) {
    const auto deadline = std::chrono::steady_clock::now() + kForcedExitTimeout;
    for (;;) {
      if (stop_confirmed_locked(pid, marker, owned)) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }

  // A stop is confirmed only by facts about the recorded execution itself:
  // - an owned child needs a real waitpid exit fact (never alive()=false, proc_pidinfo failure or a
  //   refused/EPERM signal);
  // - an execution adopted after a composition-root restart (its opaque pid+start-time marker
  //   identifies it, but this process is not its parent) is confirmed gone when the marker no
  //   longer maps to that execution and the pid does not host a live process.
  bool stop_confirmed_locked(std::int64_t pid, std::string_view marker, bool owned) {
    if (owned) {
      const auto tracked = tracked_.find(pid);
      if (tracked != tracked_.end() && tracked->second.exited) {
        return true;
      }
      return reap_locked(pid) == ReapStatus::Reaped;
    }
    return adopted_execution_gone(pid, marker);
  }

  static bool adopted_execution_gone(std::int64_t pid, std::string_view marker) {
    const std::string current = marker_for(pid);
    if (!current.empty()) {
      // The pid now hosts a different execution (pid reuse): the recorded one definitely exited.
      return current != marker;
    }
    // Proc facts are unavailable or the pid hosts nothing. Only a definite "no such process"
    // probe proves that the recorded execution is gone; an unreadable live process is not proof.
    errno = 0;
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
      return false;
    }
    return errno == ESRCH;
  }

  // Non-blocking reap of the owned child. Returns Reaped only when waitpid reaped this execution,
  // which is the sole proof used to converge a stop. "Detached" means waitpid no longer reports
  // the pid as our unreaped child, so no exit fact can be recovered for it.
  ReapStatus reap_locked(std::int64_t pid) {
    int status = 0;
    pid_t result;
    do {
      result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == static_cast<pid_t>(pid)) {
      const auto tracked = tracked_.find(pid);
      if (tracked != tracked_.end()) {
        tracked->second.exited = true;
      }
      return ReapStatus::Reaped;
    }
    if (result == 0) {
      return ReapStatus::Running;
    }
    return ReapStatus::Detached;
  }

  mutable std::mutex mutex_;
  // Executions this instance spawned, kept only to prove ownership/identity and to remember a
  // waitpid exit fact across repeated calls. Bounded by the number of children this Host spawned.
  std::map<std::int64_t, TrackedExecution> tracked_;
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
