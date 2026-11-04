#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/observation_feed.hpp"

namespace kiarabot::core::target {

// Read-only bounded query over one (owner, family) journal. Query on a fresh UoW; missing cursor
// captures the committed head and returns no changes. A gap/epoch mismatch returns
// Conflict/ReadCurrent, never a successful empty page.
class ObservationFeedPort {
public:
  virtual ~ObservationFeedPort() = default;
  virtual Outcome<ObservationFeedPage>
  query_observation_changes(const ObservationFeedQuery& query) = 0;
  // Trusted bounded maintenance, 1..1000; authoritative DB UTC, never caller time.
  virtual Outcome<std::size_t> prune_observation_changes(std::size_t limit) = 0;
};

} // namespace kiarabot::core::target
