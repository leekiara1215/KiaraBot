#pragma once

#include <memory>
#include <optional>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/ports/target/identity_verification_keys.hpp"

namespace kiarabot::core::target {
struct GetVerificationKeysRequest {
  IdentityRequestContext context;
  VerifiedPeer caller;
  std::optional<ResourceVersion> known_version;
};
struct GetVerificationKeysReply {
  VerificationKeySet keys;
  TimePoint checked_at;
};
// Public verification material requires current registered platform mTLS plus explicit exact
// IdentityKeysRead at the trusted Catalog anchor. No JWT is required to bootstrap verification.
// Concurrent calls own separate readonly UoWs; material access occurs outside ALL UoWs.
class IdentityVerificationKeysApplication {
public:
  virtual ~IdentityVerificationKeysApplication() = default;
  // Equal/older/absent known version returns the full current set. Future versions are rejected
  // only after current authorization. No success cache, empty fallback or database mutations.
  virtual Outcome<GetVerificationKeysReply>
  get_verification_keys(const GetVerificationKeysRequest& request) = 0;
};
// Both borrowed dependencies must outlive the application. Unexpected exceptions become Failure.
Outcome<std::unique_ptr<IdentityVerificationKeysApplication>>
make_identity_verification_keys_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                            VerificationKeyMaterialPort& material);
} // namespace kiarabot::core::target
