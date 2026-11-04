#include "kiarabot/infrastructure/identity_crypto/sha256.hpp"

#include <optional>

#include <openssl/err.h>
#include <openssl/evp.h>

namespace kiarabot::infrastructure::identity_crypto {

core::target::Outcome<core::target::Sha256Digest> Sha256::sha256(std::string_view value) {
  core::target::Sha256Digest digest;
  unsigned int length = 0;
  if (EVP_Digest(value.data(), value.size(), reinterpret_cast<unsigned char*>(digest.bytes.data()),
                 &length, EVP_sha256(), nullptr) != 1 ||
      length != digest.bytes.size()) {
    ERR_clear_error();
    return core::target::Failure{core::target::ErrorCode::UpstreamUnavailable,
                                 core::target::Acceptance::NotAccepted,
                                 core::target::Recovery::Backoff, std::nullopt, std::nullopt};
  }
  return digest;
}

} // namespace kiarabot::infrastructure::identity_crypto
