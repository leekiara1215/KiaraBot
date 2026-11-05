#pragma once

#include <string>
#include <utility>

#include "kiarabot/core/application/target/host_maintenance.hpp"

namespace kiarabot::core::target {

// HOST-04 typed application boundary for the HostManagement owner RPC. The transport adapter
// converts verified peer + wire projection into these calls only; authorization, operation state
// and the durable maintenance truth stay in HostMaintenanceApplication/SQLite.
class HostManagementApplicationPort {
public:
  virtual ~HostManagementApplicationPort() = default;
  // This Host's own owner identity. Wire host labels never decide stored-resource visibility.
  virtual const ResourceId& host_id() const noexcept = 0;
  virtual Outcome<HostMaintenanceState> maintenance_state(const AuthenticationContext& context,
                                                          TimePoint deadline) = 0;
  // Projection for a mutation response already authorized with host.execute on this exact Host.
  virtual Outcome<HostMaintenanceState> state_projection() = 0;
  virtual Outcome<HostMaintenanceOperation> begin(const HostMaintenanceRequest& request) = 0;
  virtual Outcome<HostMaintenanceOperation> resume(const HostMaintenanceRequest& request) = 0;
  virtual Outcome<HostMaintenanceOperation> cancel(const HostMaintenanceCancelRequest& request) = 0;
  virtual Outcome<HostMaintenanceOperation> operation(const AuthenticationContext& context,
                                                      TimePoint deadline,
                                                      const OperationId& operation) = 0;
  virtual Outcome<OperationObservationPage> changes(const AuthenticationContext& context,
                                                    TimePoint deadline,
                                                    OperationObservationQuery query) = 0;
};

// Production binding: no business logic of its own, only delegation to the HOST-02 application.
class HostMaintenanceApplicationAdapter final : public HostManagementApplicationPort {
public:
  explicit HostMaintenanceApplicationAdapter(HostMaintenanceApplication& application)
      : application_(application) {}
  const ResourceId& host_id() const noexcept override {
    return application_.host_id();
  }
  Outcome<HostMaintenanceState> maintenance_state(const AuthenticationContext& context,
                                                  TimePoint deadline) override {
    return application_.state(context, deadline);
  }
  Outcome<HostMaintenanceState> state_projection() override {
    return application_.state_projection();
  }
  Outcome<HostMaintenanceOperation> begin(const HostMaintenanceRequest& request) override {
    return application_.begin(request);
  }
  Outcome<HostMaintenanceOperation> resume(const HostMaintenanceRequest& request) override {
    return application_.resume(request);
  }
  Outcome<HostMaintenanceOperation> cancel(const HostMaintenanceCancelRequest& request) override {
    return application_.cancel(request);
  }
  Outcome<HostMaintenanceOperation> operation(const AuthenticationContext& context,
                                              TimePoint deadline,
                                              const OperationId& operation) override {
    return application_.operation(context, deadline, operation);
  }
  Outcome<OperationObservationPage> changes(const AuthenticationContext& context,
                                            TimePoint deadline,
                                            OperationObservationQuery query) override {
    return application_.changes(context, deadline, std::move(query));
  }

private:
  HostMaintenanceApplication& application_;
};

} // namespace kiarabot::core::target
