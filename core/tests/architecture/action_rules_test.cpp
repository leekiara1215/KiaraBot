#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/action_rules.hpp"

namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""h;

namespace {
void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
template <typename Value>
bool failure_is(const target::Outcome<Value>& outcome, target::ErrorCode code) {
  const auto* failure = std::get_if<target::Failure>(&outcome);
  return failure != nullptr && failure->code == code;
}
target::TimePoint at(std::int64_t seconds) {
  return target::TimePoint{std::chrono::seconds{seconds}};
}
target::ResourceId account() {
  return {"account"};
}
target::InstanceId instance(std::string value = "plugin-instance") {
  return {std::move(value)};
}
target::ActionId action_id(std::string value = "action") {
  return {std::move(value)};
}
target::AttemptId attempt_id(std::string value = "attempt") {
  return {std::move(value)};
}
target::Sha256Digest digest(std::byte value) {
  target::Sha256Digest hash;
  hash.bytes.fill(value);
  return hash;
}
target::ActionRequest request(std::string conversation = "conversation", std::uint8_t kind = 1) {
  target::ActionRequest value;
  value.owner_instance = instance();
  value.account_id = account();
  value.adapter_id = {"adapter"};
  value.binding_version = {1};
  value.kind = target::ActionKind::SendMessage;
  value.idempotency_key = "key";
  value.request_hash = digest(std::byte{1});
  value.serialized_envelope = std::string(64, 'x');
  value.target_conversation = std::move(conversation);
  value.target_conversation_kind = kind;
  value.deadline = at(1000);
  return value;
}
target::ActionRecord accepted_record(std::string conversation = "conversation",
                                     std::uint8_t kind = 1) {
  auto auto_record =
      target::admit_action(action_id(), request(std::move(conversation), kind), at(900));
  check(std::holds_alternative<target::ActionRecord>(auto_record), "admission fixture accepted");
  return std::get<target::ActionRecord>(std::move(auto_record));
}
target::ActionRecord dispatched_record(std::string conversation = "conversation",
                                       std::uint8_t kind = 1) {
  auto record = accepted_record(std::move(conversation), kind);
  check(std::holds_alternative<std::monostate>(target::mark_action_dispatched(
            record, attempt_id(), instance("adapter-instance"), at(901))),
        "dispatch fixture accepted");
  return record;
}
target::ActionPolicy policy() {
  target::ActionPolicy value;
  value.account_id = account();
  value.mute_enabled = true;
  value.kick_enabled = false;
  value.allowed_group_ids = {"group"};
  value.mute_daily_budget = 1;
  value.kick_daily_budget = 0;
  value.version = {1};
  value.updated_at = at(1);
  return value;
}
void test_kind_mapping() {
  const auto send = target::action_kind_from_payload("send_message");
  check(std::holds_alternative<target::ActionKindInfo>(send), "send_message maps");
  check(std::get<target::ActionKindInfo>(send).kind == target::ActionKind::SendMessage,
        "send kind");
  check(std::get<target::ActionKindInfo>(send).permission == target::Permission::MessageSend,
        "send permission");
  check(std::get<target::ActionKindInfo>(send).capability == "action.send_message.text.v1",
        "send capability");
  check(target::action_kind_from_payload("approve_request").index() == 0, "approve maps");
  check(std::holds_alternative<target::Failure>(target::action_kind_from_payload("schedule_task")),
        "legacy schedule_task is not an Action");
  check(std::holds_alternative<target::Failure>(target::action_kind_from_payload("unknown")),
        "unknown payload rejected");
  check(target::action_capability(target::ActionKind::KickMember) == "action.kick_member.v1",
        "kick capability");
  check(target::action_permission(target::ActionKind::ApproveRequest) ==
            target::Permission::RequestApprove,
        "approve permission");
  check(!target::known_action_kind(static_cast<target::ActionKind>(0)), "unspecified kind invalid");
  check(!target::known_action_kind(static_cast<target::ActionKind>(7)),
        "out-of-range kind invalid");
  check(target::action_policy_required(target::ActionKind::MuteMember), "mute needs policy");
  check(!target::action_requires_group_conversation(target::ActionKind::SendMessage),
        "send not group-only");
}
void test_admission() {
  auto valid = target::admit_action(action_id(), request(), at(900));
  check(std::holds_alternative<target::ActionRecord>(valid), "valid admission accepted");
  const auto& record = std::get<target::ActionRecord>(valid);
  check(record.state == target::ActionState::Accepted, "fresh action accepted");
  check(record.execution_outcome == target::ExecutionOutcome::NotStarted, "fresh not started");
  check(record.dispatch_state == target::ActionDispatchState::NotDispatched, "fresh undispatched");
  check(record.dispatch_sequence == 0, "fresh sequence zero");
  check(record.capability == "action.send_message.text.v1", "capability projected");

  auto oversized = request();
  oversized.serialized_envelope = std::string(target::kMaximumSerializedEnvelope + 1, 'x');
  check(failure_is(target::admit_action(action_id(), oversized, at(900)),
                   target::ErrorCode::InvalidArgument),
        "oversized envelope rejected");
  auto missing_key = request();
  missing_key.idempotency_key.clear();
  check(failure_is(target::admit_action(action_id(), missing_key, at(900)),
                   target::ErrorCode::InvalidArgument),
        "missing idempotency key rejected");
  auto missing_conversation = request();
  missing_conversation.target_conversation.clear();
  check(failure_is(target::admit_action(action_id(), missing_conversation, at(900)),
                   target::ErrorCode::InvalidArgument),
        "missing conversation rejected");
  check(
      failure_is(target::admit_action({}, request(), at(900)), target::ErrorCode::InvalidArgument),
      "missing action id rejected");

  auto long_deadline = request();
  long_deadline.deadline = at(900) + target::kActionDeadlineMaximum + 1s;
  check(failure_is(target::admit_action(action_id(), long_deadline, at(900)),
                   target::ErrorCode::InvalidArgument),
        "deadline above 300s rejected");
  auto maximum_deadline = request();
  maximum_deadline.deadline = at(900) + target::kActionDeadlineMaximum;
  check(std::holds_alternative<target::ActionRecord>(
            target::admit_action(action_id(), maximum_deadline, at(900))),
        "deadline exactly 300s accepted");

  auto past_deadline = request();
  past_deadline.deadline = at(899);
  auto expired = target::admit_action(action_id(), past_deadline, at(900));
  check(std::holds_alternative<target::ActionRecord>(expired), "past deadline persists expired");
  const auto& expired_record = std::get<target::ActionRecord>(expired);
  check(expired_record.state == target::ActionState::Expired, "past deadline state expired");
  check(expired_record.execution_outcome == target::ExecutionOutcome::NotStarted,
        "past deadline never started");
  check(expired_record.error_code == target::ErrorCode::Timeout, "past deadline TIMEOUT");
  check(expired_record.completed_at.has_value(), "past deadline completed");
  check(expired_record.retain_until == target::action_terminal_retain_until(at(900)),
        "past deadline retention");
}
void test_replay() {
  auto record = accepted_record();
  auto replay = target::replay_action(record, request());
  check(std::holds_alternative<target::ActionRecord>(replay), "same fingerprint replays record");
  check(std::get<target::ActionRecord>(replay).action_id == record.action_id, "replay action id");
  auto different = request();
  different.request_hash = digest(std::byte{2});
  check(failure_is(target::replay_action(record, different), target::ErrorCode::Conflict),
        "same key different fingerprint conflicts");
  auto other_owner = request();
  other_owner.owner_instance = instance("other");
  check(failure_is(target::replay_action(record, other_owner), target::ErrorCode::Conflict),
        "another owner never replays the record");
}
void test_policy_gate() {
  check(std::holds_alternative<std::monostate>(target::check_action_policy(
            target::ActionKind::SendMessage, "conversation", 0, {}, account(), at(900))),
        "non-mute kind never consults policy");
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "group", 1, {},
                                               account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "missing policy denies mute");
  auto current = policy();
  check(std::holds_alternative<std::monostate>(target::check_action_policy(
            target::ActionKind::MuteMember, "group", 1, current, account(), at(900))),
        "enabled mute policy accepts");
  current.mute_enabled = false;
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "group", 1, current,
                                               account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "disabled mute policy denies");
  current = policy();
  current.mute_daily_budget = 0;
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "group", 1, current,
                                               account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "zero policy budget denies");
  current = policy();
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "other-group", 1,
                                               current, account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "group outside whitelist denies");
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "group", 0, current,
                                               account(), at(900)),
                   target::ErrorCode::InvalidArgument),
        "mute requires a group conversation");
  current = policy();
  current.kick_enabled = false;
  check(failure_is(target::check_action_policy(target::ActionKind::KickMember, "group", 1, current,
                                               account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "kick disabled denies even when mute is enabled");
  current = policy();
  current.kick_enabled = true;
  current.kick_daily_budget = 2;
  check(std::holds_alternative<std::monostate>(target::check_action_policy(
            target::ActionKind::KickMember, "group", 1, current, account(), at(900))),
        "enabled kick policy accepts");
  current.account_id = {"other-account"};
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "group", 1, current,
                                               account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "policy for another account does not authorize");
  current = policy();
  current.updated_at = at(901);
  check(failure_is(target::check_action_policy(target::ActionKind::MuteMember, "group", 1, current,
                                               account(), at(900)),
                   target::ErrorCode::PermissionDenied),
        "future-dated policy does not authorize");
}
void test_quota() {
  target::ActionQuotaLimits limits;
  const target::ActionScope account_scope{target::ActionScopeKind::Account, "account"};
  std::vector<target::TimePoint> samples;
  for (std::uint32_t index = 0; index < limits.account.maximum; ++index) {
    samples.push_back(at(950));
  }
  check(!target::action_window_admits(limits, account_scope, samples, at(960)),
        "account window at limit rejects");
  samples.pop_back();
  check(target::action_window_admits(limits, account_scope, samples, at(960)),
        "account window below limit admits");
  samples.push_back(at(899));
  check(target::action_window_admits(limits, account_scope, samples, at(960)),
        "samples outside window do not count");
  const auto scopes =
      target::action_quota_scopes(target::ActionKind::SendMessage, account(), "conversation", 0);
  check(scopes.size() == 2 && scopes[0].kind == target::ActionScopeKind::Account &&
            scopes[1].kind == target::ActionScopeKind::PrivateConversation,
        "private send samples account and private scope");
  const auto group_scopes =
      target::action_quota_scopes(target::ActionKind::MuteMember, account(), "group", 1);
  check(group_scopes.size() == 2 && group_scopes[1].kind == target::ActionScopeKind::Group,
        "mute samples account and group scope");
  check(target::action_quota_scopes(target::ActionKind::MuteMember, account(), "group", 0).size() ==
            1,
        "mute with non-group conversation never samples group scope");
  check(!target::action_daily_budget_admits(limits, target::ActionKind::SendMessage, 1000),
        "daily budget exhausted rejects");
  check(target::action_daily_budget_admits(limits, target::ActionKind::SendMessage, 0),
        "daily budget admits fresh action");
  target::ActionQuotaLimits disabled;
  disabled.daily_budget[0] = 0;
  check(!target::action_daily_budget_admits(disabled, target::ActionKind::SendMessage, 0),
        "zero daily budget disables kind");
  check(target::utc_day(at(0)) == 0, "epoch is day zero");
  check(target::utc_day(at(0) + target::kActionDeadlineMaximum) == 0, "single day stays same day");
  check(target::utc_day(at(0) + 24h) == 1, "next UTC day");
}
void test_dispatch_and_cancel() {
  auto record = accepted_record();
  check(std::holds_alternative<std::monostate>(target::mark_action_dispatched(
            record, attempt_id(), instance("adapter-instance"), at(901))),
        "first dispatch marks record");
  check(record.dispatch_state == target::ActionDispatchState::Dispatched,
        "dispatch state recorded");
  check(record.dispatch_sequence == 1, "dispatch sequence incremented");
  check(record.execution_outcome == target::ExecutionOutcome::Unknown, "dispatched is unknown");
  check(record.dispatched_to_instance == instance("adapter-instance"),
        "dispatch instance recorded");
  check(failure_is(target::mark_action_dispatched(record, attempt_id("second"),
                                                  instance("adapter-instance"), at(902)),
                   target::ErrorCode::Conflict),
        "second dispatch conflicts");
  check(std::holds_alternative<std::monostate>(
            target::action_redispatch_allowed(record, instance("adapter-instance"), {1}, at(902))),
        "redispatch to recorded instance allowed");
  check(
      failure_is(target::action_redispatch_allowed(record, instance("other-adapter"), {1}, at(902)),
                 target::ErrorCode::Conflict),
      "unknown execution never moves to another adapter");
  check(failure_is(
            target::action_redispatch_allowed(record, instance("adapter-instance"), {2}, at(902)),
            target::ErrorCode::Conflict),
        "binding version change prevents redispatch");

  auto cancellable = accepted_record();
  check(std::holds_alternative<std::monostate>(target::cancel_action(cancellable, at(902))),
        "undispatched accepted cancels");
  check(cancellable.state == target::ActionState::Cancelled, "cancel state recorded");
  check(cancellable.execution_outcome == target::ExecutionOutcome::NotStarted,
        "cancel never started");
  check(!cancellable.dispatched_at.has_value(), "cancelled never dispatched");
  check(std::holds_alternative<std::monostate>(target::cancel_action(cancellable, at(903))),
        "repeat cancel is idempotent");
  auto dispatched = dispatched_record();
  check(failure_is(target::cancel_action(dispatched, at(902)), target::ErrorCode::Conflict),
        "dispatched action cannot cancel");
  auto expired = accepted_record();
  check(target::expire_action(expired, at(1000)), "deadline expiry applies");
  check(expired.state == target::ActionState::Expired, "expired state");
  check(expired.execution_outcome == target::ExecutionOutcome::NotStarted,
        "undispatched expiry never started");
  check(std::holds_alternative<std::monostate>(target::cancel_action(expired, at(1001))),
        "expired cancellation returns current record");
  check(expired.state == target::ActionState::Expired, "expired state is not rewritten");
  auto dispatched_expired = dispatched_record();
  check(target::expire_action(dispatched_expired, at(1000)), "dispatched expiry applies");
  check(dispatched_expired.execution_outcome == target::ExecutionOutcome::Unknown,
        "dispatched expiry remains unknown");
  check(dispatched_expired.error_code == target::ErrorCode::Timeout, "expiry TIMEOUT");
  check(dispatched_expired.retain_until == target::action_terminal_retain_until(at(1000)),
        "expiry retention");
}
target::ActionTerminalResult success_result() {
  target::ActionTerminalResult result;
  result.state = target::ActionState::Succeeded;
  result.execution_outcome = target::ExecutionOutcome::KnownSucceeded;
  result.platform_message_id = "platform-message";
  return result;
}
void test_terminal_result() {
  check(target::valid_terminal_report(success_result()), "successful terminal report valid");
  auto failure = success_result();
  failure.state = target::ActionState::Failed;
  failure.execution_outcome = target::ExecutionOutcome::KnownFailed;
  failure.platform_message_id.reset();
  check(!target::valid_terminal_report(failure), "failed report needs a stable error");
  failure.error_code = target::ErrorCode::PluginUnavailable;
  check(target::valid_terminal_report(failure), "failed report with error valid");
  auto accepted_report = success_result();
  accepted_report.state = target::ActionState::Accepted;
  check(!target::valid_terminal_report(accepted_report), "accepted is not an adapter terminal");

  auto record = dispatched_record();
  auto recorded = target::apply_action_terminal_result(
      record, attempt_id(), instance("adapter-instance"), success_result(), at(903));
  check(std::holds_alternative<target::ActionTerminalDecision>(recorded),
        "terminal result applies");
  const auto& decision = std::get<target::ActionTerminalDecision>(recorded);
  check(decision.disposition == target::ActionReceiptDisposition::Recorded,
        "first result recorded");
  check(decision.record.state == target::ActionState::Succeeded, "success state recorded");
  check(decision.record.execution_outcome == target::ExecutionOutcome::KnownSucceeded,
        "known success recorded");
  check(decision.record.platform_message_id == "platform-message", "platform id recorded");
  check(decision.record.retain_until == target::action_terminal_retain_until(at(903)),
        "terminal retention");
  check(!decision.append_evidence, "first result needs no evidence");

  auto duplicate = target::apply_action_terminal_result(
      decision.record, attempt_id(), instance("adapter-instance"), success_result(), at(904));
  check(std::holds_alternative<target::ActionTerminalDecision>(duplicate), "duplicate applies");
  check(std::get<target::ActionTerminalDecision>(duplicate).disposition ==
            target::ActionReceiptDisposition::Duplicate,
        "same terminal snapshot is duplicate");
  check(!std::get<target::ActionTerminalDecision>(duplicate).append_evidence,
        "duplicate writes no conflicting evidence");

  auto conflict_result = success_result();
  conflict_result.state = target::ActionState::Failed;
  conflict_result.execution_outcome = target::ExecutionOutcome::KnownFailed;
  conflict_result.platform_message_id.reset();
  conflict_result.error_code = target::ErrorCode::UpstreamUnavailable;
  auto conflicting = target::apply_action_terminal_result(
      decision.record, attempt_id(), instance("adapter-instance"), conflict_result, at(905));
  check(std::holds_alternative<target::ActionTerminalDecision>(conflicting),
        "conflicting result is acknowledged");
  check(std::get<target::ActionTerminalDecision>(conflicting).disposition ==
            target::ActionReceiptDisposition::Conflict,
        "different terminal snapshot conflicts");
  check(std::get<target::ActionTerminalDecision>(conflicting).append_evidence,
        "conflicting result appends evidence");
  check(std::get<target::ActionTerminalDecision>(conflicting).evidence_kind ==
            target::ActionEvidenceKind::Conflict,
        "conflict evidence kind");
  check(std::get<target::ActionTerminalDecision>(conflicting).record.state ==
            target::ActionState::Succeeded,
        "conflict never overwrites authoritative state");

  auto expired = dispatched_record();
  check(target::expire_action(expired, at(1000)), "late result fixture expires");
  auto late = target::apply_action_terminal_result(
      expired, attempt_id(), instance("adapter-instance"), success_result(), at(1001));
  check(std::holds_alternative<target::ActionTerminalDecision>(late), "late result acknowledged");
  check(std::get<target::ActionTerminalDecision>(late).disposition ==
            target::ActionReceiptDisposition::EvidenceOnly,
        "late result is evidence only");
  check(std::get<target::ActionTerminalDecision>(late).append_evidence,
        "late result appends evidence");
  check(std::get<target::ActionTerminalDecision>(late).evidence_kind ==
            target::ActionEvidenceKind::LateResult,
        "late result evidence kind");
  check(std::get<target::ActionTerminalDecision>(late).record.state == target::ActionState::Expired,
        "late result never rewrites expired");

  auto wrong_attempt = target::apply_action_terminal_result(
      record, attempt_id("other"), instance("adapter-instance"), success_result(), at(903));
  check(std::holds_alternative<target::Failure>(wrong_attempt), "attempt mismatch rejects");
  auto wrong_adapter = target::apply_action_terminal_result(
      record, attempt_id(), instance("other-adapter"), success_result(), at(903));
  check(std::holds_alternative<target::Failure>(wrong_adapter), "adapter mismatch rejects");
  auto undispatched = accepted_record();
  auto undispatched_result = target::apply_action_terminal_result(
      undispatched, attempt_id(), instance("adapter-instance"), success_result(), at(903));
  check(std::holds_alternative<target::Failure>(undispatched_result),
        "result for undispatched action rejects");
}
void test_evidence() {
  auto record = dispatched_record();
  auto evidence = [](target::ActionEvidenceKind kind, target::ExecutionOutcome outcome) {
    target::ActionEvidenceReport report;
    report.kind = kind;
    report.execution_outcome = outcome;
    report.observed_at = at(902);
    return report;
  };
  check(std::holds_alternative<std::pair<target::ActionEvidenceKind, target::TimePoint>>(
            target::validate_action_evidence(
                record, attempt_id(), instance("adapter-instance"),
                evidence(target::ActionEvidenceKind::Unknown, target::ExecutionOutcome::Unknown),
                at(905))),
        "unknown evidence accepted");
  check(std::holds_alternative<std::pair<target::ActionEvidenceKind, target::TimePoint>>(
            target::validate_action_evidence(record, attempt_id(), instance("adapter-instance"),
                                             evidence(target::ActionEvidenceKind::NotStarted,
                                                      target::ExecutionOutcome::NotStarted),
                                             at(905))),
        "not-started evidence accepted");
  check(std::holds_alternative<std::pair<target::ActionEvidenceKind, target::TimePoint>>(
            target::validate_action_evidence(record, attempt_id(), instance("adapter-instance"),
                                             evidence(target::ActionEvidenceKind::Conflict,
                                                      target::ExecutionOutcome::KnownSucceeded),
                                             at(905))),
        "conflict evidence accepted");
  check(failure_is(target::validate_action_evidence(record, attempt_id(),
                                                    instance("adapter-instance"),
                                                    evidence(target::ActionEvidenceKind::Unknown,
                                                             target::ExecutionOutcome::NotStarted),
                                                    at(905)),
                   target::ErrorCode::InvalidArgument),
        "mismatched evidence kind/outcome rejects");
  check(failure_is(target::validate_action_evidence(record, attempt_id("other"),
                                                    instance("adapter-instance"),
                                                    evidence(target::ActionEvidenceKind::Unknown,
                                                             target::ExecutionOutcome::Unknown),
                                                    at(905)),
                   target::ErrorCode::InvalidArgument),
        "evidence attempt mismatch rejects");
  auto long_reference =
      evidence(target::ActionEvidenceKind::Unknown, target::ExecutionOutcome::Unknown);
  long_reference.platform_reference = std::string(257, 'x');
  check(failure_is(target::validate_action_evidence(
                       record, attempt_id(), instance("adapter-instance"), long_reference, at(905)),
                   target::ErrorCode::InvalidArgument),
        "oversized platform reference rejects");
  check(target::action_evidence_retain_until(at(905)) == at(905) + target::kActionEvidenceRetention,
        "evidence retention is independent");
}
void test_matrix_and_wire_projection() {
  check(target::action_status_outcome_compatible(target::ActionState::Accepted,
                                                 target::ExecutionOutcome::NotStarted),
        "accepted/not_started legal");
  check(target::action_status_outcome_compatible(target::ActionState::Accepted,
                                                 target::ExecutionOutcome::Unknown),
        "accepted/unknown legal");
  check(!target::action_status_outcome_compatible(target::ActionState::Accepted,
                                                  target::ExecutionOutcome::KnownSucceeded),
        "accepted/known_succeeded illegal");
  check(target::action_status_outcome_compatible(target::ActionState::Succeeded,
                                                 target::ExecutionOutcome::KnownSucceeded),
        "succeeded/known_succeeded legal");
  check(!target::action_status_outcome_compatible(target::ActionState::Succeeded,
                                                  target::ExecutionOutcome::Unknown),
        "succeeded/unknown illegal");
  check(target::action_status_outcome_compatible(target::ActionState::Failed,
                                                 target::ExecutionOutcome::KnownFailed),
        "failed/known_failed legal");
  check(target::action_status_outcome_compatible(target::ActionState::Expired,
                                                 target::ExecutionOutcome::NotStarted),
        "expired/not_started legal");
  check(target::action_status_outcome_compatible(target::ActionState::Expired,
                                                 target::ExecutionOutcome::Unknown),
        "expired/unknown legal");
  check(!target::action_status_outcome_compatible(target::ActionState::Expired,
                                                  target::ExecutionOutcome::KnownSucceeded),
        "expired/known_succeeded illegal");
  check(target::action_status_outcome_compatible(target::ActionState::Cancelled,
                                                 target::ExecutionOutcome::NotStarted),
        "cancelled/not_started legal");
  check(!target::action_status_outcome_compatible(target::ActionState::Cancelled,
                                                  target::ExecutionOutcome::Unknown),
        "cancelled/unknown illegal");
  auto record = dispatched_record();
  check(target::action_wire_snapshot(record).has_value(), "dispatched snapshot renders");
  record.execution_outcome = target::ExecutionOutcome::KnownSucceeded;
  check(!target::action_wire_snapshot(record).has_value(),
        "illegal status/outcome row is not rendered");
  check(target::action_terminal(target::ActionState::Succeeded), "succeeded terminal");
  check(!target::action_terminal(target::ActionState::Accepted), "accepted non-terminal");
  check(target::action_terminal_retain_until(at(10)) == at(10) + 168h, "action retention 168h");
}
} // namespace

int main() {
  test_kind_mapping();
  test_admission();
  test_replay();
  test_policy_gate();
  test_quota();
  test_dispatch_and_cancel();
  test_terminal_result();
  test_evidence();
  test_matrix_and_wire_projection();
  std::cout << "P07 Action admission, policy/quota, dispatch/cancel, expiry, terminal and evidence "
               "rules passed\n";
  return 0;
}
