#pragma once

#include <filesystem>
#include <optional>

#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"

namespace kiarabot::infrastructure::identity_crypto {
// Trusted composition-root metadata only. Parent directories must be trusted; O_NOFOLLOW
// protects the final component. No file creation, rotation or path/content diagnostics.
struct TrustedSymmetricKeyFile {
  core::target::CryptoKeyRef reference;
  std::filesystem::path hex_file;
};
struct TrustedDelegationKeyFiles {
  core::target::CryptoKeyRef reference;
  std::filesystem::path public_pem_file;
  std::optional<std::filesystem::path> private_pem_file;
};
core::target::Outcome<SymmetricKey> load_symmetric_key_file(const TrustedSymmetricKeyFile& file);
core::target::Outcome<DelegationKey>
load_delegation_key_files(const TrustedDelegationKeyFiles& files);
} // namespace kiarabot::infrastructure::identity_crypto
