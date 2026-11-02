#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/invariants.hpp"

namespace kiarabot::core::target {

struct ActionTag;
using ActionId = Identifier<ActionTag>;

// P07 target Action rules shared by the Runtime application and the PG adapter. No protocol,
// SQL or host clock appears here: callers inject authoritative database UTC, trusted deployment
// limits and the persisted Action row. The wire payload remains the discriminator; these rules
// own the per-kind permission/capability mapping, admission checks, conflict-free transitions and
// the status x execution-knownness matrix.
inline constexpr auto kActionDeadlineMaximum = std::chrono::seconds{300};
inline constexpr auto kActionTerminalRetention = std::chrono::hours{168};
inline constexpr auto kActionEvidenceRetention = std::chrono::hours{168};
inline constexpr std::size_t kMaximumSerializedEnvelope = 65536;
inline constexpr std::uint32_t kMaximumActionDispatchSequence = 100;
enum class ActionState : std::uint8_t {
  Accepted = 0,
  Succeeded = 1,
  Failed = 2,
  Expired = 3,
  Cancelled = 4
};
enum class ActionDispatchState : std::uint8_t { NotDispatched = 0, Dispatched = 1 };
// Matches kiarabot.action_policies and the frozen storage contract: 0..3.
enum class ActionEvidenceKind : std::uint8_t {
  Unknown = 0,
  NotStarted = 1,
  LateResult = 2,
  Conflict = 3
};
// Wire ActionKind values; schedule_task is not a target kind and is rejected before admission.
enum class ActionKind : std::uint8_t {
  SendMessage = 1,
  ReactMessage = 2,
  DeleteMessage = 3,
  MuteMember = 4,
  KickMember = 5,
  ApproveRequest = 6
};
struct ActionKindInfo {
  ActionKind kind;
  Permission permission;
  std::string_view capability;
  std::string_view wire_payload;
};
inline Outcome<ActionKindInfo> action_kind_from_payload(std::string_view payload) {
  constexpr std::array kKinds{ActionKindInfo{ActionKind::SendMessage, Permission::MessageSend,
                                             "action.send_message.text.v1", "send_message"},
                              ActionKindInfo{ActionKind::ReactMessage, Permission::MessageReact,
                                             "action.react_message.v1", "react_message"},
                              ActionKindInfo{ActionKind::DeleteMessage, Permission::MessageDelete,
                                             "action.delete_message.v1", "delete_message"},
                              ActionKindInfo{ActionKind::MuteMember, Permission::MemberMute,
                                             "action.mute_member.v1", "mute_member"},
                              ActionKindInfo{ActionKind::KickMember, Permission::MemberKick,
                                             "action.kick_member.v1", "kick_member"},
                              ActionKindInfo{ActionKind::ApproveRequest, Permission::RequestApprove,
                                             "action.approve_request.v1", "approve_request"}};
  for (const auto& info : kKinds) {
    if (info.wire_payload == payload) {
      return info;
    }
  }
  // Legacy scheduling was replaced by host.tasks.v1; it is not an Action admission path.
  return rejected(ErrorCode::CapabilityNotSupported);
}
inline bool known_action_kind(ActionKind kind) {
  return kind >= ActionKind::SendMessage && kind <= ActionKind::ApproveRequest;
}
inline std::string_view action_capability(ActionKind kind) noexcept {
  constexpr std::array kCapabilities{std::string_view{"action.send_message.text.v1"},
                                     std::string_view{"action.react_message.v1"},
                                     std::string_view{"action.delete_message.v1"},
                                     std::string_view{"action.mute_member.v1"},
                                     std::string_view{"action.kick_member.v1"},
                                     std::string_view{"action.approve_request.v1"}};
  const auto index = static_cast<std::size_t>(kind) - 1;
  return index < kCapabilities.size() ? kCapabilities[index] : std::string_view{};
}
inline std::optional<Permission> action_permission(ActionKind kind) noexcept {
  switch (kind) {
  case ActionKind::SendMessage:
    return Permission::MessageSend;
  case ActionKind::ReactMessage:
    return Permission::MessageReact;
  case ActionKind::DeleteMessage:
    return Permission::MessageDelete;
  case ActionKind::MuteMember:
    return Permission::MemberMute;
  case ActionKind::KickMember:
    return Permission::MemberKick;
  case ActionKind::ApproveRequest:
    return Permission::RequestApprove;
  }
  return std::nullopt;
}
// Only Mute/Kick are governed by an ActionPolicy; all other kinds use their per-kind permission,
// adapter capability and the deployment quota/budget only.
inline bool action_policy_required(ActionKind kind) noexcept {
  return kind == ActionKind::MuteMember || kind == ActionKind::KickMember;
}
inline bool action_requires_group_conversation(ActionKind kind) noexcept {
  return kind == ActionKind::MuteMember || kind == ActionKind::KickMember;
}

// Status x unknownness: the only legal pairs. ACCEPTED is NOT_STARTED (not dispatched) or UNKNOWN
// (dispatched without a platform result); EXPIRED is NOT_STARTED only when dispatched_at is absent,
// UNKNOWN when a dispatch was recorded. SUCCEEDED/FAILED require a known outcome; CANCELLED only
// happens before dispatch. Invalid pairs are rejected rather than normalized.
inline bool action_status_outcome_compatible(ActionState state, ExecutionOutcome outcome) noexcept {
  switch (state) {
  case ActionState::Accepted:
    return outcome == ExecutionOutcome::NotStarted || outcome == ExecutionOutcome::Unknown;
  case ActionState::Succeeded:
    return outcome == ExecutionOutcome::KnownSucceeded;
  case ActionState::Failed:
    return outcome == ExecutionOutcome::KnownFailed;
  case ActionState::Expired:
    return outcome == ExecutionOutcome::NotStarted || outcome == ExecutionOutcome::Unknown;
  case ActionState::Cancelled:
    return outcome == ExecutionOutcome::NotStarted;
  }
  return false;
}
inline bool action_terminal(ActionState state) noexcept {
  return state != ActionState::Accepted;
}

struct ActionPolicy {
  ResourceId account_id;
  bool mute_enabled = false;
  bool kick_enabled = false;
  std::vector<std::string> allowed_group_ids;
  std::uint32_t mute_daily_budget = 0;
  std::uint32_t kick_daily_budget = 0;
  ResourceVersion version;
  TimePoint updated_at;
};

// Deployment configuration, never request-controlled. Defaults mirror the approved P07 values;
// tests and deployments may inject stricter limits.
struct ActionWindowLimit {
  std::uint32_t maximum = 0;
  std::chrono::seconds window{0};
};
struct ActionQuotaLimits {
  ActionWindowLimit account{10, std::chrono::seconds{60}};
  ActionWindowLimit group{3, std::chrono::seconds{60}};
  ActionWindowLimit private_conversation{1, std::chrono::seconds{10}};
  // (kind, account, UTC day) hard cap; 0 disables the kind. Mute/Kick additionally require a
  // positive policy budget, which is the "positive budget" gate rather than this deployment cap.
  std::array<std::uint32_t, 6> daily_budget{1000, 1000, 1000, 100, 100, 100};
};
enum class ActionScopeKind : std::uint8_t { Account = 0, Group = 1, PrivateConversation = 2 };
struct ActionScope {
  ActionScopeKind kind;
  std::string id;
  bool operator==(const ActionScope&) const = default;
};
inline std::vector<ActionScope> action_quota_scopes(ActionKind kind, const ResourceId& account,
                                                    std::string_view conversation,
                                                    std::uint8_t conversation_kind) {
  std::vector<ActionScope> scopes;
  if (account.value.empty()) {
    return scopes;
  }
  scopes.push_back({ActionScopeKind::Account, account.value});
  // Mute/Kick only act on groups; refuse an inconsistent conversation kind before admission.
  if (action_requires_group_conversation(kind)) {
    if (conversation_kind == 1 && !conversation.empty()) {
      scopes.push_back({ActionScopeKind::Group, std::string{conversation}});
    }
    return scopes;
  }
  if (conversation_kind == 1 && !conversation.empty()) {
    scopes.push_back({ActionScopeKind::Group, std::string{conversation}});
  } else if (conversation_kind == 0 && !conversation.empty()) {
    scopes.push_back({ActionScopeKind::PrivateConversation, std::string{conversation}});
  }
  return scopes;
}
inline const ActionWindowLimit& action_window_limit(const ActionQuotaLimits& limits,
                                                    ActionScopeKind kind) noexcept {
  switch (kind) {
  case ActionScopeKind::Account:
    return limits.account;
  case ActionScopeKind::Group:
    return limits.group;
  case ActionScopeKind::PrivateConversation:
    return limits.private_conversation;
  }
  return limits.account;
}
struct AcceptedActionSample {
  ActionScope scope;
  TimePoint accepted_at;
};

// A window admits when fewer than the configured maximum samples fall in the half-open window
// (accepted_at > now-window). Cancelled/failed/expired actions are deliberately not refunded:
// budgets are a conservative safety control, not billing.
inline bool action_window_admits(const ActionQuotaLimits& limits, const ActionScope& scope,
                                 std::span<const TimePoint> accepted_at, TimePoint now) {
  const auto& limit = action_window_limit(limits, scope.kind);
  if (limit.maximum == 0) {
    return false;
  }
  const auto beginning = now - limit.window;
  std::size_t in_window = 0;
  for (const auto& sample : accepted_at) {
    if (sample > beginning && sample <= now) {
      ++in_window;
    }
  }
  return in_window < limit.maximum;
}
inline bool action_daily_budget_admits(const ActionQuotaLimits& limits, ActionKind kind,
                                       std::uint32_t used) noexcept {
  if (!known_action_kind(kind)) {
    return false;
  }
  const auto budget = limits.daily_budget[static_cast<std::size_t>(kind) - 1];
  return budget > 0 && used < budget;
}
inline std::int64_t utc_day(TimePoint now) noexcept {
  const auto day = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::hours{24});
  return static_cast<std::int64_t>(now.time_since_epoch().count() / day.count());
}

