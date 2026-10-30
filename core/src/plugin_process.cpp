#include "kiarabot/core/plugin_process.hpp"

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <thread>
#include <unistd.h>

#include <sys/wait.h>

namespace kiarabot::core {
PluginProcess::~PluginProcess() {
  stop(std::chrono::milliseconds(0));
}

bool PluginProcess::start(const std::vector<std::string>& arguments,
                          const std::vector<std::string>& environment) {
  if (pid_ > 0 || arguments.empty() || arguments.front().empty() ||
      arguments.front().front() != '/') {
    return false;
  }
  std::vector<char*> argv;
  std::vector<char*> envp;
  for (const auto& argument : arguments) {
    if (argument.find('\0') != std::string::npos) {
      return false;
    }
    argv.push_back(const_cast<char*>(argument.c_str())); // POSIX API does not mutate strings.
  }
  for (const auto& variable : environment) {
    if (variable.find('\0') != std::string::npos || variable.find('=') == std::string::npos) {
      return false;
    }
    envp.push_back(const_cast<char*>(variable.c_str()));
  }
  argv.push_back(nullptr);
  envp.push_back(nullptr);
  posix_spawnattr_t attributes;
  posix_spawn_file_actions_t files;
  if (posix_spawnattr_init(&attributes) != 0) {
    return false;
  }
  if (posix_spawn_file_actions_init(&files) != 0) {
    posix_spawnattr_destroy(&attributes);
    return false;
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
  const bool started = configured && posix_spawn(&child, argv.front(), &files, &attributes,
                                                 argv.data(), envp.data()) == 0;
  posix_spawn_file_actions_destroy(&files);
  posix_spawnattr_destroy(&attributes);
  if (started) {
    pid_ = child;
  }
  return started;
}

bool PluginProcess::running() {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  pid_t result;
  do {
    result = waitpid(pid_, &status, WNOHANG);
  } while (result < 0 && errno == EINTR);
  if (result == pid_ || (result < 0 && errno == ECHILD)) {
    kill(-pid_, SIGKILL);
    pid_ = -1;
    return false;
  }
  return true;
}

bool PluginProcess::stop(std::chrono::milliseconds grace) {
  if (!running()) {
    return false;
  }
  kill(-pid_, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!running()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  kill(-pid_, SIGKILL);
  int status = 0;
  while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
  }
  pid_ = -1;
  return true;
}
} // namespace kiarabot::core
