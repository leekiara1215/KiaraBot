#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// Trusted outer-composition secret operations for the P06 external-bridge bootstrap. The
// application never touches a crypto library: the composition supplies a CSPRNG opaque credential
// and a one-way SHA-256 digest. Plaintext exists only in the single RPC response and is never
// persisted or logged; Runtime stores only the digest.
class RuntimeSecretPort {
public:
  virtual ~RuntimeSecretPort() = default;
  // 32 bytes of CSPRNG entropy encoded as 64 lowercase hexadecimal ASCII characters.
  virtual Outcome<std::string> bootstrap_credential() = 0;
  // 32 bytes of CSPRNG entropy as opaque binary material for short-lived bridge proofs and
  // refresh credentials. The value exists only in Runtime memory, never in storage or logs.
  virtual Outcome<std::array<std::byte, 32>> opaque_secret() = 0;
  // Hash exactly value.size() bytes (including NUL), for secrets and public handoff content.
  // Return the 32 SHA-256 bytes or the existing Failure; never a success fallback.
  virtual Outcome<Sha256Digest> sha256(std::string_view value) = 0;
};

} // namespace kiarabot::core::target
