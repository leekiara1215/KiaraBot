#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::core::target {
struct AuthorizationMethodPolicy {
  Service owner;
  Permission permission;
  std::vector<ResourceKind> scope_kinds;
};
// Trusted composition dependency, resolved from canonical contract method metadata outside Core.
// Never populate policy from request-supplied permissions/owner or infer it from method spelling.
// Missing/unsupported methods return empty; metadata/dependency faults remain Failure. No DB I/O.
class AuthorizationMethodPolicyPort {
public:
  virtual ~AuthorizationMethodPolicyPort() = default;
  virtual Outcome<std::optional<AuthorizationMethodPolicy>>
  resolve(std::string_view full_method) = 0;
};
} // namespace kiarabot::core::target
