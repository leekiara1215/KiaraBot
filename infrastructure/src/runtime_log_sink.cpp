#include "kiarabot/infrastructure/runtime_log_sink.hpp"

#include <google/protobuf/util/time_util.h>

namespace kiarabot::infrastructure {
namespace {
std::string quote(std::string_view value) {
  std::string result = "\"";
  for (const unsigned char ch : value) {
    if (ch == '"' || ch == '\\') {
      result += '\\';
    }
    result += ch >= 32 && ch < 127 ? static_cast<char>(ch) : '_';
  }
  return result + '"';
}
} // namespace
BufferedRuntimeLogSink::BufferedRuntimeLogSink(std::ostream* output, core::RuntimeClock clock)
    : output_(output), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = [] { return std::chrono::system_clock::now(); };
  }
}
bool BufferedRuntimeLogSink::write(const core::RuntimeLog& record) noexcept {
  try {
    std::lock_guard lock(mutex_);
    records_.push_back(record);
    while (records_.size() > 4096 ||
           (!records_.empty() &&
            records_.front().occurred_at + std::chrono::minutes(10) <= clock_())) {
      records_.pop_front();
    }
    if (output_) {
      const auto timestamp = google::protobuf::util::TimeUtil::ToString(
          google::protobuf::util::TimeUtil::MillisecondsToTimestamp(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  record.occurred_at.time_since_epoch())
                  .count()));
      *output_ << "{\"id\":" << quote(record.id) << ",\"timestamp\":" << quote(timestamp)
               << ",\"occurred_at\":" << quote(timestamp) << ",\"level\":" << quote(record.level)
               << ",\"component\":" << quote(record.component)
               << ",\"operation\":" << quote(record.operation)
               << ",\"outcome\":" << quote(record.outcome)
               << ",\"resource\":" << quote(record.resource)
               << ",\"resource_id\":" << quote(record.resource)
               << ",\"request_id\":" << quote(record.request_id)
               << ",\"trace_id\":" << quote(record.trace_id) << "}\n";
      const bool written = static_cast<bool>(*output_);
      if (!written) {
        write_failures_.fetch_add(1, std::memory_order_relaxed);
      }
      return written;
    }
    return true;
  } catch (...) {
    write_failures_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
}
std::uint64_t BufferedRuntimeLogSink::write_failures() const {
  return write_failures_.load(std::memory_order_relaxed);
}
std::vector<core::RuntimeLog> BufferedRuntimeLogSink::read() const {
  std::lock_guard lock(mutex_);
  std::vector<core::RuntimeLog> result;
  for (const auto& record : records_) {
    if (record.occurred_at + std::chrono::minutes(10) > clock_()) {
      result.push_back(record);
    }
  }
  return result;
}
} // namespace kiarabot::infrastructure
