#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kiarabot/core/runtime.h"
#include "kiarabot/v1/plugins.pb.h"

namespace kiarabot::core {

// Error is the protocol's canonical error; no exception crosses this interface.
template <typename Value> using PortResult = Result<Value, kiarabot::v1::Error>;
using PortStatus = PortResult<std::monostate>;
using WallTime = std::chrono::system_clock::time_point;

class INamespaceStore {
public:
  virtual ~INamespaceStore() = default;
  virtual PortResult<kiarabot::v1::StorageGetResponse>
  get(std::string_view plugin_id, const kiarabot::v1::StorageGetRequest& request) = 0;
  virtual PortStatus set(std::string_view plugin_id,
                         const kiarabot::v1::StorageSetRequest& request) = 0;
  virtual PortStatus erase(std::string_view plugin_id,
                           const kiarabot::v1::StorageDeleteRequest& request) = 0;
};

struct IdempotencyRecord {
  std::string request_fingerprint;
  kiarabot::v1::HostResponse response;
  WallTime expires_at{};
};

class IIdempotencyStore {
public:
  virtual ~IIdempotencyStore() = default;
  virtual PortResult<std::optional<IdempotencyRecord>> find(std::string_view plugin_id,
                                                            std::string_view key) = 0;
  virtual PortStatus insert(std::string_view plugin_id, std::string_view key,
                            const IdempotencyRecord& record) = 0;
};

class IAuditLogger;
class IScheduler;

class IUnitOfWork {
public:
  virtual ~IUnitOfWork() = default;
  // References are valid only while this unit of work is alive.
  virtual RuntimeSnapshot& runtime_snapshot() = 0;
  virtual INamespaceStore& namespace_store() = 0;
  virtual IAuditLogger& audit_logger() = 0;
  virtual IIdempotencyStore& idempotency_store() = 0;
  virtual IScheduler& scheduler() = 0;
  virtual PortStatus commit() = 0;
  virtual PortStatus rollback() = 0;
};

class IUnitOfWorkFactory {
public:
  virtual ~IUnitOfWorkFactory() = default;
  virtual PortResult<std::unique_ptr<IUnitOfWork>> begin() = 0;
};

class IModelProvider {
public:
  virtual ~IModelProvider() = default;
  virtual PortResult<kiarabot::v1::ModelChatResponse>
  chat(const kiarabot::v1::ModelChatRequest& request, std::chrono::milliseconds timeout) = 0;
};

struct ScheduledTask {
  std::string owner_plugin_id;
  kiarabot::v1::TaskCreateRequest request;
  kiarabot::v1::ScheduledTaskState state{kiarabot::v1::SCHEDULED_TASK_STATE_UNSPECIFIED};
  WallTime next_run_at{};
  std::uint32_t attempts{0};
  bool cancel_requested{false};
};

class ITimeZoneResolver {
public:
  virtual ~ITimeZoneResolver() = default;
  virtual PortResult<WallTime> resolve(const kiarabot::v1::LocalTaskTime& local) = 0;
};

struct TaskClaim {
  ScheduledTask task;
  std::string claim_token;
};

class IScheduler {
public:
  virtual ~IScheduler() = default;
  virtual PortResult<kiarabot::v1::TaskResponse>
  create(std::string_view plugin_id, const kiarabot::v1::TaskCreateRequest& request) = 0;
  virtual PortResult<kiarabot::v1::TaskResponse> get(std::string_view plugin_id,
                                                     std::string_view task_id) = 0;
  virtual PortResult<kiarabot::v1::TaskResponse> cancel(std::string_view plugin_id,
                                                        std::string_view task_id) = 0;
  virtual PortResult<std::vector<TaskClaim>> claim_due(WallTime now, std::size_t limit) = 0;
  virtual PortStatus finish(std::string_view plugin_id, std::string_view task_id,
                            std::string_view claim_token, bool succeeded,
                            std::optional<WallTime> retry_at) = 0;
  virtual PortStatus recover() = 0;
};

struct AuditRecord {
  std::string id;
  WallTime occurred_at{};
  std::string actor;
  std::string action;
  std::string resource;
  std::string request_id;
  std::string trace_id;
  kiarabot::v1::ErrorCode outcome{kiarabot::v1::ERROR_CODE_UNSPECIFIED};
};

class IAuditLogger {
public:
  virtual ~IAuditLogger() = default;
  virtual PortStatus append(const AuditRecord& record) = 0;
};

class IFileStorage {
public:
  virtual ~IFileStorage() = default;
  virtual PortResult<std::vector<std::byte>> get(std::string_view key) = 0;
  virtual PortStatus put(std::string_view key, const std::vector<std::byte>& bytes) = 0;
  virtual PortStatus erase(std::string_view key) = 0;
};

} // namespace kiarabot::core
