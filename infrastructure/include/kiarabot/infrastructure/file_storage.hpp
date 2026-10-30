#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "kiarabot/core/ports/ports.hpp"

namespace kiarabot::infrastructure {

// The directory must exist and be owned by the Core process. Keys are opaque
// single components, limited to ASCII letters, digits, hyphen and underscore.
class FileStorage final : public core::IFileStorage {
public:
  explicit FileStorage(std::string directory);
  core::PortResult<std::vector<std::byte>> get(std::string_view key) override;
  core::PortStatus put(std::string_view key, const std::vector<std::byte>& bytes) override;
  core::PortStatus erase(std::string_view key) override;

private:
  std::string directory_;
  std::atomic<std::uint64_t> temporary_sequence_{0};
};

// Failure injection applies to exactly the next operation, then clears.
class FakeFileStorage final : public core::IFileStorage {
public:
  void fail_next(kiarabot::v1::ErrorCode code);
  core::PortResult<std::vector<std::byte>> get(std::string_view key) override;
  core::PortStatus put(std::string_view key, const std::vector<std::byte>& bytes) override;
  core::PortStatus erase(std::string_view key) override;

private:
  std::optional<kiarabot::v1::ErrorCode> failure_;
  std::unordered_map<std::string, std::vector<std::byte>> files_;
};

} // namespace kiarabot::infrastructure
