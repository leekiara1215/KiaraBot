#pragma once

#include <memory>

#include "kiarabot/core/ports/target/model_ports.hpp"
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

namespace kiarabot::infrastructure::identity_crypto {
// Deployment-injected root-key AEAD adapter for Model provider credentials. The root key is the
// only input; sealed values never contain the root key and the adapter never logs/copies plaintext.
core::target::Outcome<std::unique_ptr<core::target::ModelSecretCryptoPort>>
make_model_secret_crypto(SymmetricKey root_key);
// Infrastructure SHA-256 for the canonical duplicate-call fingerprint.
core::target::Outcome<std::unique_ptr<core::target::ModelDigestPort>> make_model_digest();
} // namespace kiarabot::infrastructure::identity_crypto
