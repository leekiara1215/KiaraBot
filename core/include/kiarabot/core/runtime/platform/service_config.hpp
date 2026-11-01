#pragma once

#include <string>
#include <string_view>
#include <variant>

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::core::platform {

struct TlsMaterial {
  std::string certificate;
  std::string private_key;
  std::string ca;
};
struct ProcessConfig {
  target::Service service;
  std::string probe_listen;
  std::string grpc_listen;
  TlsMaterial tls;
};
struct ConfigFailure {
  int exit_code;
  // Static reason only; never exception/config/PEM text.
  std::string_view reason;
};
using ConfigResult = std::variant<ProcessConfig, ConfigFailure>;
std::string_view service_name(target::Service service) noexcept;
// Strict schema v1. Regular files only, <=1MiB each; private key requires owner-only permissions.
// Reads and validates TLS chain/time/key before any listener. Failure never contains file content.
ConfigResult load_service_config(const std::string& path, target::Service service);

} // namespace kiarabot::core::platform
