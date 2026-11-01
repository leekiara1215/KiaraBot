#pragma once

#include <memory>

#include "kiarabot/core/ports/target/identity_repository.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"

namespace kiarabot::infrastructure::postgres {

// Additive Identity security facade; the P03 open_identity contract remains unchanged.
// Opens/validates exactly one Identity writer session, with no DDL or reconnect. All inherited
// and security repositories borrow one physical transaction and the same failure/deadline state.
// begin waits at most 250ms; busy -> Conflict. Each UoW is single-owner, may outlive its factory,
// and releases the permit on terminal commit/rollback/destruction. Lost connection/Unknown
// permanently disables the old writer. Health sampling uses this factory's own begin/current_time.
// A non-Identity config is InvalidArgument. Storage/schema errors preserve the existing Failure.
core::target::Outcome<std::unique_ptr<core::target::IdentitySecurityUnitOfWorkFactory>>
open_identity_security(const DatabaseConfig& config);

} // namespace kiarabot::infrastructure::postgres
