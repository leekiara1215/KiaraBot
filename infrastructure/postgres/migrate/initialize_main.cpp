#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>

#include <sys/stat.h>

#include "kiarabot/infrastructure/postgres/database.hpp"

namespace {
namespace target = kiarabot::core::target;

class SecretFile final {
public:
  explicit SecretFile(const char* path)
      : descriptor_(::open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)) {}
  ~SecretFile() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }
  SecretFile(const SecretFile&) = delete;
  SecretFile& operator=(const SecretFile&) = delete;
  std::optional<std::string> read() const {
    struct stat status {};
    if (descriptor_ < 0 || ::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::getuid() || (status.st_mode & 0077) != 0 || status.st_size < 1 ||
        status.st_size > 4096) {
      return {};
    }
    std::string secret(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t offset = 0;
    while (offset < secret.size()) {
      const auto count = ::read(descriptor_, secret.data() + offset, secret.size() - offset);
      if (count <= 0) {
        return {};
      }
      offset += static_cast<std::size_t>(count);
    }
    if (secret.back() == '\n') {
      secret.pop_back();
    }
    if (secret.empty() || secret.find_first_of("\r\n") != std::string::npos ||
        secret.find('\0') != std::string::npos) {
      return {};
    }
    return secret;
  }

private:
  int descriptor_;
};

std::optional<target::Service> parse_service(std::string_view value) {
  if (value == "identity") {
    return target::Service::Identity;
  }
  if (value == "runtime") {
    return target::Service::Runtime;
  }
  if (value == "scheduler") {
    return target::Service::Scheduler;
  }
  if (value == "model") {
    return target::Service::Model;
  }
  return {};
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "kiarabot-pg-initialize --initialize-empty SERVICE HOST PORT DATABASE "
                   "MIGRATION_ROLE RUNTIME_ROLE CA_FILE PASSWORD_FILE\n";
      return 0;
    }
    if (argc != 10 || std::string_view(argv[1]) != "--initialize-empty") {
      std::cerr << "INVALID_ARGUMENT\n";
      return 2;
    }
    const auto service = parse_service(argv[2]);
    unsigned int port = 0;
    const std::string_view port_text(argv[4]);
    const auto parsed = std::from_chars(port_text.begin(), port_text.end(), port);
    const auto password = SecretFile(argv[9]).read();
    if (!service || parsed.ec != std::errc{} || parsed.ptr != port_text.end() || port == 0 ||
        port > 65535 || !password) {
      std::cerr << "INVALID_ARGUMENT\n";
      return 2;
    }
    const kiarabot::infrastructure::postgres::DatabaseConfig config{
        *service, argv[3], static_cast<std::uint16_t>(port), argv[5], argv[6], *password, argv[8],
        argv[6],  argv[7]};
    const auto initialized = kiarabot::infrastructure::postgres::initialize_empty(config);
    if (const auto* error = std::get_if<target::Failure>(&initialized)) {
      if (error->code == target::ErrorCode::InvalidArgument) {
        std::cerr << "INVALID_ARGUMENT\n";
        return 2;
      }
      std::cerr << (error->acceptance == target::Acceptance::Unknown ? "INITIALIZATION_UNKNOWN\n"
                                                                     : "INITIALIZATION_REJECTED\n");
      return 1;
    }
    std::cout << "INITIALIZED\n";
    return 0;
  } catch (...) {
    std::cerr << "INTERNAL\n";
    return 1;
  }
}
