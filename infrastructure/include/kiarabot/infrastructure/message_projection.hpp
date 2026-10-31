#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "kiarabot/core/domain/runtime_types.hpp"
#include "kiarabot/core/runtime/message_projection.hpp"

namespace kiarabot::infrastructure {
// Bounded in-memory projection of accepted message metadata. It mirrors the
// event stream's ten-minute replay window and drops duplicates by message_id.
// Losing it on restart is intentional: the control stream is a notification
// feed, not durable history, and clients must not reconstruct history from it.
class BufferedMessageProjection final : public core::IMessageMetadataSink {
public:
  explicit BufferedMessageProjection(core::RuntimeClock clock = {}, std::size_t limit = 4096);
  bool write(const core::MessageMetadata& record) noexcept override;
  // Returns entries still inside the replay window, oldest first.
  std::vector<core::MessageMetadata> read();

private:
  struct Entry {
    core::MessageMetadata metadata;
    core::RuntimeTimePoint occurred;
  };
  void prune(core::RuntimeTimePoint now);

  core::RuntimeClock clock_;
  std::size_t limit_;
  mutable std::mutex mutex_;
  std::deque<Entry> records_;
};
} // namespace kiarabot::infrastructure
