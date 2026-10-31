#include "kiarabot/infrastructure/message_projection.hpp"

#include <algorithm>
#include <chrono>

namespace kiarabot::infrastructure {
namespace {
// Matches the event stream's replay window (api/asyncapi.yaml retention_seconds).
constexpr std::chrono::minutes kRetention{10};
} // namespace

BufferedMessageProjection::BufferedMessageProjection(core::RuntimeClock clock, std::size_t limit)
    : clock_(std::move(clock)), limit_(std::max<std::size_t>(1, limit)) {
  if (!clock_) {
    clock_ = [] { return std::chrono::system_clock::now(); };
  }
}

void BufferedMessageProjection::prune(core::RuntimeTimePoint now) {
  while (!records_.empty() && records_.front().occurred + kRetention <= now) {
    records_.pop_front();
  }
  while (records_.size() > limit_) {
    records_.pop_front();
  }
}

bool BufferedMessageProjection::write(const core::MessageMetadata& record) noexcept {
  try {
    std::lock_guard lock(mutex_);
    const auto now = clock_();
    prune(now);
    // Platform message IDs are unique; a redelivered event must not duplicate a
    // notification (at-least-once ingestion is deduplicated before this point).
    const auto duplicate = std::any_of(records_.begin(), records_.end(), [&](const Entry& entry) {
      return entry.metadata.message_id == record.message_id;
    });
    if (duplicate) {
      return true;
    }
    records_.push_back({record, now});
    prune(now);
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<core::MessageMetadata> BufferedMessageProjection::read() {
  std::lock_guard lock(mutex_);
  const auto now = clock_();
  prune(now);
  std::vector<core::MessageMetadata> result;
  result.reserve(records_.size());
  for (const auto& entry : records_) {
    result.push_back(entry.metadata);
  }
  return result;
}
} // namespace kiarabot::infrastructure
