#pragma once

#include <memory>

#include "kiarabot/core/ports/target/identity_principal_fingerprint.hpp"

namespace kiarabot::infrastructure::identity_crypto {

// Public, non-secret request content only. EVP SHA-256 with typed domain/length framing.
// The adapter is immutable and concurrent calls own independent digest contexts.
core::target::Outcome<std::unique_ptr<core::target::IdentityPrincipalFingerprintPort>>
make_principal_fingerprints();

} // namespace kiarabot::infrastructure::identity_crypto
