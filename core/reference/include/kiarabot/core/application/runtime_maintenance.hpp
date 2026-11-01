#pragma once

#include "kiarabot/core/domain/runtime_types.hpp"
#include "kiarabot/core/ports/runtime_store.hpp"

namespace kiarabot::core {
// Retention and capacity enforcement for one RuntimeSnapshot. The host runs this
// from a maintenance loop; it is safe to call repeatedly and never retries a
// storage conflict on its own. Every removed record is counted so the host can
// log the effect instead of silently shrinking the snapshot.
struct MaintenanceOptions {
  std::chrono::hours retention{168};
  // Hard cap used by AuthorizedCommands; the maintenance trims to a lower
  // watermark so record writers always have headroom.
  std::size_t audit_limit{100000};
  std::size_t command_limit{100000};
};

struct MaintenanceStats {
  std::size_t events{0};
  std::size_t deliveries{0};
  std::size_t actions{0};
  std::size_t audit{0};
  std::size_t commands{0};
  std::size_t namespace_entries{0};
  std::size_t host_receipts{0};
  std::size_t daily_budgets{0};

  [[nodiscard]] bool changed() const noexcept {
    return events != 0 || deliveries != 0 || actions != 0 || audit != 0 || commands != 0 ||
           namespace_entries != 0 || host_receipts != 0 || daily_budgets != 0;
  }
};

// Applies terminal-record retention plus bounded audit, command, namespace-TTL
// and Host-receipt cleanup in a single transaction. A failed commit removes
// nothing; the caller keeps the previous durable state and may retry later.
class RuntimeMaintenance {
public:
  RuntimeMaintenance(RuntimeStore& store, MaintenanceOptions options = {})
      : store_(&store), options_(options) {}

  // Empty now means the system clock. Caller must not hold another transaction.
  StorageResult<MaintenanceStats> run(RuntimeTimePoint now = {});

private:
  RuntimeStore* store_;
  MaintenanceOptions options_;
};
} // namespace kiarabot::core
