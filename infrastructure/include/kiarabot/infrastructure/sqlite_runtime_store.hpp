#pragma once

#include "kiarabot/core/runtime_store.hpp"

namespace kiarabot::infrastructure {
class SqliteRuntimeStore final : public core::RuntimeStore {
public:
  static core::StorageResult<std::unique_ptr<SqliteRuntimeStore>> open(const std::string& path);
  ~SqliteRuntimeStore() override;
  core::StorageResult<core::RuntimeSnapshot> read() const override;
  core::StorageResult<std::unique_ptr<core::RuntimeTransaction>> begin() override;

private:
  struct State;
  class Transaction;

public:
  explicit SqliteRuntimeStore(std::shared_ptr<State> state);

private:
  std::shared_ptr<State> state_;
};
} // namespace kiarabot::infrastructure
