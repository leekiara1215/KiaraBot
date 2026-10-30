#pragma once

#include "kiarabot/core/control_service.hpp"

namespace kiarabot::control {
struct Configuration {
  std::string listen;
  core::ControlOptions application;
};
// Reads only a current-user-owned regular file, mode 0600, without following symlinks.
Configuration read_configuration(const std::string& path);
} // namespace kiarabot::control
