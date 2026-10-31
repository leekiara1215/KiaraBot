#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <ostream>

#include "kiarabot/core/runtime/runtime_log.hpp"

namespace kiarabot::infrastructure {
class BufferedRuntimeLogSink final : public core::IRuntimeLogSink {
public:
  explicit BufferedRuntimeLogSink(std::ostream* output = nullptr, core::RuntimeClock clock = {});
  bool write(const core::RuntimeLog& record) noexcept override;
  std::vector<core::RuntimeLog> read() const;
  // Monotonic count of failed writes (ostream failure or exception). The
  // composition root does not export it yet; see doc/05 T10b.
  std::uint64_t write_failures() const;

private:
  std::ostream* output_;
  core::RuntimeClock clock_;
  mutable std::mutex mutex_;
  std::deque<core::RuntimeLog> records_;
  mutable std::atomic<std::uint64_t> write_failures_{0};
};
} // namespace kiarabot::infrastructure
