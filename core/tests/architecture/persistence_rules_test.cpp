#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

#include "kiarabot/core/domain/target/persistence_rules.hpp"

namespace target = kiarabot::core::target;
using std::chrono_literals::operator""h;
using std::chrono_literals::operator""s;

void check(bool condition, std::string_view scenario) {
  if (!condition) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
template <typename Value>
bool failure_is(const target::Outcome<Value>& result, target::ErrorCode code) {
  const auto* failure = std::get_if<target::Failure>(&result);
  return failure && failure->code == code && failure->acceptance == target::Acceptance::NotAccepted;
}
template <typename Value> bool equals(const target::Outcome<Value>& result, Value expected) {
  const auto* value = std::get_if<Value>(&result);
  return value && *value == expected;
}
target::HandoffContext handoff(target::TimePoint now) {
  const target::ResourceRef resource{target::ResourceKind::Task, {"task-resource"}};
  return {target::Service::Scheduler,
          {"message"},
          {target::Service::Scheduler, {"operation"}, resource},
          std::nullopt,
          resource,
          {std::numeric_limits<std::uint64_t>::max()},
          {"principal"},
          {1},
          {},
          now,
          now + 1h};
}
target::OutboxEntry outbox(target::TimePoint now) {
  const auto context = handoff(now);
  return {context,
          {context.resource, context.version},
          target::OutboxLane::Ordinary,
          target::OutboxState::Pending,
          0,
          now,
          std::nullopt,
          std::nullopt};
}
target::InboxRecord inbox(target::TimePoint now) {
  const auto context = handoff(now);
  return {context, now, now + 1h + target::kHandoffRetention, target::EmptySuccess{}};
}
void receipt_rules(target::TimePoint now) {
  const target::ResourceRef resource{target::ResourceKind::Model, {"same-id"}};
  const target::CommandReceipt original{
      {{"principal"}, "update", resource, "key"},
      {},
      target::ResourceMutationResult{resource, {std::numeric_limits<std::uint64_t>::max()}},
      now,
      now + target::kMinimumReceiptRetention};
  check(equals(target::check_receipt_write(nullptr, original, 0, {1}, now),
               target::WriteDecision::Insert),
        "new receipt at exactly seven-day retention is accepted");
  check(failure_is(target::check_receipt_write(nullptr, original, 1, {1}, now),
                   target::ErrorCode::RateLimited),
        "full receipt capacity rejects new acceptance");
  check(failure_is(target::check_receipt_write(nullptr, original, 0, {0}, now),
                   target::ErrorCode::RateLimited),
        "zero receipt budget never admits new records");
  auto proposed = original;
  proposed.result = target::EmptySuccess{};
  proposed.created_at += 1h;
  proposed.retain_until += 1h;
  check(equals(target::check_receipt_write(&original, proposed, 1, {1}, now),
               target::WriteDecision::ReplayOriginal),
        "same request replays original result despite full capacity or proposed expiry renewal");
  check(std::get<target::ResourceMutationResult>(original.result).version.value ==
            std::numeric_limits<std::uint64_t>::max(),
        "replay snapshot keeps complete uint64 version");
  proposed = original;
  proposed.fingerprint.bytes[0] = std::byte{1};
  check(failure_is(target::check_receipt_write(&original, proposed, 0, {1}, now),
                   target::ErrorCode::Conflict),
        "same receipt identity with different request hash conflicts");
  proposed = original;
  proposed.key.resource.kind = target::ResourceKind::Account;
  check(failure_is(target::check_receipt_write(&original, proposed, 0, {1}, now),
                   target::ErrorCode::Conflict),
        "different resource kinds never alias one receipt");
  proposed = original;
  proposed.retain_until -= 1s;
  check(failure_is(target::check_receipt_write(nullptr, proposed, 0, {1}, now),
                   target::ErrorCode::InvalidArgument),
        "retention shorter than seven days rejected");
  check(failure_is(target::check_receipt_write(nullptr, original, 0, {1}, original.retain_until),
                   target::ErrorCode::InvalidArgument),
        "expired receipt cannot be newly inserted");
  check(failure_is(target::minimum_receipt_retention(target::TimePoint::max()),
                   target::ErrorCode::InvalidArgument),
        "receipt retention arithmetic cannot overflow");
  const std::uint64_t high = (std::uint64_t{1} << 53) + 1;
  const auto next = target::next_version({high}, {high});
  check(std::get<target::ResourceVersion>(next).value == high + 1,
        "resource versions never round through double");
}
void handoff_rules(target::TimePoint now) {
  const auto original = outbox(now);
  const target::JournalCapacity capacity{1, 1, 1};
  check(equals(target::check_outbox_enqueue(nullptr, original, 0, capacity, now),
               target::WriteDecision::Insert),
        "new outbox record fits its own lane");
  check(failure_is(target::check_outbox_enqueue(nullptr, original, 1, capacity, now),
                   target::ErrorCode::RateLimited),
        "full ordinary outbox rejects new work");
  auto control = original;
  control.lane = target::OutboxLane::Control;
  check(equals(target::check_outbox_enqueue(nullptr, control, 0, capacity, now),
               target::WriteDecision::Insert),
        "reserved control lane remains usable when ordinary lane is full");
  check(equals(target::check_outbox_enqueue(&original, original, 1, capacity, now + 2h),
               target::WriteDecision::ReplayOriginal),
        "already recorded handoff remains a duplicate after expiry");
  auto changed = original;
  changed.context.payload_hash.bytes[0] = std::byte{1};
  check(failure_is(target::check_outbox_enqueue(&original, changed, 0, capacity, now),
                   target::ErrorCode::Conflict),
        "same source and ID with different hash cannot enqueue");
  changed = original;
  changed.context.valid_until += 1h;
  check(failure_is(target::check_outbox_enqueue(&original, changed, 0, capacity, now),
                   target::ErrorCode::Conflict),
        "same ID cannot extend its immutable validity window");
  changed = original;
  changed.payload_reference.version.value -= 1;
  check(failure_is(target::check_outbox_enqueue(&original, changed, 0, capacity, now),
                   target::ErrorCode::Conflict),
        "same envelope cannot silently point at a changed payload version");
  check(failure_is(target::check_outbox_enqueue(nullptr, original, 0, capacity, now + 1h),
                   target::ErrorCode::Conflict),
        "expired absent message rejected at exact expiry even after dedup cleanup");
  const auto received = inbox(now);
  check(equals(target::check_inbox_write(nullptr, received, 0, capacity, now),
               target::WriteDecision::Insert),
        "inbox accepts exact retention boundary");
  auto retry = received;
  retry.received_at += 2h;
  retry.retain_until += 2h;
  check(equals(target::check_inbox_write(&received, retry, 1, capacity, now + 2h),
               target::WriteDecision::ReplayOriginal),
        "accepted duplicate after expiry replays original receipt and does not renew retention");
  retry.context.payload_hash.bytes[1] = std::byte{1};
  check(failure_is(target::check_inbox_write(&received, retry, 0, capacity, now),
                   target::ErrorCode::Conflict),
        "same inbox source ID and changed hash conflict");
  retry = received;
  retry.retain_until -= 1s;
  check(failure_is(target::check_inbox_write(nullptr, retry, 0, capacity, now),
                   target::ErrorCode::InvalidArgument),
        "inbox retention includes valid_until plus seven days");
  check(failure_is(target::check_inbox_write(nullptr, received, 0, capacity, now + 1h),
                   target::ErrorCode::Conflict),
        "expired inbox absence never pretends to be a duplicate");
  check(failure_is(target::check_inbox_write(nullptr, received, 1, capacity, now),
                   target::ErrorCode::RateLimited),
        "inbox full rejects new acceptance");
  auto other_source = received;
  other_source.context.source = target::Service::Model;
  check(target::handoff_key(other_source.context) != target::handoff_key(received.context),
        "same message ID under another source is a different identity");
  check(equals(target::check_inbox_write(nullptr, other_source, 0, capacity, now),
               target::WriteDecision::Insert),
        "distinct source may independently use same message ID");
  check(failure_is(target::minimum_inbox_retention(now, target::TimePoint::max()),
                   target::ErrorCode::InvalidArgument),
        "inbox retention cannot overflow");
  check(equals(target::minimum_inbox_retention(now + 2h, now + 1h),
               now + 2h + target::kHandoffRetention),
        "retention uses later of receive time and validity time");
  auto invalid = handoff(now);
  invalid.resource.kind = static_cast<target::ResourceKind>(255);
  check(failure_is(target::validate_new_handoff(invalid, now), target::ErrorCode::InvalidArgument),
        "unknown resource kind rejected without inventing a future contract kind");
}
void claim_rules(target::TimePoint now) {
  const auto original = outbox(now);
  check(std::holds_alternative<std::monostate>(target::validate_claim_request(
            target::Service::Scheduler, target::Service::Scheduler, 100, 30s)),
        "claim upper bounds accepted");
  check(failure_is(target::validate_claim_request(target::Service::Scheduler,
                                                  target::Service::Scheduler, 101, 30s),
                   target::ErrorCode::InvalidArgument),
        "oversized claim batch rejected");
  check(failure_is(target::validate_claim_request(target::Service::Scheduler,
                                                  target::Service::Scheduler, 1, 31s),
                   target::ErrorCode::InvalidArgument),
        "oversized claim lease rejected");
  check(failure_is(target::validate_claim_request(target::Service::Scheduler,
                                                  target::Service::Runtime, 1, 30s),
                   target::ErrorCode::PermissionDenied),
        "claim cannot cross UoW ownership");
  check(failure_is(target::validate_claim_request(target::Service::Scheduler,
                                                  target::Service::Scheduler, 0, 30s),
                   target::ErrorCode::InvalidArgument),
        "zero batch rejected");
  check(failure_is(target::validate_claim_request(target::Service::Scheduler,
                                                  target::Service::Scheduler, 1, 0s),
                   target::ErrorCode::InvalidArgument),
        "zero lease rejected");
  check(failure_is(target::take_outbox_claim(original, target::Service::Scheduler, {}, now, 30s),
                   target::ErrorCode::InvalidArgument),
        "empty claim token rejected");
  auto corrupt = original;
  corrupt.state = target::OutboxState::Claimed;
  corrupt.lease_until = now + 30s;
  check(failure_is(target::classify_outbox(corrupt, target::Service::Scheduler, now),
                   target::ErrorCode::Internal),
        "stored claimed row without a token is corruption, not an empty queue");
  const auto first = std::get<target::OutboxClaim>(
      target::take_outbox_claim(original, target::Service::Scheduler, {"first"}, now, 30s));
  check(original.state == target::OutboxState::Pending && !original.claim_token,
        "proposed claim never mutates input before repository commit");
  check(equals(target::classify_outbox(first.entry, target::Service::Scheduler, now + 29s),
               target::ClaimDecision::Waiting),
        "live lease cannot be reclaimed");
  check(failure_is(target::take_outbox_claim(first.entry, target::Service::Scheduler, {"first"},
                                             now + 30s, 30s),
                   target::ErrorCode::Conflict),
        "reclaim cannot reuse previous token");
  const auto second = std::get<target::OutboxClaim>(target::take_outbox_claim(
      first.entry, target::Service::Scheduler, {"second"}, now + 30s, 30s));
  check(failure_is(target::check_confirmation(second.entry, target::handoff_key(original.context),
                                              {"first"}, now + 31s),
                   target::ErrorCode::Conflict),
        "late old worker cannot confirm newer claim");
  check(failure_is(target::check_confirmation(first.entry, target::handoff_key(original.context),
                                              {"first"}, now + 30s),
                   target::ErrorCode::Conflict),
        "expired token rejected at exact lease boundary");
  check(equals(target::check_confirmation(second.entry, target::handoff_key(original.context),
                                          {"second"}, now + 31s),
               target::ConfirmationDecision::Apply),
        "current unexpired claim can confirm");
  auto wrong_source = target::handoff_key(original.context);
  wrong_source.source = target::Service::Model;
  check(failure_is(target::check_confirmation(second.entry, wrong_source, {"second"}, now + 31s),
                   target::ErrorCode::Conflict),
        "confirmation key includes source");
  auto confirmed = second.entry;
  confirmed.state = target::OutboxState::Confirmed;
  check(equals(target::check_confirmation(confirmed, target::handoff_key(original.context),
                                          {"second"}, now + 2h),
               target::ConfirmationDecision::Duplicate),
        "same confirmed token replays fact after lease expiry");
  auto exhausted = original;
  exhausted.attempts = target::kMaximumHandoffAttempts;
  check(equals(target::classify_outbox(exhausted, target::Service::Scheduler, now),
               target::ClaimDecision::NeedsAttention),
        "one hundred attempts require attention without dropping record");
  check(failure_is(
            target::take_outbox_claim(exhausted, target::Service::Scheduler, {"new"}, now, 30s),
            target::ErrorCode::Conflict),
        "retry cap forbids automatic resend");
  exhausted.attempts = std::numeric_limits<std::uint32_t>::max();
  check(equals(target::classify_outbox(exhausted, target::Service::Scheduler, now),
               target::ClaimDecision::NeedsAttention),
        "corrupt oversized attempt count cannot overflow into a fresh attempt");
  auto almost_expired = original;
  almost_expired.context.valid_until = now + 5s;
  const auto bounded = std::get<target::OutboxClaim>(
      target::take_outbox_claim(almost_expired, target::Service::Scheduler, {"bounded"}, now, 30s));
  check(bounded.lease_until == now + 5s, "claim lease cannot outlive message validity");
}
int main() {
  const target::TimePoint now{24h};
  receipt_rules(now);
  handoff_rules(now);
  claim_rules(now);
}
