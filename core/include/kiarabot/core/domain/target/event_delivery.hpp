#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/invariants.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

struct EventKey {
  ResourceId source;
  std::string event_id;
  bool operator==(const EventKey&) const = default;
};
struct DeliveryKey {
  EventKey event;
  InstanceId instance;
  bool operator==(const DeliveryKey&) const = default;
};
struct DeliveryAttempt {
  DeliveryKey delivery;
  AttemptId attempt;
  Fence fence;
  TimePoint started_at;
  TimePoint lease_until;
  // 1-based dispatch counter for this delivery target; the wire lease carries it for the SDK.
  std::uint32_t attempt_number = 1;
};

// Target P06 event/delivery rules shared by the Runtime application and the PG adapter.
// No protocol type, SQL or host clock appears here: callers inject authoritative database UTC
// and a uniform [0,1] jitter sample. Lease arithmetic itself stays in invariants.hpp.
inline constexpr std::uint32_t kMaximumDeliveryRetries = 5;
inline constexpr std::chrono::seconds kDeliveryLease{30};
inline constexpr std::chrono::seconds kDeliveryLeaseRenewal{10};
inline constexpr std::chrono::seconds kDeliveryLeaseLifetime{300};
inline constexpr std::size_t kDefaultEventCapacity = 1024;
inline constexpr std::size_t kDefaultInstanceDeliveryCapacity = 1024;
inline constexpr std::size_t kDefaultDeadLetterPage = 100;

// Wire EventEnvelope payload kinds that may be selected for plugin delivery. Adapter and
// runtime-only payloads are intentionally absent: TimerTriggered is created by Runtime itself
// and PluginStatusChanged has no target producer in P06.
enum class EventPayloadKind {
  MessageReceived,
  MessageSent,
  GroupMemberChanged,
  FriendRequest,
  AdapterStatusChanged,
  // Runtime-created one-shot task trigger; event_id equals the Scheduler occurrence id and the
  // target is the owning plugin instance, not a subscription fan-out.
  TimerTriggered,
};

inline std::string_view event_subscription(EventPayloadKind kind) noexcept {
  switch (kind) {
  case EventPayloadKind::MessageReceived:
    return "event.message.received";
  case EventPayloadKind::MessageSent:
    return "event.message.sent";
  case EventPayloadKind::GroupMemberChanged:
    return "event.group.member.changed";
  case EventPayloadKind::FriendRequest:
    return "event.friend.request";
  case EventPayloadKind::AdapterStatusChanged:
    return "event.adapter.status.changed";
  case EventPayloadKind::TimerTriggered:
    return {};
  }
  return {};
}

// Persisted sequence watermark for one conversation key. `degraded` is sticky until an
// operator/repair action resets it; no gap detection clears it automatically.
struct SequenceWatermark {
  std::optional<std::uint64_t> last_sequence;
  bool degraded = false;
  bool operator==(const SequenceWatermark&) const = default;
};

enum class SequenceVerdict { Accepted, OutOfOrder };

struct SequenceDecision {
  SequenceVerdict verdict = SequenceVerdict::Accepted;
  SequenceWatermark watermark;
};

// Apply doc/01-runtime-semantics "顺序和背压":
// - sequence 0/absent is treated as not provided: accepted but marks the conversation degraded;
// - no previous watermark: sequence 1 is contiguous, any other first value degrades;
// - value <= last_sequence is a non-duplicate out-of-order rejection (duplicates are detected by
//   event id/content fingerprint before this rule);
// - value == last+1 advances contiguously; a jump advances and degrades.
inline SequenceDecision observe_sequence(const std::optional<SequenceWatermark>& current,
                                         std::optional<std::uint64_t> sequence) {
  SequenceDecision decision;
  decision.watermark = current.value_or(SequenceWatermark{});
  const bool provided = sequence.has_value() && *sequence != 0;
  if (!provided) {
    decision.watermark.degraded = true;
    return decision;
  }
  if (decision.watermark.last_sequence.has_value()) {
    if (*sequence <= *decision.watermark.last_sequence) {
      decision.verdict = SequenceVerdict::OutOfOrder;
      return decision;
    }
    if (*sequence != *decision.watermark.last_sequence + 1) {
      decision.watermark.degraded = true;
    }
  } else if (*sequence != 1) {
    decision.watermark.degraded = true;
  }
  decision.watermark.last_sequence = *sequence;
  return decision;
}

