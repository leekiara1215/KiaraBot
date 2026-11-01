#pragma once

#include <memory>
#include <string>
#include <vector>

#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

namespace kiarabot::infrastructure::identity_crypto {
struct DelegationKey {
  core::target::CryptoKeyRef reference;
  std::string public_key_pem;
  core::target::OwnedSecret private_key_pem;
};
struct DelegationConfig {
  std::string issuer;
  std::vector<DelegationKey> keys;
};
// Trusted immutable deployment keys only. Clock outlives the adapter; signing rejects untrusted
// samples. Verification never substitutes local wall clock for its explicit trusted sample.
core::target::Outcome<std::unique_ptr<core::target::DelegationCryptoPort>>
make_delegation_crypto(DelegationConfig config, core::target::SecurityClockPort& signing_clock);
} // namespace kiarabot::infrastructure::identity_crypto
