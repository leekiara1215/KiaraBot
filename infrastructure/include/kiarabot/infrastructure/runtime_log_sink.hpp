#pragma once

#include <deque>
#include <mutex>
#include <ostream>

#include "kiarabot/core/runtime_log.hpp"

namespace kiarabot::infrastructure {
class BufferedRuntimeLogSink final : public core::IRuntimeLogSink {
public:
  explicit BufferedRuntimeLogSink(std::ostream* output = nullptr, core::RuntimeClock clock = {});
  bool write(const core::RuntimeLog& record) noexcept override;
  std::vector<core::RuntimeLog> read() const;

private:
  std::ostream* output_;
  core::RuntimeClock clock_;
  mutable std::mutex mutex_;
  std::deque<core::RuntimeLog> records_;
};
} // namespace kiarabot::infrastructure
