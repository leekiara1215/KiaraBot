#include "kiarabot/infrastructure/file_storage.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace kiarabot::infrastructure {
namespace {

class Fd {
public:
  explicit Fd(int value) : value_(value) {}
  ~Fd() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const {
    return value_;
  }

private:
  int value_;
};

bool valid_key(std::string_view key) {
  if (key.empty() || key.size() > 256) {
    return false;
  }
  for (const unsigned char c : key) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_')) {
      return false;
    }
  }
  return true;
}

kiarabot::v1::Error error(kiarabot::v1::ErrorCode code) {
  kiarabot::v1::Error result;
  result.set_code(code);
  result.set_message(code == kiarabot::v1::INVALID_ARGUMENT ? "Invalid file key"
                                                            : "File storage failure");
  return result;
}

std::optional<kiarabot::v1::Error> injected(std::optional<kiarabot::v1::ErrorCode>& failure) {
  if (!failure) {
    return std::nullopt;
  }
  const auto code = *failure;
  failure.reset();
  return error(code);
}

} // namespace

FileStorage::FileStorage(std::string directory) : directory_(std::move(directory)) {}

core::PortResult<std::vector<std::byte>> FileStorage::get(std::string_view key) {
  if (!valid_key(key)) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  const Fd directory(::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.get() < 0) {
    return error(kiarabot::v1::INTERNAL);
  }
  const std::string name(key);
  const Fd file(::openat(directory.get(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (file.get() < 0) {
    return error(errno == ENOENT ? kiarabot::v1::NOT_FOUND : kiarabot::v1::INTERNAL);
  }
  struct stat stat_buffer{};
  if (::fstat(file.get(), &stat_buffer) != 0 || !S_ISREG(stat_buffer.st_mode)) {
    return error(kiarabot::v1::INTERNAL);
  }
  std::vector<std::byte> result;
  std::byte buffer[4096];
  for (;;) {
    const ssize_t count = ::read(file.get(), buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return error(kiarabot::v1::INTERNAL);
    }
    if (count == 0) {
      return result;
    }
    result.insert(result.end(), buffer, buffer + count);
  }
}

core::PortStatus FileStorage::put(std::string_view key, const std::vector<std::byte>& bytes) {
  if (!valid_key(key)) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  const Fd directory(::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.get() < 0) {
    return error(kiarabot::v1::INTERNAL);
  }
  std::string temporary;
  int descriptor = -1;
  for (int attempt = 0; attempt < 16 && descriptor < 0; ++attempt) {
    temporary = ".kiarabot-" + std::to_string(::getpid()) + "-" +
                std::to_string(temporary_sequence_.fetch_add(1, std::memory_order_relaxed));
    descriptor = ::openat(directory.get(), temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0 && errno != EEXIST) {
      return error(kiarabot::v1::INTERNAL);
    }
  }
  if (descriptor < 0) {
    return error(kiarabot::v1::INTERNAL);
  }
  const Fd file(descriptor);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      ::unlinkat(directory.get(), temporary.c_str(), 0);
      return error(kiarabot::v1::INTERNAL);
    }
    offset += static_cast<std::size_t>(count);
  }
  const std::string name(key);
  if (::fsync(file.get()) != 0 ||
      ::renameat(directory.get(), temporary.c_str(), directory.get(), name.c_str()) != 0) {
    ::unlinkat(directory.get(), temporary.c_str(), 0);
    return error(kiarabot::v1::INTERNAL);
  }
  return std::monostate{};
}

core::PortStatus FileStorage::erase(std::string_view key) {
  if (!valid_key(key)) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  const Fd directory(::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.get() < 0) {
    return error(kiarabot::v1::INTERNAL);
  }
  const std::string name(key);
  if (::unlinkat(directory.get(), name.c_str(), 0) != 0 && errno != ENOENT) {
    return error(kiarabot::v1::INTERNAL);
  }
  return std::monostate{};
}

void FakeFileStorage::fail_next(kiarabot::v1::ErrorCode code) {
  failure_ = code;
}

core::PortResult<std::vector<std::byte>> FakeFileStorage::get(std::string_view key) {
  if (const auto failure = injected(failure_)) {
    return *failure;
  }
  if (!valid_key(key)) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  const auto it = files_.find(std::string(key));
  if (it == files_.end()) {
    return error(kiarabot::v1::NOT_FOUND);
  }
  return it->second;
}

core::PortStatus FakeFileStorage::put(std::string_view key, const std::vector<std::byte>& bytes) {
  if (const auto failure = injected(failure_)) {
    return *failure;
  }
  if (!valid_key(key)) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  files_[std::string(key)] = bytes;
  return std::monostate{};
}

core::PortStatus FakeFileStorage::erase(std::string_view key) {
  if (const auto failure = injected(failure_)) {
    return *failure;
  }
  if (!valid_key(key)) {
    return error(kiarabot::v1::INVALID_ARGUMENT);
  }
  files_.erase(std::string(key));
  return std::monostate{};
}

} // namespace kiarabot::infrastructure
