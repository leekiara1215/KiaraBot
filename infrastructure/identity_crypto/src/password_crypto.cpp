// clang-format off
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

#include <array>
#include <cstring>
#include <new>
#include <string_view>

#include <sodium.h>
// clang-format on

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace target = core::target;
constexpr std::size_t kMaximumSecret = 65536;
constexpr unsigned long long kOperations = 2;
constexpr std::size_t kMemory = std::size_t{64} * 1024 * 1024;

target::Failure failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}

class SodiumSecret final : public target::SecretBuffer {
public:
  explicit SodiumSecret(std::size_t size)
      : allocation_(sodium_malloc(size == 0 ? 1 : size)), size_(size) {
    if (allocation_ == nullptr) {
      throw std::bad_alloc{};
    }
  }
  ~SodiumSecret() override {
    sodium_free(allocation_);
  }
  SodiumSecret(const SodiumSecret&) = delete;
  SodiumSecret& operator=(const SodiumSecret&) = delete;
  std::span<const std::byte> bytes() const noexcept override {
    return {static_cast<const std::byte*>(allocation_), size_};
  }
  unsigned char* writable() noexcept {
    return static_cast<unsigned char*>(allocation_);
  }

private:
  void* allocation_;
  std::size_t size_;
};

class SodiumRandom final : public target::RandomPort {
public:
  target::Outcome<target::OwnedSecret> bytes(std::size_t count) override {
    if (count == 0 || count > kMaximumSecret) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    try {
      auto secret = std::make_unique<SodiumSecret>(count);
      randombytes_buf(secret->writable(), count);
      return target::OwnedSecret{std::move(secret)};
    } catch (const std::bad_alloc&) {
      return failure(target::ErrorCode::UpstreamUnavailable);
    }
  }
};

bool canonical_base64(std::string_view encoded, std::size_t expected) {
  std::array<unsigned char, 32> decoded{};
  std::size_t decoded_size = 0;
  const char* end = nullptr;
  if (sodium_base642bin(decoded.data(), decoded.size(), encoded.data(), encoded.size(), nullptr,
                        &decoded_size, &end, sodium_base64_VARIANT_ORIGINAL_NO_PADDING) != 0 ||
      decoded_size != expected || end != encoded.data() + encoded.size()) {
    return false;
  }
  std::array<char, 45> canonical{};
  sodium_bin2base64(canonical.data(), canonical.size(), decoded.data(), decoded_size,
                    sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
  return encoded == canonical.data();
}

class SodiumPassword final : public target::PasswordCryptoPort {
public:
  target::Outcome<target::PasswordVerifier> derive(target::SecretView password) override try {
    if (password.empty() || password.size() > kMaximumSecret) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    char encoded[crypto_pwhash_STRBYTES]{};
    if (crypto_pwhash_str_alg(encoded, reinterpret_cast<const char*>(password.data()),
                              password.size(), kOperations, kMemory,
                              crypto_pwhash_ALG_ARGON2ID13) != 0) {
      return failure(target::ErrorCode::UpstreamUnavailable);
    }
    return target::PasswordVerifier{encoded, {1}};
  } catch (const std::bad_alloc&) {
    return failure(target::ErrorCode::UpstreamUnavailable);
  }
  target::Outcome<bool> verify(target::SecretView password,
                               const target::PasswordVerifier& stored) override {
    // Only the fixed supported policy reaches the expensive library verifier. PHC parsing is
    // deliberately strict: bounded canonical salt/hash base64 and no trailing fields or NULs.
    constexpr std::string_view kPrefix = "$argon2id$v=19$m=65536,t=2,p=1$";
    const std::string_view encoded = stored.encoded;
    if (stored.hash_policy_version.value != 1 || encoded.size() != kPrefix.size() + 22 + 1 + 43 ||
        !encoded.starts_with(kPrefix) || encoded[kPrefix.size() + 22] != '$') {
      return failure(target::ErrorCode::Internal);
    }
    for (std::size_t index = kPrefix.size(); index < encoded.size(); ++index) {
      if (index == kPrefix.size() + 22) {
        continue;
      }
      const char value = encoded[index];
      if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '+' || value == '/')) {
        return failure(target::ErrorCode::Internal);
      }
    }
    if (!canonical_base64(encoded.substr(kPrefix.size(), 22), 16) ||
        !canonical_base64(encoded.substr(kPrefix.size() + 23), 32)) {
      return failure(target::ErrorCode::Internal);
    }
    if (password.empty() || password.size() > kMaximumSecret) {
      return failure(target::ErrorCode::InvalidArgument);
    }
    return crypto_pwhash_str_verify(stored.encoded.c_str(),
                                    reinterpret_cast<const char*>(password.data()),
                                    password.size()) == 0;
  }
};
} // namespace

core::target::Outcome<core::target::OwnedSecret> copy_secret(core::target::SecretView secret) {
  if (sodium_init() < 0) {
    return failure(core::target::ErrorCode::UpstreamUnavailable);
  }
  if (secret.size() > kMaximumSecret) {
    return failure(core::target::ErrorCode::InvalidArgument);
  }
  try {
    auto copy = std::make_unique<SodiumSecret>(secret.size());
    if (!secret.empty()) {
      std::memcpy(copy->writable(), secret.data(), secret.size());
    }
    return core::target::OwnedSecret{std::move(copy)};
  } catch (const std::bad_alloc&) {
    return failure(core::target::ErrorCode::UpstreamUnavailable);
  }
}

core::target::Outcome<std::unique_ptr<core::target::RandomPort>> make_random() try {
  if (sodium_init() < 0) {
    return failure(core::target::ErrorCode::UpstreamUnavailable);
  }
  return std::unique_ptr<core::target::RandomPort>{std::make_unique<SodiumRandom>()};
} catch (const std::bad_alloc&) {
  return failure(core::target::ErrorCode::UpstreamUnavailable);
}
core::target::Outcome<std::unique_ptr<core::target::PasswordCryptoPort>>
make_password_crypto() try {
  if (sodium_init() < 0) {
    return failure(core::target::ErrorCode::UpstreamUnavailable);
  }
  return std::unique_ptr<core::target::PasswordCryptoPort>{std::make_unique<SodiumPassword>()};
} catch (const std::bad_alloc&) {
  return failure(core::target::ErrorCode::UpstreamUnavailable);
}
} // namespace kiarabot::infrastructure::identity_crypto