// Event stored in the Runtime event inbox together with the opaque envelope bytes preserved
// exactly as received (dedupe/replay must not re-encode them; delivery decodes and rewrites the
// trace id from the delivery record). `sequence` is the wire value, where 0 means absent.
struct EventRecord {
  EventKey key;
  Sha256Digest fingerprint;
  ConversationKey conversation;
  EventPayloadKind payload = EventPayloadKind::MessageReceived;
  bool has_conversation = false;
  std::optional<std::uint64_t> sequence;
  bool has_sequence = false;
  std::string serialized_envelope;
  TimePoint occurred_at{};
  std::optional<TimePoint> retain_until;
};

// One independently persisted fan-out target. `ordered` comes from the accepted subscription
// snapshot at receive time; it never changes after acceptance.
struct DeliveryRecord {
  DeliveryKey delivery;
  bool ordered = false;
  std::string trace_id;
  std::uint32_t attempts = 0;
  std::optional<AttemptId> current_attempt;
};

enum class DeliveryState { Persisted, Retrying, Dispatched, Acknowledged, DeadLetter };

inline bool delivery_state_terminal(DeliveryState state) noexcept {
  return state == DeliveryState::Acknowledged || state == DeliveryState::DeadLetter;
}

struct StoredDelivery {
  DeliveryRecord record;
  DeliveryState state = DeliveryState::Persisted;
  TimePoint next_attempt_at{};
  std::optional<ErrorCode> dead_letter_reason;
  std::optional<TimePoint> updated_at;
};

// ACK/Renew result mapping frozen by doc/01-contracts P06 section.
enum class DeliveryDisposition { Acknowledged, Duplicate, Stale, Renewed, Rejected };

struct DeliveryReceipt {
  DeliveryAttempt attempt;
  DeliveryDisposition disposition = DeliveryDisposition::Stale;
  std::optional<Failure> failure;
  TimePoint observed_at{};
};

// Runtime-local retry policy: five retries after the first delivery. Delay sequence is
// 1/2/4/8/16 seconds with a uniform +/-20% jitter; the caller supplies the sample so tests are
// deterministic. A non-retryable failure is dead-lettered immediately.
struct RetryPolicy {
  std::uint32_t maximum_retries = kMaximumDeliveryRetries;
};

inline double clamp_jitter(double sample) noexcept {
  if (!(sample >= 0.0)) { // Also rejects NaN.
    return 0.0;
  }
  return std::min(sample, 1.0);
}

// retry_number is 1 for the delay after the first failed delivery. Values above the policy
// maximum retry count have no retry delay and must dead-letter instead.
inline std::chrono::nanoseconds retry_delay(std::uint32_t retry_number, double jitter_sample) {
  if (retry_number == 0) {
    return std::chrono::nanoseconds::zero();
  }
  const auto exponent = static_cast<std::uint32_t>(std::min<std::uint32_t>(retry_number - 1, 4));
  const std::uint64_t base_seconds = std::uint64_t{1} << exponent;
  const double factor = 1.0 + (clamp_jitter(jitter_sample) * 0.4 - 0.2);
  const double nanoseconds = static_cast<double>(base_seconds) * 1'000'000'000.0 * factor;
  return std::chrono::nanoseconds{static_cast<std::int64_t>(std::llround(nanoseconds))};
}

struct DeliveryFailureDecision {
  DeliveryState state = DeliveryState::Retrying;
  std::optional<TimePoint> next_attempt_at;
  std::optional<ErrorCode> dead_letter_reason;
};

// Apply one failed delivery. `record.attempts` must already include the attempt that just
// failed (claim increments it). Non-retryable failures and exhausted retries dead-letter:
// the first delivery plus five retries are attempted before dead-lettering.
inline DeliveryFailureDecision fail_delivery(const DeliveryRecord& record, bool retryable,
                                             ErrorCode reason, TimePoint now, double jitter_sample,
                                             const RetryPolicy& policy = {}) {
  DeliveryFailureDecision decision;
  if (!retryable || record.attempts > policy.maximum_retries) {
    decision.state = DeliveryState::DeadLetter;
    decision.dead_letter_reason = reason;
    return decision;
  }
  decision.state = DeliveryState::Retrying;
  decision.next_attempt_at = now + retry_delay(record.attempts, jitter_sample);
  return decision;
}

