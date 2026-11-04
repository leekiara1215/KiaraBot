#pragma once

#include <string_view>

#include "kiarabot/core/domain/target/host_maintenance.hpp"
#include "kiarabot/core/ports/target/operation_observation_ports.hpp"

namespace kiarabot::core::target {

class HostLocalAdmissionPort {
public:
  virtual ~HostLocalAdmissionPort() = default;
  virtual Outcome<bool> allows_start() = 0;
};
class HostMaintenanceAuthorizationPort {
public:
  virtual ~HostMaintenanceAuthorizationPort() = default;
  virtual Outcome<ResourceVersion> check(const AuthenticationContext&, Permission,
                                         const ResourceRef&, TimePoint deadline) = 0;
};
class RuntimeAdmissionGatePort {
public:
  virtual ~RuntimeAdmissionGatePort() = default;
  // Current registered Host/fence and platform authorization belong to the outer backend.
  // Same operation/version/payload is idempotent even if the first reply was lost.
  virtual Outcome<HostAdmissionGateReceipt> set_admission(const HostAdmissionGateRequest&) = 0;
};
class HostMaintenanceExecutionPort {
public:
  virtual ~HostMaintenanceExecutionPort() = default;
  // Includes Exited+UNKNOWN, never infers stopped from offline or an absent PID.
  virtual Outcome<std::vector<ManagedExecution>> maintenance_executions() = 0;
  // Target must match the existing journal's instance/generation/marker; no arbitrary PID.
  virtual Outcome<ManagedExecution> stop_for_maintenance(const ManagedExecution& target,
                                                         std::uint32_t grace_ms,
                                                         std::string_view operation_id) = 0;
};
class HostMaintenanceStore : public HostLocalAdmissionPort, public OperationObservationPort {
public:
  virtual Outcome<HostMaintenanceState> maintenance_state() = 0;
  // Accept/create, replay comparison, authority state, audit and change append are one local UoW.
  virtual Outcome<HostMaintenanceOperation> accept_maintenance(const HostMaintenanceIntent&) = 0;
  virtual Outcome<std::optional<HostMaintenanceOperation>> find_maintenance(const OperationId&) = 0;
  virtual Outcome<HostMaintenanceOperation> update_maintenance(const HostMaintenanceUpdate&) = 0;
  virtual Outcome<HostMaintenanceOperation>
  cancel_maintenance(const HostMaintenanceIntent& cancel_identity, const OperationId& operation,
                     std::uint64_t expected_operation_version) = 0;
  // Trusted offline restore/rebuild only; invalidates all old observation cursors explicitly.
  virtual Status rotate_observation_epoch(std::string_view epoch) = 0;
};
} // namespace kiarabot::core::target
