#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/domain/target/identity_assignment_state.hpp"
#include "kiarabot/core/ports/target/authorization_method_policy.hpp"
#include "kiarabot/core/ports/target/identity_assignment_fingerprint.hpp"
#include "kiarabot/core/ports/target/identity_repository.hpp"

namespace kiarabot::core::target {

// Enumeration filter for ListAssignments. Empty scope means "all scopes of the target subject".
// A nonempty scope keeps only assignments whose complete exact scope is inside the filter; it
// never widens authority and never joins permissions/resources into a product.
struct AssignmentScopeFilter {
  std::vector<ResourceRef> resources;
};
struct AssignmentPageRequest {
  // 0 -> default 20; maximum 100. Output-only wire fields must not be accepted here.
  std::uint32_t page_size = 0;
  std::string page_token;
  std::string snapshot_id;
};
struct ListAssignmentsRequest {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  SubjectId subject;
  AssignmentScopeFilter filter;
  AssignmentPageRequest page;
};
struct ListAssignmentsReply {
  std::vector<AssignmentProjection> assignments;
  std::uint32_t page_size = 0;
  std::string snapshot_id;
  std::string next_page_token;
  TimePoint expires_at;
  ResourceVersion authorization_version;
  TimePoint checked_at;
};

struct SetAssignmentRequest {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  ExactAssignmentSpec spec;
  // Absent on create (server generates the ID and version 1); updates require both fields.
  std::optional<AssignmentId> assignment_id;
  std::optional<ResourceVersion> expected_version;
};
struct SetAssignmentReply {
  ExactAssignment assignment;
  TimePoint checked_at;
};

struct RevokeAssignmentRequest {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  AssignmentId assignment_id;
  // Target declarations make the single expected version required; zero is invalid.
  std::optional<ResourceVersion> expected_version;
};
struct RevokeAssignmentReply {
  ResourceVersion version;
  TimePoint checked_at;
};

struct IdentityAssignmentOptions {
  // Trusted composition/configuration only. Zero capacities reject construction instead of
  // substituting an unlimited list snapshot or a fake success.
  std::size_t maximum_snapshots = 0;
  std::size_t maximum_snapshot_records = 0;
};

// P04-B management use cases over ONE authoritative Identity transaction per write. The caller
// supplies only a real currently authenticated Control peer; bodies never self-report the actor.
// Permanent assignment tombstones, current-scope management authority, typed receipts and the
// per-subject authorization watermark advance in the same physical UoW. Replays return the
// ORIGINAL success snapshot only after current authentication, never a mutable re-read.
// No protobuf/JSON/SQL/JWT objects enter this boundary.
class IdentityAssignmentApplication {
public:
  virtual ~IdentityAssignmentApplication() = default;
  virtual Outcome<ListAssignmentsReply> list_assignments(const ListAssignmentsRequest&) = 0;
  virtual Outcome<SetAssignmentReply> set_assignment(const SetAssignmentRequest&) = 0;
  virtual Outcome<RevokeAssignmentReply> revoke_assignment(const RevokeAssignmentRequest&) = 0;
};

// Borrowed dependencies outlive the application and all calls. Fingerprints are computed outside
// every UoW; each call owns its own short-lived transaction.
Outcome<std::unique_ptr<IdentityAssignmentApplication>>
make_identity_assignment_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                     IdentityAssignmentFingerprintPort& fingerprints,
                                     RandomPort& random, IdentityAssignmentOptions options);

} // namespace kiarabot::core::target
