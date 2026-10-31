#include "kiarabot/control/configuration.hpp"

#include <array>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace kiarabot::control {
namespace {
constexpr std::size_t kMaxCorsOrigins = 8;
constexpr std::size_t kMaxOriginLength = 256;
constexpr std::uint32_t kMaxRate = 1000000;
constexpr std::uint32_t kMaxStreams = 1024;
constexpr std::uint32_t kMaxRequestsPerConnection = 1000000;
constexpr std::uint32_t kMaxTimeoutSeconds = 300;

std::uint32_t bounded_uint(const core::ControlJson& limits, const char* key, std::uint32_t fallback,
                           std::uint32_t maximum) {
  if (!limits.contains(key)) {
    return fallback;
  }
  const auto& value = limits.at(key);
  if (!value.is_number_unsigned()) {
    throw std::invalid_argument("invalid control transport limit");
  }
  const auto parsed = value.get<std::uint64_t>();
  if (parsed < 1 || parsed > maximum) {
    throw std::invalid_argument("invalid control transport limit");
  }
  return static_cast<std::uint32_t>(parsed);
}

bool origin_allowed(std::string_view origin) {
  if (origin.size() < 8 || origin.size() > kMaxOriginLength) {
    return false;
  }
  const bool https = origin.starts_with("https://");
  const bool http = origin.starts_with("http://");
  if (!https && !http) {
    return false;
  }
  for (const char ch : origin) {
    const auto value = static_cast<unsigned char>(ch);
    if (value <= 32 || value >= 127 || ch == ',' || ch == '\\' || ch == '"') {
      return false;
    }
  }
  return true;
}

void read_transport(const core::ControlJson& value, TransportSettings& settings) {
  if (value.contains("cors_origins")) {
    const auto& origins = value.at("cors_origins");
    if (!origins.is_array() || origins.size() > kMaxCorsOrigins) {
      throw std::invalid_argument("invalid control cors origins");
    }
    for (const auto& origin : origins) {
      if (!origin.is_string() || !origin_allowed(origin.get<std::string>())) {
        throw std::invalid_argument("invalid control cors origin");
      }
      settings.cors_origins.push_back(origin.get<std::string>());
    }
  }
  if (!value.contains("limits")) {
    return;
  }
  const auto& limits = value.at("limits");
  if (!limits.is_object()) {
    throw std::invalid_argument("invalid control transport limits");
  }
  for (const auto& [name, item] : limits.items()) {
    if (name != "authenticated_requests_per_second" && name != "authenticated_burst" &&
        name != "public_requests_per_second" && name != "public_burst" &&
        name != "max_streams_per_token" && name != "max_requests_per_connection" &&
        name != "idle_timeout_seconds" && name != "drain_timeout_seconds") {
      throw std::invalid_argument("unknown control transport limit");
    }
    (void)item;
  }
  settings.authenticated_requests_per_second =
      bounded_uint(limits, "authenticated_requests_per_second",
                   settings.authenticated_requests_per_second, kMaxRate);
  settings.authenticated_burst =
      bounded_uint(limits, "authenticated_burst", settings.authenticated_burst, kMaxRate);
  settings.public_requests_per_second = bounded_uint(limits, "public_requests_per_second",
                                                     settings.public_requests_per_second, kMaxRate);
  settings.public_burst = bounded_uint(limits, "public_burst", settings.public_burst, kMaxRate);
  settings.max_streams_per_token =
      bounded_uint(limits, "max_streams_per_token", settings.max_streams_per_token, kMaxStreams);
  settings.max_requests_per_connection =
      bounded_uint(limits, "max_requests_per_connection", settings.max_requests_per_connection,
                   kMaxRequestsPerConnection);
  settings.idle_timeout_seconds = bounded_uint(limits, "idle_timeout_seconds",
                                               settings.idle_timeout_seconds, kMaxTimeoutSeconds);
  settings.drain_timeout_seconds = bounded_uint(limits, "drain_timeout_seconds",
                                                settings.drain_timeout_seconds, kMaxTimeoutSeconds);
}
} // namespace

Configuration read_configuration(const std::string& path) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("control configuration unreadable");
  }
  struct File {
    int descriptor;
    ~File() {
      close(descriptor);
    }
  } file{descriptor};
  struct stat status {};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid() ||
      (status.st_mode & 0777) != 0600 || status.st_size < 1 || status.st_size > 1048576) {
    throw std::runtime_error("invalid control configuration permissions");
  }
  std::string content;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = read(descriptor, buffer.data(), buffer.size());
    if (count < 0) {
      throw std::runtime_error("control configuration read failed");
    }
    if (count == 0) {
      break;
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
    if (content.size() > 1048576) {
      throw std::runtime_error("control configuration too large");
    }
  }
  const auto value = core::ControlJson::parse(content);
  Configuration result;
  result.listen = value.at("listen");
  result.application.encryption_key = value.at("encryption_key");
  for (const auto& user : value.at("users")) {
    // Optional explicit permission grants. Parsing only checks the JSON shape; Core owns the
    // whitelist (currently message.read) and rejects anything else at startup.
    std::vector<std::string> permissions;
    if (user.contains("permissions")) {
      const auto& entries = user.at("permissions");
      if (!entries.is_array()) {
        throw std::invalid_argument("invalid control user permissions");
      }
      for (const auto& entry : entries) {
        if (!entry.is_string()) {
          throw std::invalid_argument("invalid control user permission");
        }
        permissions.push_back(entry.get<std::string>());
      }
    }
    result.application.users.push_back({user.at("id"), user.at("username"),
                                        user.at("password_hash"), user.at("role"),
                                        std::move(permissions)});
  }
  result.application.plugins = value.value("plugins", std::vector<core::ControlJson>{});
  read_transport(value, result.transport);
  return result;
}
} // namespace kiarabot::control