// Mute/Kick policy gate: fail closed on a missing/disabled policy, a non-positive budget, a
// non-group target or a group outside the explicit allowlist. Other kinds never consult policy.
inline Status check_action_policy(ActionKind kind, std::string_view conversation,
                                  std::uint8_t conversation_kind,
                                  const std::optional<ActionPolicy>& policy,
                                  const ResourceId& account, TimePoint now) {
  if (!known_action_kind(kind)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (!action_policy_required(kind)) {
    return std::monostate{};
  }
  if (conversation_kind != 1 || conversation.empty()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (!policy || policy->account_id != account || policy->updated_at > now) {
    return rejected(ErrorCode::PermissionDenied);
  }
  const bool enabled = kind == ActionKind::MuteMember ? policy->mute_enabled : policy->kick_enabled;
  const auto budget =
      kind == ActionKind::MuteMember ? policy->mute_daily_budget : policy->kick_daily_budget;
  if (!enabled || budget == 0) {
    return rejected(ErrorCode::PermissionDenied);
  }
  if (std::find(policy->allowed_group_ids.begin(), policy->allowed_group_ids.end(),
                std::string{conversation}) == policy->allowed_group_ids.end()) {
    return rejected(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}

struct ActionRequest {
  InstanceId owner_instance;
  ResourceId account_id;
  ResourceId adapter_id;
  ResourceVersion binding_version;
  ActionKind kind = ActionKind::SendMessage;
  std::string idempotency_key;
  Sha256Digest request_hash;
  std::string serialized_envelope;
  std::string target_conversation;
  std::uint8_t target_conversation_kind = 0;
  TimePoint deadline;
};
struct ActionRecord {
  ActionId action_id;
  InstanceId owner_instance;
  ResourceId account_id;
  ResourceId adapter_id;
  std::optional<InstanceId> dispatched_to_instance;
  ResourceVersion binding_version;
  ActionKind kind = ActionKind::SendMessage;
  std::string capability;
  std::string idempotency_key;
  Sha256Digest request_hash;
  std::string serialized_envelope;
  std::string target_conversation;
  std::uint8_t target_conversation_kind = 0;
  TimePoint deadline;
  TimePoint created_at;
  TimePoint updated_at;
  ActionState state = ActionState::Accepted;
  ExecutionOutcome execution_outcome = ExecutionOutcome::NotStarted;
  ActionDispatchState dispatch_state = ActionDispatchState::NotDispatched;
  std::optional<AttemptId> dispatch_attempt_id;
  std::uint32_t dispatch_sequence = 0;
  std::optional<TimePoint> dispatched_at;
  std::optional<std::string> platform_message_id;
  std::optional<ErrorCode> error_code;
  std::optional<TimePoint> completed_at;
  TimePoint retain_until;
};

inline TimePoint action_terminal_retain_until(TimePoint now) noexcept {
  return now + kActionTerminalRetention;
}
inline TimePoint action_evidence_retain_until(TimePoint now) noexcept {
  return now + kActionEvidenceRetention;
}

// Validate the immutable request shape and create the admitted record. A future deadline over the
// 300s cap is INVALID_ARGUMENT; an already elapsed deadline persists EXPIRED+NOT_STARTED without
// consuming quota or invoking a platform (same idempotency identity, replayed by key).
inline Outcome<ActionRecord> admit_action(ActionId action_id, const ActionRequest& request,
                                          TimePoint now) {
  if (action_id.value.empty() || request.owner_instance.value.empty() ||
      request.account_id.value.empty() || request.adapter_id.value.empty() ||
      request.idempotency_key.empty() || request.target_conversation.empty() ||
      request.serialized_envelope.empty() ||
      request.serialized_envelope.size() > kMaximumSerializedEnvelope ||
      !known_action_kind(request.kind) || request.target_conversation_kind > 2) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (request.deadline > now + kActionDeadlineMaximum) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  ActionRecord record;
  record.action_id = std::move(action_id);
  record.owner_instance = request.owner_instance;
  record.account_id = request.account_id;
  record.adapter_id = request.adapter_id;
  record.binding_version = request.binding_version;
  record.kind = request.kind;
  record.capability = std::string{action_capability(request.kind)};
  record.idempotency_key = request.idempotency_key;
  record.request_hash = request.request_hash;
  record.serialized_envelope = request.serialized_envelope;
  record.target_conversation = request.target_conversation;
  record.target_conversation_kind = request.target_conversation_kind;
  record.deadline = request.deadline;
  record.created_at = now;
  record.updated_at = now;
  record.retain_until = TimePoint{};
  if (request.deadline <= now) {
    record.state = ActionState::Expired;
    record.execution_outcome = ExecutionOutcome::NotStarted;
    record.error_code = ErrorCode::Timeout;
    record.completed_at = now;
    record.retain_until = action_terminal_retain_until(now);
  }
  return record;
}
// Idempotent replay: the stored request hash and kind decide duplicate vs conflict. A duplicate
// returns the original record (including a terminal snapshot); a different fingerprint is CONFLICT.
inline Outcome<ActionRecord> replay_action(ActionRecord record, const ActionRequest& request) {
  if (record.owner_instance != request.owner_instance ||
      record.idempotency_key != request.idempotency_key ||
      record.request_hash != request.request_hash || record.kind != request.kind) {
    return rejected(ErrorCode::Conflict);
  }
  return record;
}

// Mark the first dispatch attempt. Only Accepted+NotDispatched may be dispatched; the caller
// persists the attempt id/sequence and then writes the Adapter frame after commit. Unknown
// dispatched actions are never dispatched again to a different adapter instance (old adapter
// execution is never taken over).
inline Status mark_action_dispatched(ActionRecord& record, AttemptId attempt_id,
                                     InstanceId adapter_instance, TimePoint now) {
  if (record.state != ActionState::Accepted ||
      record.dispatch_state != ActionDispatchState::NotDispatched || attempt_id.value.empty() ||
      adapter_instance.value.empty() || now >= record.deadline ||
      record.dispatch_sequence >= kMaximumActionDispatchSequence) {
    return rejected(ErrorCode::Conflict);
  }
  record.dispatch_attempt_id = std::move(attempt_id);
  record.dispatched_to_instance = std::move(adapter_instance);
  record.dispatch_sequence += 1;
  record.dispatch_state = ActionDispatchState::Dispatched;
  record.execution_outcome = ExecutionOutcome::Unknown;
  record.dispatched_at = now;
  record.updated_at = now;
  return std::monostate{};
}
// Re-dispatch is limited to the already recorded adapter instance (adapter journal dedupes);
// a different current binding means no dispatch and eventual EXPIRED+UNKNOWN evidence.
inline Status action_redispatch_allowed(const ActionRecord& record,
                                        const InstanceId& adapter_instance,
                                        const ResourceVersion& binding_version, TimePoint now) {
  if (record.state != ActionState::Accepted ||
      record.dispatch_state != ActionDispatchState::Dispatched || now >= record.deadline) {
    return rejected(ErrorCode::Conflict);
  }
  if (!record.dispatched_to_instance || *record.dispatched_to_instance != adapter_instance ||
      record.binding_version != binding_version) {
    return rejected(ErrorCode::Conflict);
  }
  return std::monostate{};
}
// Cancel is only possible before dispatch. Already-terminal records keep their state (EXPIRED is
// returned as EXPIRED, not rewritten); dispatched accepted records CONFLICT.
inline Status cancel_action(ActionRecord& record, TimePoint now) {
  if (record.state == ActionState::Accepted &&
      record.dispatch_state == ActionDispatchState::NotDispatched) {
    record.state = ActionState::Cancelled;
    record.execution_outcome = ExecutionOutcome::NotStarted;
    record.updated_at = now;
    record.completed_at = now;
    record.retain_until = action_terminal_retain_until(now);
    return std::monostate{};
  }
  if (record.state == ActionState::Expired || record.state == ActionState::Cancelled) {
    return std::monostate{};
  }
  return rejected(ErrorCode::Conflict);
}
// Expire an accepted action at/after its business deadline. A dispatched action becomes
// EXPIRED+UNKNOWN (a platform side effect may exist); a never-dispatched one EXPIRED+NOT_STARTED.
inline bool expire_action(ActionRecord& record, TimePoint now) {
  if (record.state != ActionState::Accepted || now < record.deadline) {
    return false;
  }
  record.state = ActionState::Expired;
  record.execution_outcome = record.dispatch_state == ActionDispatchState::Dispatched
                                 ? ExecutionOutcome::Unknown
                                 : ExecutionOutcome::NotStarted;
  record.error_code = ErrorCode::Timeout;
  record.updated_at = now;
  record.completed_at = now;
  record.retain_until = action_terminal_retain_until(now);
  return true;
}

struct ActionTerminalResult {
  // Only SUCCEEDED or FAILED may be reported by an adapter; other states reject.
  ActionState state = ActionState::Succeeded;
  ExecutionOutcome execution_outcome = ExecutionOutcome::KnownSucceeded;
  std::optional<std::string> platform_message_id;
  std::optional<ErrorCode> error_code;
};
enum class ActionReceiptDisposition : std::uint8_t { Recorded, Duplicate, EvidenceOnly, Conflict };
struct ActionTerminalDecision {
  ActionReceiptDisposition disposition = ActionReceiptDisposition::Recorded;
  ActionRecord record;
  bool append_evidence = false;
  ActionEvidenceKind evidence_kind = ActionEvidenceKind::LateResult;
};
inline bool valid_terminal_report(const ActionTerminalResult& result) noexcept {
  if (result.state == ActionState::Succeeded) {
    return result.execution_outcome == ExecutionOutcome::KnownSucceeded && !result.error_code;
  }
  if (result.state == ActionState::Failed) {
    return result.execution_outcome == ExecutionOutcome::KnownFailed &&
           result.error_code.has_value();
  }
  return false;
}
inline bool same_terminal_snapshot(const ActionRecord& record,
                                   const ActionTerminalResult& result) noexcept {
  if (record.state != result.state || record.execution_outcome != result.execution_outcome ||
      record.platform_message_id != result.platform_message_id ||
      record.error_code != result.error_code) {
    return false;
  }
  return true;
}
// Apply an adapter's terminal report. The reporting attempt and adapter instance must match the
// persisted dispatch; a late result for EXPIRED/CANCELLED appends evidence only (EVIDENCE_ONLY),
// a duplicate of the known terminal snapshot is DUPLICATE (no rewrite), and a different terminal
// result is CONFLICT plus evidence (never overwrite the authoritative record).
inline Outcome<ActionTerminalDecision>
apply_action_terminal_result(ActionRecord record, std::optional<AttemptId> attempt_id,
                             std::optional<InstanceId> adapter_instance,
                             const ActionTerminalResult& result, TimePoint now) {
  if (!valid_terminal_report(result) || record.dispatch_state != ActionDispatchState::Dispatched ||
      !record.dispatch_attempt_id || !record.dispatched_to_instance || !attempt_id ||
      !adapter_instance || *record.dispatch_attempt_id != *attempt_id ||
      *record.dispatched_to_instance != *adapter_instance) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  ActionTerminalDecision decision;
  decision.record = record;
  if (record.state == ActionState::Expired || record.state == ActionState::Cancelled) {
    decision.disposition = ActionReceiptDisposition::EvidenceOnly;
    decision.append_evidence = true;
    decision.evidence_kind = ActionEvidenceKind::LateResult;
    return decision;
  }
  if (action_terminal(record.state)) {
    if (same_terminal_snapshot(record, result)) {
      decision.disposition = ActionReceiptDisposition::Duplicate;
      return decision;
    }
    decision.disposition = ActionReceiptDisposition::Conflict;
    decision.append_evidence = true;
    decision.evidence_kind = ActionEvidenceKind::Conflict;
    return decision;
  }
  record.state = result.state;
  record.execution_outcome = result.execution_outcome;
  record.platform_message_id = result.platform_message_id;
  record.error_code = result.error_code;
  record.completed_at = now;
  record.updated_at = now;
  record.retain_until = action_terminal_retain_until(now);
  decision.record = record;
  decision.disposition = ActionReceiptDisposition::Recorded;
  return decision;
}

struct ActionEvidenceReport {
  ActionEvidenceKind kind = ActionEvidenceKind::Unknown;
  ExecutionOutcome execution_outcome = ExecutionOutcome::Unknown;
  std::optional<ErrorCode> error_code;
  std::optional<std::string> platform_reference;
  TimePoint observed_at;
};
// Non-terminal evidence is append-only. A known terminal snapshot is never altered by evidence;
// callers persist the evidence row and acknowledge EVIDENCE_ONLY. The report must match the
// recorded dispatch attempt and adapter instance, and the evidence kind/outcome pair must be one
// of the frozen combinations (unknown/not_started for non-terminal, late_result/conflict for
// post-terminal reporting).
inline Outcome<std::pair<ActionEvidenceKind, TimePoint>>
validate_action_evidence(const ActionRecord& record, const AttemptId& attempt_id,
                         const InstanceId& adapter_instance, const ActionEvidenceReport& report,
                         TimePoint now) {
  if (record.dispatch_state != ActionDispatchState::Dispatched || !record.dispatch_attempt_id ||
      !record.dispatched_to_instance || *record.dispatch_attempt_id != attempt_id ||
      *record.dispatched_to_instance != adapter_instance ||
      (report.platform_reference && report.platform_reference->size() > 256)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const bool compatible = (report.kind == ActionEvidenceKind::Unknown &&
                           report.execution_outcome == ExecutionOutcome::Unknown) ||
                          (report.kind == ActionEvidenceKind::NotStarted &&
                           report.execution_outcome == ExecutionOutcome::NotStarted) ||
                          (report.kind == ActionEvidenceKind::LateResult &&
                           report.execution_outcome == ExecutionOutcome::Unknown) ||
                          (report.kind == ActionEvidenceKind::Conflict &&
                           (report.execution_outcome == ExecutionOutcome::KnownSucceeded ||
                            report.execution_outcome == ExecutionOutcome::KnownFailed));
  if (!compatible) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::make_pair(report.kind, action_evidence_retain_until(now));
}
// Terminal retention and evidence retention never extend each other; callers keep both rows.
inline TimePoint action_retain_until(const ActionRecord& record) noexcept {
  return record.retain_until;
}

// Wire ActionResult may only expose the frozen status x outcome matrix; callers rendering a
// record must not fabricate a known outcome after EXPIRED. Returns nullopt for an invalid row
// rather than silently coercing it.
inline std::optional<std::pair<ActionState, ExecutionOutcome>>
action_wire_snapshot(const ActionRecord& record) noexcept {
  if (!action_status_outcome_compatible(record.state, record.execution_outcome)) {
    return std::nullopt;
  }
  return std::make_pair(record.state, record.execution_outcome);
}
} // namespace kiarabot::core::target
