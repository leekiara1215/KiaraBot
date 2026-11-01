#pragma once

#include <algorithm>
#include <cstddef>

#include "kiarabot/core/domain/target/invariants.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"

namespace kiarabot::core::target {

// Stateless decisions, not a memory repository. Callers must distinguish failed reads from absent
// rows before passing nullptr. Real implementations lock/condition unique keys, counters and
// versions and recheck database UTC in the same transaction. No helper grants authorization.
enum class WriteDecision { Insert, ReplayOriginal };
enum class ClaimDecision { Eligible, Waiting, Confirmed, Expired, NeedsAttention };
enum class ConfirmationDecision { Apply, Duplicate };

inline bool valid_service(Service service) {
  switch (service) {
  case Service::Identity:
  case Service::Runtime:
  case Service::Host:
  case Service::Scheduler:
  case Service::Model:
  case Service::Control:
    return true;
  }
  return false;
}
inline bool valid_resource_ref(const ResourceRef& resource) {
  if (resource.id.value.empty()) {
    return false;
  }
  switch (resource.kind) {
  case ResourceKind::Account:
  case ResourceKind::Adapter:
  case ResourceKind::PluginType:
  case ResourceKind::Installation:
  case ResourceKind::Instance:
  case ResourceKind::Host:
  case ResourceKind::Namespace:
  case ResourceKind::Task:
  case ResourceKind::Model:
  case ResourceKind::Principal:
  case ResourceKind::Session:
  case ResourceKind::Action:
  case ResourceKind::Operation:
  case ResourceKind::Credential:
  case ResourceKind::Assignment:
  case ResourceKind::Catalog:
    return true;
  }
  return false;
}
inline bool valid_operation_ref(const OperationRef& operation) {
  return valid_service(operation.owner) && !operation.id.value.empty() &&
         valid_resource_ref(operation.resource);
}
inline HandoffKey handoff_key(const HandoffContext& context) {
  return {context.source, context.message};
}
inline bool same_handoff(const HandoffContext& stored, const HandoffContext& proposed) {
  return handoff_key(stored) == handoff_key(proposed) && stored.operation == proposed.operation &&
         stored.causation == proposed.causation && stored.resource == proposed.resource &&
         stored.version == proposed.version && stored.actor == proposed.actor &&
         stored.authorization_version == proposed.authorization_version &&
         stored.payload_hash == proposed.payload_hash && stored.created_at == proposed.created_at &&
         stored.valid_until == proposed.valid_until;
}
inline Outcome<TimePoint> minimum_receipt_retention(TimePoint created_at) {
  if (created_at > TimePoint::max() - kMinimumReceiptRetention) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return created_at + kMinimumReceiptRetention;
}
inline Outcome<TimePoint> minimum_inbox_retention(TimePoint received_at, TimePoint valid_until) {
  const auto latest = std::max(received_at, valid_until);
  if (latest > TimePoint::max() - kHandoffRetention) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return latest + kHandoffRetention;
}
inline Status validate_handoff(const HandoffContext& context) {
  if (!valid_service(context.source) || context.message.value.empty() ||
      !valid_operation_ref(context.operation) || !valid_resource_ref(context.resource) ||
      context.version.value == 0 || context.actor.value.empty() ||
      (context.causation && context.causation->value.empty()) ||
      context.valid_until <= context.created_at) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  return std::monostate{};
}
inline Status validate_new_handoff(const HandoffContext& context, TimePoint now) {
  const auto validation = validate_handoff(context);
  if (std::holds_alternative<Failure>(validation)) {
    return validation;
  }
  if (context.created_at > now) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (context.valid_until <= now) {
    return rejected(ErrorCode::Conflict);
  }
  return std::monostate{};
}
inline Outcome<WriteDecision> check_receipt_write(const CommandReceipt* stored,
                                                  const CommandReceipt& proposed, std::size_t used,
                                                  ReceiptCapacity capacity, TimePoint now) {
  const auto& key = proposed.key;
  if (key.subject.value.empty() || key.method.empty() || key.idempotency_key.empty() ||
      !valid_resource_ref(key.resource)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (stored) {
    if (stored->key != key || stored->fingerprint != proposed.fingerprint) {
      return rejected(ErrorCode::Conflict);
    }
    // Preserve original result/timestamps even at full capacity or after an attempted TTL renewal.
    return WriteDecision::ReplayOriginal;
  }
  const auto minimum = minimum_receipt_retention(proposed.created_at);
  if (std::holds_alternative<Failure>(minimum) || proposed.created_at > now ||
      proposed.retain_until <= now || proposed.retain_until < std::get<TimePoint>(minimum)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (used >= capacity.max_records) {
    return rejected(ErrorCode::RateLimited, Recovery::Backoff);
  }
  return WriteDecision::Insert;
}
inline Outcome<WriteDecision> check_outbox_enqueue(const OutboxEntry* stored,
                                                   const OutboxEntry& proposed,
                                                   std::size_t lane_used, JournalCapacity capacity,
                                                   TimePoint now) {
  if (stored) {
    if (!same_handoff(stored->context, proposed.context) || stored->lane != proposed.lane ||
        stored->payload_reference.resource != proposed.payload_reference.resource ||
        stored->payload_reference.version != proposed.payload_reference.version) {
      return rejected(ErrorCode::Conflict);
    }
    return WriteDecision::ReplayOriginal;
  }
  const auto validation = validate_new_handoff(proposed.context, now);
  if (const auto* failure = std::get_if<Failure>(&validation)) {
    return *failure;
  }
  if (!valid_resource_ref(proposed.payload_reference.resource) ||
      proposed.payload_reference.version.value == 0 || proposed.state != OutboxState::Pending ||
      proposed.attempts != 0 || proposed.claim_token || proposed.lease_until ||
      (proposed.lane != OutboxLane::Ordinary && proposed.lane != OutboxLane::Control)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  const auto limit = proposed.lane == OutboxLane::Ordinary ? capacity.ordinary_outbox_records
                                                           : capacity.reserved_control_records;
  if (lane_used >= limit) {
    return rejected(ErrorCode::RateLimited, Recovery::Backoff);
  }
  return WriteDecision::Insert;
}
inline Outcome<WriteDecision> check_inbox_write(const InboxRecord* stored,
                                                const InboxRecord& proposed, std::size_t used,
                                                JournalCapacity capacity, TimePoint now) {
  if (stored) {
    if (!same_handoff(stored->context, proposed.context)) {
      return rejected(ErrorCode::Conflict);
    }
    // Replaying an existing durable acceptance remains a fact after message expiry. Return the
    // original snapshot; do not extend retention. An expired ABSENT message is rejected below.
    return WriteDecision::ReplayOriginal;
  }
  const auto validation = validate_new_handoff(proposed.context, now);
  if (const auto* failure = std::get_if<Failure>(&validation)) {
    return *failure;
  }
  const auto minimum = minimum_inbox_retention(proposed.received_at, proposed.context.valid_until);
  if (std::holds_alternative<Failure>(minimum) || proposed.received_at > now ||
      proposed.received_at < proposed.context.created_at ||
      proposed.retain_until < std::get<TimePoint>(minimum)) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (used >= capacity.inbox_records) {
    return rejected(ErrorCode::RateLimited, Recovery::Backoff);
  }
  return WriteDecision::Insert;
}
inline Status validate_claim_request(Service source, Service owner, std::size_t count,
                                     std::chrono::seconds lease) {
  if (!valid_service(source) || !valid_service(owner) || count == 0 || count > kMaximumClaimBatch ||
      lease <= std::chrono::seconds::zero() || lease > kMaximumClaimLease) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (source != owner) {
    return rejected(ErrorCode::PermissionDenied);
  }
  return std::monostate{};
}
inline Outcome<ClaimDecision> classify_outbox(const OutboxEntry& entry, Service owner,
                                              TimePoint now) {
  const auto validation = validate_handoff(entry.context);
  if (const auto* failure = std::get_if<Failure>(&validation)) {
    return *failure;
  }
  if (entry.context.source != owner) {
    return rejected(ErrorCode::PermissionDenied);
  }
  if (entry.state == OutboxState::Confirmed) {
    return ClaimDecision::Confirmed;
  }
  if (entry.state == OutboxState::Expired || now >= entry.context.valid_until) {
    return ClaimDecision::Expired;
  }
  if (entry.state == OutboxState::NeedsAttention || entry.attempts >= kMaximumHandoffAttempts) {
    return ClaimDecision::NeedsAttention;
  }
  if (entry.state != OutboxState::Pending && entry.state != OutboxState::Claimed) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (entry.state == OutboxState::Claimed &&
      (!entry.lease_until || !entry.claim_token || entry.claim_token->value.empty() ||
       *entry.lease_until <= entry.context.created_at)) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  if (now < entry.context.created_at || now < entry.next_attempt_at ||
      (entry.state == OutboxState::Claimed && now < *entry.lease_until)) {
    return ClaimDecision::Waiting;
  }
  return ClaimDecision::Eligible;
}
// Propose a value transition only. Persist it in the caller's short transaction BEFORE sending.
// Token comes from the trusted implementation's CSPRNG, with persisted uniqueness.
inline Outcome<OutboxClaim> take_outbox_claim(const OutboxEntry& entry, Service owner,
                                              const ClaimToken& token, TimePoint now,
                                              std::chrono::seconds lease) {
  const auto request = validate_claim_request(entry.context.source, owner, 1, lease);
  if (const auto* failure = std::get_if<Failure>(&request)) {
    return *failure;
  }
  const auto decision = classify_outbox(entry, owner, now);
  if (const auto* failure = std::get_if<Failure>(&decision)) {
    return *failure;
  }
  if (std::get<ClaimDecision>(decision) != ClaimDecision::Eligible ||
      (entry.claim_token && token == *entry.claim_token)) {
    return rejected(ErrorCode::Conflict);
  }
  if (token.value.empty() || now > TimePoint::max() - lease) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  auto claimed = entry;
  claimed.state = OutboxState::Claimed;
  ++claimed.attempts;
  claimed.claim_token = token;
  claimed.lease_until = std::min(now + lease, entry.context.valid_until);
  return OutboxClaim{claimed, token, *claimed.lease_until};
}
inline Outcome<ConfirmationDecision> check_confirmation(const OutboxEntry& entry,
                                                        const HandoffKey& key,
                                                        const ClaimToken& token, TimePoint now) {
  if (handoff_key(entry.context) != key || !entry.claim_token || *entry.claim_token != token ||
      token.value.empty()) {
    return rejected(ErrorCode::Conflict);
  }
  if (entry.state == OutboxState::Confirmed) {
    return ConfirmationDecision::Duplicate;
  }
  if (entry.state != OutboxState::Claimed || !entry.lease_until || now >= *entry.lease_until ||
      now < entry.context.created_at) {
    return rejected(ErrorCode::Conflict);
  }
  return ConfirmationDecision::Apply;
}

} // namespace kiarabot::core::target
