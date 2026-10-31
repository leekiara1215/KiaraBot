#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kiarabot/core/application/control_service.hpp"

namespace kiarabot::control {
// Transport-level production settings. Every field is optional in the private JSON file; a
// missing field keeps the conservative default below so existing configurations keep starting.
struct TransportSettings {
  // Exact same-origin allowlist. Empty means no cross-origin access is granted.
  std::vector<std::string> cors_origins;
  std::uint32_t authenticated_requests_per_second{1000};
  std::uint32_t authenticated_burst{2000};
  std::uint32_t public_requests_per_second{30};
  std::uint32_t public_burst{60};
  std::uint32_t max_streams_per_token{16};
  std::uint32_t max_requests_per_connection{1000};
  std::uint32_t idle_timeout_seconds{30};
  std::uint32_t drain_timeout_seconds{3};
};
struct Configuration {
  std::string listen;
  TransportSettings transport;
  core::ControlOptions application;
};
// Reads only a current-user-owned regular file, mode 0600, without following symlinks.
Configuration read_configuration(const std::string& path);
} // namespace kiarabot::control
