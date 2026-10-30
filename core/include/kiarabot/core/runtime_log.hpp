#pragma once

#include <atomic>

#include "kiarabot/core/runtime_store.hpp"

namespace kiarabot::core {
struct RuntimeLog {
  std::string id;
  RuntimeTimePoint occurred_at{};
  std::string level;
  std::string component;
  std::string operation;
  std::string outcome;
  std::string resource;
  std::string request_id;
  std::string trace_id;
};
class IRuntimeLogSink {
public:
  virtual ~IRuntimeLogSink() = default;
  virtual bool write(const RuntimeLog& record) noexcept = 0;
};
// Observes transaction completion, never controls business decisions or retries.
class LoggingRuntimeStore final : public RuntimeStore {
public:
  LoggingRuntimeStore(std::unique_ptr<RuntimeStore> store, std::shared_ptr<IRuntimeLogSink> sink,
                      RuntimeClock clock = {});
  StorageResult<RuntimeSnapshot> read() const override;
  StorageResult<std::unique_ptr<RuntimeTransaction>> begin() override;
  std::uint64_t sink_failures() const;

private:
  struct State;
  class Transaction;
  std::unique_ptr<RuntimeStore> store_;
  std::shared_ptr<State> state_;
};
} // namespace kiarabot::core
