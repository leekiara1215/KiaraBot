#pragma once

#include <memory>

#include "kiarabot/core/ports/target/identity_verification_keys.hpp"
#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"

namespace kiarabot::infrastructure::identity_crypto {
struct DelegationCryptoBundle {
  std::unique_ptr<core::target::DelegationCryptoPort> crypto;
  std::unique_ptr<core::target::VerificationKeyMaterialPort> verification_keys;
};
// First version accepts exactly one trusted Ed25519 key, positive key/set versions, finite
// expiry and an offline canonical lowercase UUIDv4 Catalog anchor. Both adapters consume the SAME
// configuration; there is no separate public-material configuration and no rotation/expiry
// fallback.
core::target::Outcome<DelegationCryptoBundle>
make_delegation_crypto_bundle(DelegationConfig config, core::target::SecurityClockPort& clock);
} // namespace kiarabot::infrastructure::identity_crypto
