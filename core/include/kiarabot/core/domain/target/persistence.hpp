#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

inline constexpr auto kMinimumReceiptRetention = std::chrono::hours{24 * 7};
inline constexpr auto kHandoffRetention = std::chrono::hours{24 * 7};
inline constexpr auto kMaximumClaimLease = std::chrono::seconds{30};
inline constexpr std::size_t kMaximumClaimBatch = 100;
inline constexpr std::uint32_t kMaximumHandoffAttempts = 100;
struct EmptySuccess {};
struct ResourceMutationResult {
  ResourceRef resource;
  ResourceVersion version;
};
// Immutable complete success snapshot, not a live-resource lookup. No transport bytes or secrets.
// Login/refresh/credential secret replay needs a separate encrypted P04 contract; not supported
// here.
using ReplayResult =
    std::variant<EmptySuccess, ResourceMutationResult, Operation, Principal, LoginSession,
                 ResourceGrant, BotAccount, PluginInstallation, PluginInstance, ScheduledTask,
                 Occurrence, ModelIntent, SessionMetadata>;
struct ReceiptKey {
  SubjectId subject;
  std::string method;
  ResourceRef resource;
  std::string idempotency_key;
  bool operator==(const ReceiptKey&) const = default;
};
struct CommandReceipt {
  ReceiptKey key;
  Sha256Digest fingerprint;
  ReplayResult result;
  TimePoint created_at;
  TimePoint retain_until;
};
// Trusted application policy only; transport requests cannot enlarge these limits.
struct ReceiptCapacity {
  std::size_t max_records = 10000;
};
struct ReceiptWrite {
  CommandReceipt receipt;
  bool duplicate;
};
struct AuditEntry {
  SubjectId actor;
  ResourceRef resource;
  std::string request_id;
  std::string trace_id;
  std::string method;
  std::optional<ErrorCode> rejection;
  ResourceVersion authorization_version;
  TimePoint occurred_at;
};
struct HandoffKey {
  Service source;
  MessageId message;
  bool operator==(const HandoffKey&) const = default;
};
struct ImmutablePayloadRef {
  ResourceRef resource;
  ResourceVersion version;
};
enum class OutboxLane { Ordinary, Control };
enum class OutboxState { Pending, Claimed, Confirmed, Expired, NeedsAttention };
struct OutboxEntry {
  HandoffContext context;
  // Owner must retain immutable payload at this version until handoff obligations are satisfied.
  ImmutablePayloadRef payload_reference;
  OutboxLane lane = OutboxLane::Ordinary;
  OutboxState state = OutboxState::Pending;
  std::uint32_t attempts = 0;
  TimePoint next_attempt_at;
  std::optional<ClaimToken> claim_token;
  std::optional<TimePoint> lease_until;
};
struct OutboxClaim {
  OutboxEntry entry;
  ClaimToken claim_token;
  TimePoint lease_until;
};
// Trusted application policy only; transport requests cannot enlarge these limits.
struct JournalCapacity {
  std::size_t ordinary_outbox_records = 10000;
  std::size_t reserved_control_records = 1024;
  std::size_t inbox_records = 10000;
};
struct InboxRecord {
  HandoffContext context;
  TimePoint received_at;
  TimePoint retain_until;
  ReplayResult result;
};
struct InboxWrite {
  InboxRecord receipt;
  bool duplicate;
};

} // namespace kiarabot::core::target
