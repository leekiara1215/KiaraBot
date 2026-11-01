#pragma once

#include "kiarabot/core/application/plugin_hub.hpp"

namespace kiarabot::core {
// One scheduling owner per database. All public mutations use optimistic transactions.
class TaskScheduler final : public IScheduler {
public:
  TaskScheduler(RuntimeStore& store, std::shared_ptr<ITimeZoneResolver> zones,
                RuntimeClock clock = {}, RuntimeLimits limits = {});
  kiarabot::v1::HostResponse dispatch(const PluginSession& session,
                                      const kiarabot::v1::HostRequest& request);
  PortResult<kiarabot::v1::TaskResponse>
  create(std::string_view owner, const kiarabot::v1::TaskCreateRequest& request) override;
  PortResult<kiarabot::v1::TaskResponse> get(std::string_view owner,
                                             std::string_view task_id) override;
  PortResult<kiarabot::v1::TaskResponse> cancel(std::string_view owner,
                                                std::string_view task_id) override;
  PortResult<std::vector<TaskClaim>> claim_due(WallTime now, std::size_t limit) override;
  PortStatus finish(std::string_view owner, std::string_view task_id, std::string_view claim_token,
                    bool succeeded, std::optional<WallTime> retry_at) override;
  PortStatus recover() override;
  PortStatus tick();

private:
  RuntimeTimePoint now() const;
  PortResult<kiarabot::v1::TaskResponse> create_in(RuntimeSnapshot& snapshot,
                                                   std::string_view owner,
                                                   const kiarabot::v1::TaskCreateRequest& request);
  RuntimeStore& store_;
  std::shared_ptr<ITimeZoneResolver> zones_;
  RuntimeClock clock_;
  RuntimeLimits limits_;
  std::mutex mutex_;
};
} // namespace kiarabot::core
