#pragma once

#include <mutex>

#include "kiarabot/core/ports/runtime_store.hpp"

namespace kiarabot::core {

// Reference store for tests and local development. Independent transactions are
// isolated by revision; a conflicting commit fails instead of overwriting.
class InMemoryRuntimeStore final : public RuntimeStore {
  struct State {
    mutable std::mutex mutex;
    RuntimeSnapshot snapshot;
    std::uint64_t revision{0};
    std::size_t fail_conflicts{0};
    bool fail_reads{false};
    bool fail_commits{false};
  };
  class Transaction final : public RuntimeTransaction {
  public:
    explicit Transaction(std::shared_ptr<State> state)
        : state_(std::move(state)), snapshot_(state_->snapshot), revision_(state_->revision) {}
    RuntimeSnapshot& snapshot() override {
      return snapshot_;
    }
    StorageStatus commit() override {
      std::lock_guard lock(state_->mutex);
      if (state_->fail_conflicts > 0) {
        --state_->fail_conflicts;
        return StorageError::Conflict;
      }
      if (finished_ || state_->revision != revision_ ||
          revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return StorageError::Conflict;
      }
      if (state_->fail_commits) {
        return StorageError::Unavailable;
      }
      auto replacement = snapshot_;
      std::swap(state_->snapshot, replacement);
      ++state_->revision;
      finished_ = true;
      return std::monostate{};
    }

  private:
    std::shared_ptr<State> state_;
    RuntimeSnapshot snapshot_;
    std::uint64_t revision_;
    bool finished_{false};
  };

public:
  StorageResult<RuntimeSnapshot> read() const override {
    std::lock_guard lock(state_->mutex);
    if (state_->fail_reads) {
      return StorageError::Unavailable;
    }
    return state_->snapshot;
  }
  StorageResult<std::unique_ptr<RuntimeTransaction>> begin() override {
    std::lock_guard lock(state_->mutex);
    if (state_->fail_reads) {
      return StorageError::Unavailable;
    }
    return std::unique_ptr<RuntimeTransaction>(std::make_unique<Transaction>(state_));
  }
  void set_fail_reads(bool value) {
    std::lock_guard lock(state_->mutex);
    state_->fail_reads = value;
  }
  void set_fail_commits(bool value) {
    std::lock_guard lock(state_->mutex);
    state_->fail_commits = value;
  }
  // Injects a bounded number of optimistic-locking conflicts for retry tests.
  void set_fail_conflicts(std::size_t count) {
    std::lock_guard lock(state_->mutex);
    state_->fail_conflicts = count;
  }

private:
  std::shared_ptr<State> state_{std::make_shared<State>()};
};
} // namespace kiarabot::core
