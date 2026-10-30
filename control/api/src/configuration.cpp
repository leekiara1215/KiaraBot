#include "kiarabot/control/configuration.hpp"

#include <array>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

#include <sys/stat.h>

namespace kiarabot::control {
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
  struct stat status{};
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
    result.application.users.push_back(
        {user.at("id"), user.at("username"), user.at("password_hash"), user.at("role")});
  }
  result.application.plugins = value.value("plugins", std::vector<core::ControlJson>{});
  return result;
}
} // namespace kiarabot::control
