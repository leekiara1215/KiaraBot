#include "kiarabot/infrastructure/identity_crypto/model_secret_crypto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sodium.h>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace target = core::target;

target::Failure failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}
class SodiumSecret final : public target::SecretBuffer {
public:
  explicit SodiumSecret(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
  ~SodiumSecret() override {
    sodium_memzero(bytes_.data(), bytes_.size());
  }
  std::span<const std::byte> bytes() const noexcept override {
    return bytes_;
  }

private:
  std::vector<std::byte> bytes_;
};
class ModelSecretCrypto final : public target::ModelSecretCryptoPort {
public:
  explicit ModelSecretCrypto(SymmetricKey root_key) {
    const auto material = root_key.material ? root_key.material->bytes() : target::SecretView{};
    if (material.size() != key_.size()) {
      throw std::runtime_error("invalid Model root key length");
    }
    std::memcpy(key_.data(), material.data(), key_.size());
  }
  ~ModelSecretCrypto() override {
    sodium_memzero(key_.data(), key_.size());
  }
  target::Outcome<std::vector<std::byte>> seal(target::SecretView plaintext,
                                               std::span<const std::byte> aad,
                                               std::array<std::byte, 24>& nonce) override {
    if (plaintext.empty() || plaintext.size() > 4096) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    randombytes_buf(nonce.data(), nonce.size());
    std::vector<std::byte> sealed(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char*>(sealed.data()), &length,
            reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(),
            reinterpret_cast<const unsigned char*>(aad.data()), aad.size(), nullptr,
            reinterpret_cast<const unsigned char*>(nonce.data()), key_.data()) != 0 ||
        length != sealed.size()) {
      return failure(target::ErrorCode::Internal);
    }
    return sealed;
  }
  target::Outcome<target::OwnedSecret> open(std::span<const std::byte> sealed,
                                            std::span<const std::byte> aad,
                                            const std::array<std::byte, 24>& nonce) override {
    if (sealed.size() <= crypto_aead_xchacha20poly1305_ietf_ABYTES || sealed.size() > 8192) {
      return failure(target::ErrorCode::Internal);
    }
    std::vector<std::byte> plaintext(sealed.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char*>(plaintext.data()), &length, nullptr,
            reinterpret_cast<const unsigned char*>(sealed.data()), sealed.size(),
            reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
            reinterpret_cast<const unsigned char*>(nonce.data()), key_.data()) != 0 ||
        length != plaintext.size()) {
      sodium_memzero(plaintext.data(), plaintext.size());
      return failure(target::ErrorCode::Internal);
    }
    return target::OwnedSecret{std::make_unique<SodiumSecret>(std::move(plaintext))};
  }

private:
  std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> key_{};
};
class ModelDigest final : public target::ModelDigestPort {
public:
  target::Outcome<target::Sha256Digest> sha256(std::span<const std::byte> bytes) override {
    target::Sha256Digest digest;
    if (crypto_hash_sha256(reinterpret_cast<unsigned char*>(digest.bytes.data()),
                           reinterpret_cast<const unsigned char*>(bytes.data()),
                           bytes.size()) != 0) {
      return failure(target::ErrorCode::Internal);
    }
    return digest;
  }
};
} // namespace

target::Outcome<std::unique_ptr<target::ModelSecretCryptoPort>>
make_model_secret_crypto(SymmetricKey root_key) {
  try {
    if (sodium_init() < 0) {
      return failure(target::ErrorCode::Internal);
    }
    return std::unique_ptr<target::ModelSecretCryptoPort>(
        std::make_unique<ModelSecretCrypto>(std::move(root_key)));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
target::Outcome<std::unique_ptr<target::ModelDigestPort>> make_model_digest() {
  try {
    if (sodium_init() < 0) {
      return failure(target::ErrorCode::Internal);
    }
    return std::unique_ptr<target::ModelDigestPort>(std::make_unique<ModelDigest>());
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
