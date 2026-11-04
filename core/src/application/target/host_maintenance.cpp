#include "kiarabot/core/application/target/host_maintenance.hpp"

#include <algorithm>
#include <set>
#include <string_view>

namespace kiarabot::core::target {
namespace {
Failure failed(ErrorCode code, Recovery recovery = Recovery::ReadCurrent) {
  return {code, Acceptance::NotAccepted, recovery, {}, {}};
}
bool identifier(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return character > 32 && character < 127; });
}
bool terminal(const Operation& operation) {
  return operation.state == OperationState::Succeeded ||
         operation.state == OperationState::Failed ||
         operation.state == OperationState::Cancelled || operation.state == OperationState::Expired;
}
bool known_stopped(const ManagedExecution& execution) {
  return execution.state == ManagedExecutionState::Exited &&
         (execution.outcome == ExecutionOutcome::KnownSucceeded ||
          execution.outcome == ExecutionOutcome::KnownFailed);
}
void running(HostMaintenanceOperation& value) {
  value.operation.state = OperationState::Running;
  value.operation.execution_outcome = ExecutionOutcome::Unknown;
  value.operation.blocked_reason.reset();
  value.operation.next_attempt.reset();
}
Outcome<HostMaintenanceOperation> save_progress(HostMaintenanceStore& store,
                                                HostMaintenanceUpdate update) {
  const auto reference = update.value.operation.ref;
  auto result = store.update_maintenance(update);
  if (auto* rejected = std::get_if<Failure>(&result)) {
    // The original intent is already durable and a preceding port call may have completed.
    // Even a known rollback of THIS progress write cannot prove its external effect absent.
    rejected->acceptance = Acceptance::Unknown;
    rejected->recovery = Recovery::QuerySameIdentity;
    rejected->operation = reference;
  }
  return result;
}
void succeeded(HostMaintenanceOperation& value) {
  value.phase = HostMaintenancePhase::Finished;
  value.operation.state = OperationState::Succeeded;
  value.operation.execution_outcome = ExecutionOutcome::KnownSucceeded;
  value.operation.blocked_reason.reset();
  value.operation.next_attempt.reset();
  value.operation.participants = {{"runtime_gate", OperationState::Succeeded, {}, {}},
                                  {"managed_executions", OperationState::Succeeded, {}, {}}};
}
} // namespace

