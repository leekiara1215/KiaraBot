#pragma once

#include <memory>

#include "kiarabot/core/ports/runtime_store.hpp"
#include "kiarabot/infrastructure/snapshot_limits.hpp"

namespace kiarabot::infrastructure {
class SqliteRuntimeStore final : public core::RuntimeStore {
public:
  // Capacity limits of the private snapshot format. An oversized commit payload
  // returns StorageError::Unavailable (durable state unchanged, the transaction
  // may be retried after shrinking); an oversized stored payload returns
  // StorageError::Corrupt on read/open.
  static SnapshotLimits limits();
  static core::StorageResult<std::unique_ptr<SqliteRuntimeStore>> open(const std::string& path);
  ~SqliteRuntimeStore() override;
  core::StorageResult<core::RuntimeSnapshot> read() const override;
  core::StorageResult<std::unique_ptr<core::RuntimeTransaction>> begin() override;
  // Monotonic per-instance rejection counters for health/metrics reporting.
  SnapshotRejections rejected_snapshots() const;

private:
  struct State;
  class Transaction;

public:
  explicit SqliteRuntimeStore(std::shared_ptr<State> state);

private:
  std::shared_ptr<State> state_;
};
} // namespace kiarabot::infrastructure
