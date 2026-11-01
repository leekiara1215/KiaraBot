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
  core::target::ResourceVersion version;
  core::target::TimePoint valid_until;
};
// Text password: 1..1024 bytes after one optional trailing LF, no CR/LF/NUL.
// Same trusted owned secret-file checks; returned plaintext stays in secure memory.
core::target::Outcome<core::target::OwnedSecret>
load_bootstrap_password_file(const std::filesystem::path& trusted_path);
core::target::Outcome<SymmetricKey> load_symmetric_key_file(const TrustedSymmetricKeyFile& file);
core::target::Outcome<DelegationKey>
load_delegation_key_files(const TrustedDelegationKeyFiles& files);
} // namespace kiarabot::infrastructure::identity_crypto
