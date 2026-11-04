#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/domain/target/host_records.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/operation_observation.hpp"

namespace kiarabot::core::target {

enum class HostMaintenanceAction { Begin, Resume };
enum class HostMaintenancePhase { Accepted, Closing, GateClosed, Draining, Opening, Finished };

struct HostMaintenanceRequest {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string idempotency_key;
  std::string host_id;
  std::uint64_t expected_version = 0;
  std::uint32_t grace_ms = 0;
  TimePoint deadline;
};
struct HostMaintenanceCancelRequest {
  HostMaintenanceRequest context;
  OperationId operation;
};

// Durable authorization/audit facts only. No reusable proof, mTLS material or environment.
struct HostMaintenanceIntent {
  HostMaintenanceAction action = HostMaintenanceAction::Begin;
  SubjectId actor;
  ResourceVersion authorization_version;
  std::string request_id;
  std::string trace_id;
  std::string idempotency_key;
  std::string host_id;
  std::uint64_t expected_version = 0;
  std::uint32_t grace_ms = 10000;
};
struct HostMaintenanceTarget {
  ManagedExecution execution;
  bool stopped = false;
};
struct HostMaintenanceOperation {
  Operation operation;
  HostMaintenanceIntent intent;
  HostMaintenancePhase phase = HostMaintenancePhase::Accepted;
  std::uint64_t gate_intent_version = 0;
  std::uint64_t expected_gate_version = 0;
  std::vector<HostMaintenanceTarget> targets;
};
struct HostMaintenanceState {
  ResourceId host;
  ResourceVersion version{1};
  bool admission_open = true;
  std::optional<OperationRef> active_operation;
  std::uint64_t runtime_gate_version = 0;
  TimePoint observed_at;
};
struct HostMaintenanceUpdate {
  HostMaintenanceOperation value;
  std::uint64_t expected_operation_version = 0;
  std::optional<bool> admission_open;
  std::optional<std::uint64_t> runtime_gate_version;
};
struct HostAdmissionGateRequest {
  OperationRef operation;
  std::uint64_t maintenance_version = 0;
  std::uint64_t expected_gate_version = 0;
  bool admission_open = false;
};
struct HostAdmissionGateReceipt {
  OperationRef operation;
  std::uint64_t maintenance_version = 0;
  std::uint64_t gate_version = 0;
  bool admission_open = false;
};
inline constexpr std::size_t kHostMaintenanceExecutionLimit = 256;

} // namespace kiarabot::core::target
