#pragma once

#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"

namespace kiarabot::core::target {

// One online current-authorization check against Identity for a Runtime-owned method. The boundary
// already authenticated the direct peer and verified the delegation proof; this port only asks the
// authoritative Identity authority (never a cached local role table) for the current grant and
// authorization version. A missing/failed dependency must fail closed, never allow.
struct RuntimeAuthorizationRequest {
  AuthenticationContext authentication;
  Permission permission;
  std::string method;
  std::vector<ResourceRef> scope;
  TimePoint deadline;
};

// The owner validates this full binding before using an allow: the decision must name the same
// subject, method permission and exact resource scope the call asked for, and must be a current
// online observation (a stale or future-skewed checked_at is rejected). A decision cannot be
// replayed from another subject/permission/scope, and a test fake must satisfy the same shape.
struct RuntimeAuthorizationDecision {
  bool allowed = false;
  SubjectId subject;
  Permission permission = Permission::Unspecified;
  std::vector<ResourceRef> scope;
  ResourceVersion authorization_version;
  TimePoint checked_at;
  std::optional<Failure> denial;
};

class RuntimeAuthorizationPort {
public:
  virtual ~RuntimeAuthorizationPort() = default;
  // Readiness observation only: true only when a real current Identity authorization chain is
  // configured and verified. A fail-closed placeholder returns false; transport failures must not
  // be reported as available. Never used to authorize a request.
  virtual bool available() const noexcept = 0;
  virtual Outcome<RuntimeAuthorizationDecision>
  check(const RuntimeAuthorizationRequest& request) = 0;
};

} // namespace kiarabot::core::target
