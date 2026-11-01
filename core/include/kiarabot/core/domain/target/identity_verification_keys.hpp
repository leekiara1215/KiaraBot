#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "kiarabot/core/domain/target/identity_types.hpp"

namespace kiarabot::core::target {

// Public verification material only; never PEM/private bytes or token-selected key sources.
// version must be positive and valid_until finite. The mature outer adapter exports raw32.
struct PublicVerificationKey {
  CryptoKeyRef reference;
  std::array<std::byte, 32> ed25519_public_key;
  ResourceVersion version;
  TimePoint valid_until;
};
struct VerificationKeySet {
  std::vector<PublicVerificationKey> keys;
  ResourceVersion version;
  // Trusted offline Identity deployment UUID Catalog anchor, never "all"/"keys" or caller input.
  ResourceRef keyset_resource;
};

} // namespace kiarabot::core::target