// Claim eligibility: persisted/retrying rows become dispatchable at next_attempt_at; a row is
// never claimed while a dispatcher attempt is still unexpired.
inline bool delivery_due(const StoredDelivery& delivery, TimePoint now) noexcept {
  if (delivery.state != DeliveryState::Persisted && delivery.state != DeliveryState::Retrying) {
    return false;
  }
  return delivery.next_attempt_at <= now;
}

// Ordered delivery head rule: a target may be dispatched only when no earlier non-terminal
// target exists for the same instance+conversation group.
struct OrderedHead {
  std::optional<std::uint64_t> ordinal;
  bool operator==(const OrderedHead&) const = default;
};

// ACK/Renew classification. The repository supplies the stored current attempt row for the
// delivery; these helpers are the only place that decides disposition semantics.
inline std::optional<DeliveryDisposition> classify_acknowledgement(DeliveryState state,
                                                                   const DeliveryAttempt& current,
                                                                   const DeliveryAttempt& supplied,
                                                                   TimePoint now) noexcept {
  const bool same_delivery = current.attempt == supplied.attempt &&
                             current.delivery.event.event_id == supplied.delivery.event.event_id &&
                             current.delivery.event.source == supplied.delivery.event.source &&
                             current.delivery.instance == supplied.delivery.instance;
  if (state == DeliveryState::Acknowledged) {
    return same_delivery ? std::optional<DeliveryDisposition>{DeliveryDisposition::Duplicate}
                         : std::optional<DeliveryDisposition>{DeliveryDisposition::Stale};
  }
  if (state != DeliveryState::Dispatched || !same_delivery || current.fence != supplied.fence ||
      now >= current.lease_until) {
    return DeliveryDisposition::Stale;
  }
  return DeliveryDisposition::Acknowledged;
}

inline std::optional<DeliveryDisposition> classify_renewal(const DeliveryAttempt& current,
                                                           const DeliveryAttempt& supplied,
                                                           TimePoint now) noexcept {
  if (current.attempt != supplied.attempt ||
      current.delivery.event.event_id != supplied.delivery.event.event_id ||
      current.delivery.event.source != supplied.delivery.event.source ||
      current.delivery.instance != supplied.delivery.instance || current.fence != supplied.fence ||
      now < current.started_at || now >= current.lease_until) {
    return DeliveryDisposition::Stale;
  }
  if (current.started_at + kDeliveryLeaseLifetime <= now) {
    return DeliveryDisposition::Stale;
  }
  return DeliveryDisposition::Renewed;
}

inline TimePoint renewed_lease_until(const DeliveryAttempt& current, TimePoint now) {
  const auto extended = now + kDeliveryLease;
  const auto limit = current.started_at + kDeliveryLeaseLifetime;
  return std::min(extended, limit);
}

// Exact fence/attempt identity required for ACK and Renew: event, instance, attempt id and the
// complete fence must match; the wire attempt never supplies a Fence the client may choose.
inline bool same_delivery_identity(const DeliveryKey& left, const DeliveryKey& right) noexcept {
  return left.event.source == right.event.source && left.event.event_id == right.event.event_id &&
         left.instance == right.instance;
}

// A plugin instance is an eligible fan-out target only when it is active, has a current
// registration fence and has applied the latest desired configuration. Initial instances are
// created with desired==applied==0, which passes without a P08 configuration delivery.
inline bool instance_accepts_events(const PluginInstance& instance) noexcept {
  return instance.state == LifecycleState::Active && instance.current.has_value() &&
         instance.applied_config_version.value >= instance.desired_version.value;
}

// Fan-out target selection over the subscription snapshot. Duplicate instance ids are collapsed
// to one target and instances are returned in ascending id order for deterministic storage
// tests.
inline std::vector<InstanceId>
delivery_targets(const std::vector<PluginInstance>& instances, std::string_view subscription,
                 const std::vector<std::string>& instance_subscriptions) {
  std::vector<InstanceId> targets;
  if (subscription.empty()) {
    return targets;
  }
  for (const auto& instance : instances) {
    if (!instance_accepts_events(instance)) {
      continue;
    }
    const bool subscribed =
        std::any_of(instance_subscriptions.begin(), instance_subscriptions.end(),
                    [subscription](const std::string& name) { return name == subscription; });
    if (subscribed) {
      targets.push_back(instance.id);
    }
  }
  std::sort(targets.begin(), targets.end(), [](const InstanceId& left, const InstanceId& right) {
    return left.value < right.value;
  });
  targets.erase(std::unique(targets.begin(), targets.end(),
                            [](const InstanceId& left, const InstanceId& right) {
                              return left.value == right.value;
                            }),
                targets.end());
  return targets;
}

