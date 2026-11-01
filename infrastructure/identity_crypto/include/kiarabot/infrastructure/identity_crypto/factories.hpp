#pragma once

#include <memory>
#include <vector>

#include "kiarabot/core/ports/target/identity_crypto.hpp"

namespace kiarabot::infrastructure::identity_crypto {

// Trusted composition-root input only. Keys must contain exactly 32 bytes and use secure
// allocation. Factories consume ownership; callers cannot mutate installed key material.
struct SymmetricKey {
  core::target::CryptoKeyRef reference;
  core::target::OwnedSecret material;
};

// Initializes libsodium and copies into sodium_malloc-backed, erased-on-release storage.
core::target::Outcome<core::target::OwnedSecret> copy_secret(core::target::SecretView secret);
core::target::Outcome<std::unique_ptr<core::target::RandomPort>> make_random();
core::target::Outcome<std::unique_ptr<core::target::PasswordCryptoPort>> make_password_crypto();
core::target::Outcome<std::unique_ptr<core::target::TokenCryptoPort>>
make_token_crypto(std::vector<SymmetricKey> keys);

// Authenticates retention timestamps and enforces the maximum retention window only. The
// application checks current database time, credential and rotation before every open/replay.
core::target::Outcome<std::unique_ptr<core::target::SecretReceiptCryptoPort>>
make_secret_receipt_crypto(std::vector<SymmetricKey> keys);

} // namespace kiarabot::infrastructure::identity_crypto
