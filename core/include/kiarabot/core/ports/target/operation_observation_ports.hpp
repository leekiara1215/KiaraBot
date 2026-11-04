#pragma once

#include <cstddef>

#include "kiarabot/core/domain/target/operation_observation.hpp"

namespace kiarabot::core::target {

// Borrows a single owner-local UoW. No wire/SQL types or implicit authorization.
// Query on a fresh UoW and finish it before business mutations or remote calls.
// Missing cursor captures committed head; consume a complete page before advancing
// next_cursor. A gap/epoch mismatch returns Conflict/ReadCurrent, never empty success.
// Invalid shape is InvalidArgument, unavailable storage is a typed dependency failure.
class OperationObservationPort {
public:
  virtual ~OperationObservationPort() = default;
  virtual Outcome<OperationObservationPage>
  query_operation_changes(const OperationObservationQuery& query) = 0;
  // Trusted bounded maintenance, 1..1000; authoritative DB UTC, never caller time.
  virtual Outcome<std::size_t> prune_operation_changes(std::size_t limit) = 0;
};

} // namespace kiarabot::core::target