// Capacity is a trusted application policy, never request controlled. `pending_events` counts
// non-terminal events; `pending_deliveries` counts per-instance non-terminal targets.
struct EventCapacityLimits {
  std::size_t events = kDefaultEventCapacity;
  std::size_t instance_deliveries = kDefaultInstanceDeliveryCapacity;
};
struct EventCapacity {
  std::size_t pending_events = 0;
  std::size_t maximum_instance_deliveries = 0;
  bool events_saturated = false;
  bool deliveries_saturated = false;
};
inline bool capacity_available(const EventCapacity& capacity) noexcept {
  return !capacity.events_saturated && !capacity.deliveries_saturated;
}

// Recovery after a Runtime restart: every Dispatched row whose dispatcher stopped becomes one
// retryable failure (backoff or dead letter); a delivered-but-unconfirmed event is redelivered.
inline DeliveryFailureDecision recover_dispatched(const StoredDelivery& delivery, TimePoint now,
                                                  double jitter_sample,
                                                  const RetryPolicy& policy = {}) {
  return fail_delivery(delivery.record, true, ErrorCode::Timeout, now, jitter_sample, policy);
}

// Dead letter projection used by query/replay; delivery_id is the event_id and the instance is
// carried by the attempt fence (frozen contract decision, no extra wire field).
struct DeadLetter {
  DeliveryKey delivery;
  DeliveryAttempt last_attempt;
  ErrorCode reason = ErrorCode::Timeout;
  TimePoint failed_at{};
};

// Replay only resets the selected target: attempts/trace/attempt id change, event id and other
// targets are untouched. Only dead-letter targets are replayable.
inline Status replay_dead_letter(const StoredDelivery& delivery) {
  if (delivery.state != DeliveryState::DeadLetter) {
    return Failure{ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::ReadCurrent, {}, {}};
  }
  return std::monostate{};
}

// Canonical, unambiguous four-tuple conversation key used for watermarks and ordered-delivery
// heads. The encoding is length-prefixed so adapter/account/conversation values containing the
// separator can never alias another key; it is storage-only, never a wire field.
inline std::string conversation_key_text(const ConversationKey& key) {
  const auto append = [](std::string& out, std::string_view part) {
    out += std::to_string(part.size());
    out += ':';
    out.append(part);
  };
  std::string text;
  append(text, key.adapter.value);
  append(text, key.account.value);
  append(text, key.conversation_id);
  append(text, key.kind == ConversationKey::Kind::Group     ? "1"
               : key.kind == ConversationKey::Kind::Private ? "2"
                                                            : "0");
  return text;
}
inline std::optional<ConversationKey> conversation_key_from_text(std::string_view text) {
  const auto next = [&text](std::string& out) -> bool {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator > std::string_view::size_type{20}) {
      return false;
    }
    std::size_t size = 0;
    for (const char digit : text.substr(0, separator)) {
      if (digit < '0' || digit > '9' || size > 1'000'000) {
        return false;
      }
      size = size * 10 + static_cast<std::size_t>(digit - '0');
    }
    text.remove_prefix(separator + 1);
    if (size > text.size()) {
      return false;
    }
    out.assign(text.substr(0, size));
    text.remove_prefix(size);
    return true;
  };
  ConversationKey key;
  std::string kind;
  if (!next(key.adapter.value) || !next(key.account.value) || !next(key.conversation_id) ||
      !next(kind) || !text.empty()) {
    return std::nullopt;
  }
  if (kind == "1") {
    key.kind = ConversationKey::Kind::Group;
  } else if (kind == "2") {
    key.kind = ConversationKey::Kind::Private;
  } else if (kind == "0") {
    key.kind = ConversationKey::Kind::Unspecified;
  } else {
    return std::nullopt;
  }
  return key;
}

} // namespace kiarabot::core::target