Outcome<ResourceVersion> HostMaintenanceApplication::authorize(const AuthenticationContext& context,
                                                               Permission permission,
                                                               TimePoint deadline) {
  const auto now = clock_.now();
  if (now.time_since_epoch().count() <= 0) {
    return failed(ErrorCode::Internal);
  }
  const auto* peer = std::get_if<PlatformPeer>(&context.direct_peer);
  if (!peer || peer->service != Service::Control || context.audience != Service::Host ||
      !identifier(context.subject.value) || context.authorization_version.value == 0 ||
      context.verified_at.time_since_epoch().count() <= 0 || context.verified_at > now ||
      context.expires_at <= now) {
    return failed(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  if (deadline <= now) {
    return failed(ErrorCode::Timeout);
  }
  auto result = authorization_.check(context, permission, {ResourceKind::Host, host_}, deadline);
  if (const auto* version = std::get_if<ResourceVersion>(&result); version && version->value == 0) {
    return failed(ErrorCode::Internal);
  }
  return result;
}

Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::accept(const HostMaintenanceRequest& request,
                                   HostMaintenanceAction action) {
  if (request.host_id != host_.value || !identifier(request.request_id) ||
      !identifier(request.trace_id) || request.idempotency_key.size() < 16 ||
      request.idempotency_key.size() > 128 || request.expected_version == 0 ||
      request.grace_ms > 10000) {
    return failed(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  auto checked = authorize(request.authentication, Permission::HostExecute, request.deadline);
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  // Store performs replay lookup first, then current-state acceptance. In particular a replayed
  // Resume must not be rejected merely because its original effect already reopened admission.
  HostMaintenanceIntent intent{action,
                               request.authentication.subject,
                               std::get<ResourceVersion>(checked),
                               request.request_id,
                               request.trace_id,
                               request.idempotency_key,
                               host_.value,
                               request.expected_version,
                               request.grace_ms == 0 ? 10000U : request.grace_ms};
  return store_.accept_maintenance(intent);
}
Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::begin(const HostMaintenanceRequest& request) {
  return accept(request, HostMaintenanceAction::Begin);
}
Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::resume(const HostMaintenanceRequest& request) {
  return accept(request, HostMaintenanceAction::Resume);
}
Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::cancel(const HostMaintenanceCancelRequest& request) {
  const auto& context = request.context;
  if (context.host_id != host_.value || !identifier(request.operation.value) ||
      !identifier(context.request_id) || !identifier(context.trace_id) ||
      context.idempotency_key.size() < 16 || context.idempotency_key.size() > 128 ||
      context.expected_version == 0) {
    return failed(ErrorCode::InvalidArgument);
  }
  auto checked = authorize(context.authentication, Permission::HostExecute, context.deadline);
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  HostMaintenanceIntent identity{HostMaintenanceAction::Begin,
                                 context.authentication.subject,
                                 std::get<ResourceVersion>(checked),
                                 context.request_id,
                                 context.trace_id,
                                 context.idempotency_key,
                                 host_.value,
                                 context.expected_version,
                                 0};
  return store_.cancel_maintenance(identity, request.operation, context.expected_version);
}
Outcome<HostMaintenanceState>
HostMaintenanceApplication::state(const AuthenticationContext& context, TimePoint deadline) {
  auto checked = authorize(context, Permission::HostRead, deadline);
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  return store_.maintenance_state();
}
Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::operation(const AuthenticationContext& context, TimePoint deadline,
                                      const OperationId& operation) {
  if (!identifier(operation.value)) {
    return failed(ErrorCode::InvalidArgument);
  }
  auto checked = authorize(context, Permission::HostRead, deadline);
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  auto found = store_.find_maintenance(operation);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  const auto& value = std::get<std::optional<HostMaintenanceOperation>>(found);
  if (!value || value->operation.ref.resource != ResourceRef{ResourceKind::Host, host_}) {
    // Stored resource identity decides visibility; a caller relabel of the wire host is not a
    // cross-host disclosure and never becomes success.
    return failed(ErrorCode::NotFound);
  }
  return *value;
}
Outcome<OperationObservationPage>
HostMaintenanceApplication::changes(const AuthenticationContext& context, TimePoint deadline,
                                    OperationObservationQuery query) {
  if (query.scope != std::vector<ResourceRef>{{ResourceKind::Host, host_}}) {
    return failed(ErrorCode::InvalidArgument);
  }
  auto checked = authorize(context, Permission::HostRead, deadline);
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  query.deadline = std::min(
      query.deadline,
      std::chrono::steady_clock::now() +
          std::min(std::chrono::milliseconds{500},
                   std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_.now())));
  return store_.query_operation_changes(query);
}
Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::blocked(HostMaintenanceOperation value, ErrorCode reason) {
  const auto expected = value.operation.version.value;
  value.operation.state = OperationState::Blocked;
  value.operation.execution_outcome = ExecutionOutcome::Unknown;
  value.operation.blocked_reason = reason;
  value.operation.next_attempt = clock_.now() + std::chrono::seconds{1};
  const bool execution_blocked = value.phase == HostMaintenancePhase::Draining ||
                                 (value.intent.action == HostMaintenanceAction::Resume &&
                                  value.phase == HostMaintenancePhase::Accepted);
  value.operation.participants = {{execution_blocked ? "managed_executions" : "runtime_gate",
                                   OperationState::Blocked, reason, value.operation.next_attempt}};
  if (value.phase == HostMaintenancePhase::Draining) {
    value.operation.participants.push_back({"runtime_gate", OperationState::Succeeded, {}, {}});
  }
  return save_progress(store_, {std::move(value), expected, {}, {}});
}

Outcome<HostMaintenanceOperation>
HostMaintenanceApplication::advance(const OperationId& operation) {
  auto found = store_.find_maintenance(operation);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  auto current = std::move(std::get<std::optional<HostMaintenanceOperation>>(found));
  if (!current) {
    return failed(ErrorCode::NotFound);
  }
  auto value = std::move(*current);
  if (terminal(value.operation)) {
    return value;
  }
  const auto revision = value.operation.version.value;
  if (value.phase == HostMaintenancePhase::Accepted) {
    running(value);
    if (value.intent.action == HostMaintenanceAction::Begin) {
      value.phase = HostMaintenancePhase::Closing;
      value.operation.participants = {{"runtime_gate", OperationState::Running, {}, {}},
                                      {"managed_executions", OperationState::Accepted, {}, {}}};
      return save_progress(store_, {std::move(value), revision, false, {}});
    }
    auto active = executions_.maintenance_executions();
    if (const auto* failure = std::get_if<Failure>(&active)) {
      return blocked(std::move(value), failure->code);
    }
    if (!std::get<std::vector<ManagedExecution>>(active).empty()) {
      return blocked(std::move(value), ErrorCode::Conflict);
    }
    value.phase = HostMaintenancePhase::Opening;
    value.operation.participants = {{"runtime_gate", OperationState::Running, {}, {}},
                                    {"managed_executions", OperationState::Succeeded, {}, {}}};
    return save_progress(store_, {std::move(value), revision, {}, {}});
  }
  if (value.phase == HostMaintenancePhase::Closing ||
      value.phase == HostMaintenancePhase::Opening) {
    const bool opening = value.phase == HostMaintenancePhase::Opening;
    if (opening) {
      auto active = executions_.maintenance_executions();
      if (const auto* failure = std::get_if<Failure>(&active)) {
        return blocked(std::move(value), failure->code);
      }
      if (!std::get<std::vector<ManagedExecution>>(active).empty()) {
        return blocked(std::move(value), ErrorCode::Conflict);
      }
    }
    HostAdmissionGateRequest request{value.operation.ref, value.gate_intent_version,
                                     value.expected_gate_version, opening};
    auto changed = gate_.set_admission(request);
    if (const auto* failure = std::get_if<Failure>(&changed)) {
      return blocked(std::move(value), failure->code);
    }
    const auto& receipt = std::get<HostAdmissionGateReceipt>(changed);
    if (receipt.operation != request.operation ||
        receipt.maintenance_version != request.maintenance_version ||
        receipt.admission_open != opening ||
        receipt.gate_version <= request.expected_gate_version) {
      return blocked(std::move(value), ErrorCode::Internal);
    }
    running(value);
    if (opening) {
      succeeded(value);
    } else {
      value.phase = HostMaintenancePhase::GateClosed;
      value.operation.participants = {{"runtime_gate", OperationState::Succeeded, {}, {}},
                                      {"managed_executions", OperationState::Accepted, {}, {}}};
    }
    return save_progress(store_, {std::move(value), revision,
                                  opening ? std::optional<bool>{true} : std::nullopt,
                                  receipt.gate_version});
  }
  if (value.phase == HostMaintenancePhase::GateClosed) {
    auto active = executions_.maintenance_executions();
    if (const auto* failure = std::get_if<Failure>(&active)) {
      return blocked(std::move(value), failure->code);
    }
    const auto& executions = std::get<std::vector<ManagedExecution>>(active);
    if (executions.size() > kHostMaintenanceExecutionLimit) {
      return blocked(std::move(value), ErrorCode::RateLimited);
    }
    std::set<std::pair<std::string, std::uint64_t>> identities;
    for (const auto& execution : executions) {
      if (execution.host_id != host_.value || execution.instance_id.empty() ||
          execution.execution_generation == 0 ||
          !identities.emplace(execution.instance_id, execution.execution_generation).second) {
        return blocked(std::move(value), ErrorCode::Internal);
      }
      value.targets.push_back({execution, known_stopped(execution)});
    }
    running(value);
    value.phase = HostMaintenancePhase::Draining;
    value.operation.participants = {{"runtime_gate", OperationState::Succeeded, {}, {}},
                                    {"managed_executions", OperationState::Running, {}, {}}};
    return save_progress(store_, {std::move(value), revision, {}, {}});
  }
  if (value.phase == HostMaintenancePhase::Draining) {
    auto target = std::find_if(value.targets.begin(), value.targets.end(),
                               [](const auto& item) { return !item.stopped; });
    if (target != value.targets.end()) {
      auto stopped = executions_.stop_for_maintenance(target->execution, value.intent.grace_ms,
                                                      value.operation.ref.id.value);
      if (const auto* failure = std::get_if<Failure>(&stopped)) {
        return blocked(std::move(value), failure->code);
      }
      const auto& evidence = std::get<ManagedExecution>(stopped);
      if (evidence.host_id != host_.value ||
          evidence.instance_id != target->execution.instance_id ||
          evidence.execution_generation != target->execution.execution_generation ||
          evidence.process_marker != target->execution.process_marker ||
          evidence.pid != target->execution.pid || !known_stopped(evidence)) {
        return blocked(std::move(value), ErrorCode::UpstreamUnavailable);
      }
      target->execution = evidence;
      target->stopped = true;
    }
    running(value);
    value.operation.participants = {{"runtime_gate", OperationState::Succeeded, {}, {}},
                                    {"managed_executions", OperationState::Running, {}, {}}};
    if (std::all_of(value.targets.begin(), value.targets.end(),
                    [](const auto& item) { return item.stopped; })) {
      succeeded(value);
    }
    return save_progress(store_, {std::move(value), revision, {}, {}});
  }
  return failed(ErrorCode::Internal);
}
} // namespace kiarabot::core::target
