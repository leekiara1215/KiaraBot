#pragma once

#include <cstddef>
#include <vector>

#include "kiarabot/core/domain/target/observation_feed.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// Commit-only final flush for one (owner-local) feed family. The caller has completed every
// business write, appends its safe projections here, and takes no further business row/quota locks
// afterwards. Rollback publishes nothing; sequences are commit-ordered by the family head lock.
core::target::Status
append_observation_feed(SqlWork& work, core::target::ObservationFamily family,
                        const std::vector<core::target::ObservationFeedAppend>& changes);
core::target::Outcome<core::target::ObservationFeedPage>
query_observation_feed(SqlWork& work, core::target::Service owner,
                       const core::target::ObservationFeedQuery& query);
core::target::Outcome<std::size_t> prune_observation_feed(SqlWork& work,
                                                          core::target::ObservationFamily family,
                                                          std::size_t limit);
// Offline restore/rejournal only: rotate one family dataset epoch so every old cursor becomes a
// typed CONFLICT/NOT_ACCEPTED/RESYNC_SNAPSHOT. Ordinary process restart never calls this.
core::target::Status rotate_observation_feed_epoch(SqlWork& work,
                                                   core::target::ObservationFamily family);

// Safe payload builders shared by the owner write paths. Callers pass only redacted metadata.
core::target::ObservationFeedAppend
make_log_feed_append(const core::target::LogObservation& log);
core::target::ObservationFeedAppend
make_task_feed_append(const core::target::TaskObservation& task, const core::target::ResourceRef& resource);
core::target::ObservationFeedAppend
make_message_feed_append(const core::target::MessageObservation& message,
                         const core::target::ResourceRef& resource);

} // namespace kiarabot::infrastructure::postgres::detail
