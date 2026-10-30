#pragma once

#include <variant>

#include "kiarabot/core/domain/runtime_types.hpp"

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

} // namespace kiarabot::core
