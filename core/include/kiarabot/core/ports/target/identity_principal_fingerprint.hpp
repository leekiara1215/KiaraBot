#pragma once

#include <string_view>
#include <vector>

#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::core::target {

inline constexpr std::string_view kRegisterPrincipalFingerprintDomain =
    "kiarabot.identity.RegisterPrincipal.v1";
inline constexpr std::string_view kRevokeCredentialFingerprintDomain =
    "kiarabot.identity.RevokeCredential.v1";

// Mature outer fingerprint implementation only; no DB/authorization/network or raw secrets.
// Canonical typed encoding covers the fixed method domain, principal kind, exact binding
// (kind, id) and the sorted duplicate-free allowed service set. Ordering is equivalent; duplicate
// or unknown services, unknown kinds and invalid bindings are InvalidArgument. Excludes
// request/trace/deadline/idempotency key. Called OUTSIDE every UoW; a digest is server-produced
// request content, never accepted from a client and never replaces current authorization on
// replay. Dependency/resource failure is UpstreamUnavailable, not an all-zero digest.
class IdentityPrincipalFingerprintPort {
public:
  virtual ~IdentityPrincipalFingerprintPort() = default;
  virtual Outcome<Sha256Digest>
  register_principal(PrincipalKind kind, const ResourceRef& binding,
                     const std::vector<Service>& allowed_services) = 0;
  // RevokeCredential has no caller business payload beyond the target credential and expected
  // version; both are folded into the keyed domain framing so the same idempotency key with a
  // different expected version conflicts instead of replaying the original success.
  virtual Outcome<Sha256Digest> revoke_credential(const CredentialId& target,
                                                  const ResourceVersion& expected) = 0;
};

} // namespace kiarabot::core::target
