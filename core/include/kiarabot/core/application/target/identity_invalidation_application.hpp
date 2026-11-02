#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/domain/target/identity_invalidation.hpp"
#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/ports/target/identity_repository.hpp"

namespace kiarabot::core::target {

struct InvalidationWatchRequest {
  IdentityRequestContext context;
  VerifiedPeer peer;
  std::vector<ResourceRef> scope;
  InvalidationCursor cursor;
};
struct InvalidationWatchStart {
  InvalidationWatermark watermark;
  // Set when the durable log cannot prove continuity: the caller MUST take a full
  // GetAuthorizationSnapshot and only then apply later records.
  bool resync_required = false;
  std::vector<InvalidationRecord> backlog;
};
struct InvalidationPollRequest {
  IdentityRequestContext context;
  VerifiedPeer peer;
  std::vector<ResourceRef> scope;
  std::string epoch;
  std::uint64_t after_sequence = 0;
  std::size_t limit = 32;
};
struct InvalidationPollReply {
  InvalidationWatermark watermark;
  bool resync_required = false;
  std::vector<InvalidationRecord> records;
};
struct AuthorizationSnapshotRequest {
  IdentityRequestContext context;
  VerifiedPeer peer;
  std::vector<ResourceRef> scope;
  InvalidationCursor after;
};
struct AuthorizationSnapshotReply {
  InvalidationCursor cursor;
  ResourceVersion authorization_version;
  std::vector<AssignmentProjection> assignments;
  TimePoint checked_at;
};

// Read-only online subscription/snapshot use cases over the Identity invalidation log. Every call
// re-authenticates the CURRENT platform peer and current exact IdentityInvalidationRead grants;
// no cached subscription authority and no fabricated empty stream/snapshot. The durable sequence
// is the only ordering authority; online CheckAuthorization remains the final authority for
// high-risk decisions, so this stream is an accelerator only.
class IdentityInvalidationApplication {
public:
  virtual ~IdentityInvalidationApplication() = default;
  virtual Outcome<InvalidationWatchStart> open_watch(const InvalidationWatchRequest&) = 0;
  virtual Outcome<InvalidationPollReply> poll(const InvalidationPollRequest&) = 0;
  virtual Outcome<AuthorizationSnapshotReply> snapshot(const AuthorizationSnapshotRequest&) = 0;
};

struct IdentityInvalidationOptions {
  // Trusted composition policy for a bounded poll/backlog batch.
  std::size_t batch = 32;
};

Outcome<std::unique_ptr<IdentityInvalidationApplication>>
make_identity_invalidation_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                       RandomPort& random, IdentityInvalidationOptions options);

// Shared transaction helper for owners that must write the invalidation in the SAME physical UoW
// as the assignment/credential change (SetAssignment, RevokeAssignment, later credential revoke).
// Ensures the durable epoch exists in this transaction and appends one committed record.
Outcome<std::string> ensure_identity_invalidation_epoch(IdentitySecurityUnitOfWork& work,
                                                        RandomPort& random);
Outcome<InvalidationRecord> append_identity_invalidation(IdentitySecurityUnitOfWork& work,
                                                         RandomPort& random,
                                                         const InvalidationEvent& event,
                                                         TimePoint now);

} // namespace kiarabot::core::target
