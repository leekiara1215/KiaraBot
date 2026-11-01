#pragma once

#include <cstdint>
#include <optional>
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
// Outer composition input only. Password is loaded from an owner-only file, never JSON/argv.
// Connection policy (TLS verification, channel binding, timeouts) remains in the PG adapter.
struct DatabaseMaterial {
  std::string host;
  std::uint16_t port;
  std::string name;
  std::string migration_role;
  std::string runtime_role;
  std::string password;
  std::string ca_file;
};
struct ControlApiMaterial {
  std::string listen;
  std::string identity_endpoint;
};
// Trusted file references only; mature crypto loaders own secret allocation and key validation.
struct IdentitySymmetricKeyFile {
  std::string id;
  std::string file;
};
struct IdentityDelegationKeyFiles {
  std::string id;
  std::string public_key_file;
  std::string private_key_file;
};
struct IdentityClockDeclaration {
  bool trusted = false;
  std::uint32_t maximum_known_skew_ms = 0;
};
struct IdentitySecurityMaterial {
  IdentitySymmetricKeyFile token_key;
  IdentitySymmetricKeyFile request_key;
  IdentitySymmetricKeyFile receipt_key;
  IdentityDelegationKeyFiles delegation_key;
  std::string issuer;
  IdentityClockDeclaration clock;
};
struct ProcessConfig {
  target::Service service;
  std::string probe_listen;
  std::string grpc_listen;
  TlsMaterial tls;
  std::optional<DatabaseMaterial> database;
  std::optional<ControlApiMaterial> control_api = std::nullopt;
  std::optional<IdentitySecurityMaterial> identity_security = std::nullopt;
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
// Validation time is injectable for exact certificate-boundary tests; production uses current UTC.
ConfigResult load_service_config(const std::string& path, target::Service service,
                                 target::TimePoint now = std::chrono::system_clock::now());

} // namespace kiarabot::core::platform
