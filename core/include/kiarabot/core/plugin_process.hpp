#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <sys/types.h>

namespace kiarabot::core {
// POSIX process owner, used only by the executable composition root. Single
// thread only. No shell, inherited environment, Python runtime or business logic.
class PluginProcess {
public:
  PluginProcess() = default;
  ~PluginProcess();
  PluginProcess(const PluginProcess&) = delete;
  PluginProcess& operator=(const PluginProcess&) = delete;
  PluginProcess(PluginProcess&&) = delete;
  PluginProcess& operator=(PluginProcess&&) = delete;

  bool start(const std::vector<std::string>& arguments,
             const std::vector<std::string>& environment);
  // Reaps exited children and removes remaining members of their process group.
  bool running();
  // TERM, bounded grace, then KILL and reap. True means force was required.
  bool stop(std::chrono::milliseconds grace);

private:
  pid_t pid_ = -1;
};
} // namespace kiarabot::core
