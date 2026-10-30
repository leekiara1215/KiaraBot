#pragma once

#include <variant>

#include "kiarabot/core/runtime_types.hpp"

namespace kiarabot::core {

// Shared result convention; Host ports specialize Error with protobuf Error.
template <typename Value, typename Error> using Result = std::variant<Value, Error>;
enum class StorageError { Unavailable, Conflict, Corrupt };
template <typename Value> using StorageResult = Result<Value, StorageError>;
using StorageStatus = StorageResult<std::monostate>;

class RuntimeTransaction {
public:
  virtual ~RuntimeTransaction() = default;
  virtual RuntimeSnapshot& snapshot() = 0;
  // Failure must leave the durable state unchanged. Destruction rolls back.
  virtual StorageStatus commit() = 0;
};

class RuntimeStore {
public:
  virtual ~RuntimeStore() = default;
  virtual StorageResult<RuntimeSnapshot> read() const = 0;
  virtual StorageResult<std::unique_ptr<RuntimeTransaction>> begin() = 0;
};

// Single-owner reference store; transaction state may outlive its originating store.
class InMemoryRuntimeStore final : public RuntimeStore {
  struct State {
    RuntimeSnapshot snapshot;
    std::uint64_t revision{0};
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
    if (state_->fail_reads) {
      return StorageError::Unavailable;
    }
    return state_->snapshot;
  }
  StorageResult<std::unique_ptr<RuntimeTransaction>> begin() override {
    if (state_->fail_reads) {
      return StorageError::Unavailable;
    }
    return std::unique_ptr<RuntimeTransaction>(std::make_unique<Transaction>(state_));
  }
  void set_fail_reads(bool value) {
    state_->fail_reads = value;
  }
  void set_fail_commits(bool value) {
    state_->fail_commits = value;
  }

private:
  std::shared_ptr<State> state_{std::make_shared<State>()};
};
} // namespace kiarabot::core
