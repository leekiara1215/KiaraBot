#pragma once

#include <utility>

#include "kiarabot/core/ports/target/host_maintenance.hpp"
#include "kiarabot/core/ports/target/host_ports.hpp"

namespace kiarabot::core::target {

class HostMaintenanceApplication {
public:
  HostMaintenanceApplication(ResourceId host, HostMaintenanceStore& store,
                             HostMaintenanceExecutionPort& executions,
                             RuntimeAdmissionGatePort& gate,
                             HostMaintenanceAuthorizationPort& authorization, HostClock& clock)
      : host_(std::move(host)), store_(store), executions_(executions), gate_(gate),
        authorization_(authorization), clock_(clock) {}
  Outcome<HostMaintenanceOperation> begin(const HostMaintenanceRequest& request);
  Outcome<HostMaintenanceOperation> resume(const HostMaintenanceRequest& request);
  Outcome<HostMaintenanceOperation> cancel(const HostMaintenanceCancelRequest& request);
  Outcome<HostMaintenanceState> state(const AuthenticationContext& context, TimePoint deadline);
  Outcome<HostMaintenanceOperation> operation(const AuthenticationContext& context,
                                              TimePoint deadline, const OperationId& operation);
  Outcome<OperationObservationPage> changes(const AuthenticationContext& context,
                                            TimePoint deadline, OperationObservationQuery query);
  // Trusted background runner. One durable phase or at most one bounded process stop per call.
  // It borrows no expired human proof and never holds a store transaction across remote work.
  Outcome<HostMaintenanceOperation> advance(const OperationId& operation);

private:
  Outcome<ResourceVersion> authorize(const AuthenticationContext&, Permission, TimePoint deadline);
  Outcome<HostMaintenanceOperation> accept(const HostMaintenanceRequest&, HostMaintenanceAction);
  Outcome<HostMaintenanceOperation> blocked(HostMaintenanceOperation value, ErrorCode reason);
  ResourceId host_;
  HostMaintenanceStore& store_;
  HostMaintenanceExecutionPort& executions_;
  RuntimeAdmissionGatePort& gate_;
  HostMaintenanceAuthorizationPort& authorization_;
  HostClock& clock_;
};
} // namespace kiarabot::core::target
