#pragma once

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
  virtual Outcome<Sha256Digest> sha256(std::string_view value) = 0;
};

} // namespace kiarabot::core::target
