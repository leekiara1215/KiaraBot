#pragma once

#include <optional>
#include <string_view>

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::core::target {

inline constexpr std::string_view kSetAssignmentFingerprintDomain =
    "kiarabot.identity.SetAssignment.v1";
inline constexpr std::string_view kRevokeAssignmentFingerprintDomain =
    "kiarabot.identity.RevokeAssignment.v1";

// Mature outer fingerprint implementation only; no DB/authorization/network or raw secrets.
// Use unambiguous typed canonical encoding with the fixed method domain. Include target presence
// and ID, spec.subject, basis tag, role OR sorted permissions, sorted exact resources, and expected
// version presence/value. Revoke includes its ID and expected version. Missing optionals cannot
// collide with present empty values. Exclude request/trace/deadline/idempotency key and
// credentials. Only structurally valid closed, nonempty, duplicate-free specs/IDs and positive
// versions are accepted; semantically identical permission/resource orderings yield the SAME
// digest. Invalid input -> InvalidArgument/CorrectInput; dependency/resource failure ->
// UpstreamUnavailable, not an all-zero digest. The application independently validates owner kind
// and current authority. All methods are called OUTSIDE every UoW. The returned digest is
// server-produced request content, never accepted from a client, and cannot replace current
// authorization before receipt replay.
class IdentityAssignmentFingerprintPort {
public:
  virtual ~IdentityAssignmentFingerprintPort() = default;
  virtual Outcome<Sha256Digest> set_assignment(const ExactAssignmentSpec& spec,
                                               std::optional<AssignmentId> target,
                                               std::optional<ResourceVersion> expected) = 0;
  virtual Outcome<Sha256Digest> revoke_assignment(const AssignmentId& target,
                                                  ResourceVersion expected) = 0;
};

} // namespace kiarabot::core::target
