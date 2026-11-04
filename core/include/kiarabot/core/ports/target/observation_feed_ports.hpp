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
  // Read-only bounded query over one (owner, family) journal. Missing cursor captures the
  // committed head; a gap/epoch mismatch is Conflict/ReadCurrent, never an empty success.
  virtual Outcome<ObservationFeedPage>
  query_observation_changes(const ObservationFeedQuery& query) = 0;
  // Trusted bounded maintenance, 1..1000 per call; authoritative DB UTC, never caller time.
  virtual Outcome<std::size_t> prune_observation_changes(ObservationFamily family,
                                                         std::size_t limit) = 0;
  // Same-UoW owner feed append: the business mutation and this journal row commit atomically.
  virtual Status append_observation_changes(ObservationFamily family,
                                            const std::vector<ObservationFeedAppend>& changes) = 0;
};

} // namespace kiarabot::core::target
