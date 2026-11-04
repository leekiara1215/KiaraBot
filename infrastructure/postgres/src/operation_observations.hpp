#pragma once

#include <set>
#include <string>

#include "kiarabot/core/domain/target/operation_observation.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// Commit-only final flush: caller has completed every business write and takes no
// business row/quota locks afterward. This function never commits or retries.
core::target::Status flush_operation_observations(SqlWork& work, core::target::Service owner,
                                                  const std::set<std::string>& operation_ids);
core::target::Outcome<core::target::OperationObservationPage>
query_operation_observations(SqlWork& work, core::target::Service owner,
                             const core::target::OperationObservationQuery& query);
core::target::Outcome<std::size_t> prune_operation_observations(SqlWork& work, std::size_t limit);

} // namespace kiarabot::infrastructure::postgres::detail
