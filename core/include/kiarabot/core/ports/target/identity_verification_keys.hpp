#pragma once

#include "kiarabot/core/domain/target/identity_verification_keys.hpp"

namespace kiarabot::core::target {

// Immutable trusted deployment material, bounded and public-only. No authorization/DB/network
// calls; do not load token jku/x5u or synthesize versions/expiry. Expired active key, empty set,
// untrusted time or dependency failure returns Failure (UpstreamUnavailable), never fake empty.
class VerificationKeyMaterialPort {
public:
  virtual ~VerificationKeyMaterialPort() = default;
  virtual Outcome<VerificationKeySet> snapshot() = 0;
};
// GetVerificationKeys application (separate capability) requires current registered platform mTLS
// plus explicit IdentityKeysRead at keyset_resource using the Identity DB, WITHOUT a JWT precheck.
// Equal/older/absent known version returns the complete current set; future version is
// InvalidArgument/CorrectInput. The material port itself neither authorizes nor implements RPC.

} // namespace kiarabot::core::target
