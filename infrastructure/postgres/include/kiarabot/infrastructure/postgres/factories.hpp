#pragma once

#include <memory>

#include "kiarabot/core/ports/target/unit_of_work.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"

namespace kiarabot::infrastructure::postgres {

// Infrastructure composition facade. Each successful open validates exactly its private
// database/schema/roles and owns one exclusive writer session. No DDL or automatic reconnect.
// Concurrent factory begin() calls serialize for at most 250ms, then return Conflict if busy;
// the permit lasts through UoW commit/rollback/destruction. A UoW itself is single-owner.
// Session/lock loss or Unknown commit permanently disables this factory. Health sampling uses
// begin()->current_time()->rollback() on the SAME factory/session, outside application ports.
// Configured service must match the selected function; otherwise InvalidArgument.
core::target::Outcome<std::unique_ptr<core::target::IdentityUnitOfWorkFactory>>
open_identity(const DatabaseConfig& config);
core::target::Outcome<std::unique_ptr<core::target::RuntimeUnitOfWorkFactory>>
open_runtime(const DatabaseConfig& config);
core::target::Outcome<std::unique_ptr<core::target::SchedulerUnitOfWorkFactory>>
open_scheduler(const DatabaseConfig& config);
core::target::Outcome<std::unique_ptr<core::target::ModelUnitOfWorkFactory>>
open_model(const DatabaseConfig& config);

} // namespace kiarabot::infrastructure::postgres
