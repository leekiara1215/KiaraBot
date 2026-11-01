#include <array>
#include <sodium.h>
#include <string_view>
#include <utility>

#include "kiarabot/infrastructure/identity_crypto/factories.hpp"

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace target = core::target;
target::Failure invalid(target::ErrorCode code = target::ErrorCode::InvalidArgument) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}
class Mac {
public:
  explicit Mac(target::SecretView key) {
    crypto_auth_hmacsha256_init(&state_, reinterpret_cast<const unsigned char*>(key.data()),
                                key.size());
  }
  ~Mac() {
    sodium_memzero(&state_, sizeof(state_));
  }
  Mac(const Mac&) = delete;
  Mac& operator=(const Mac&) = delete;
  void integer(std::uint64_t value) {
    std::array<unsigned char, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      encoded[7 - index] = static_cast<unsigned char>(value & 255);
      value >>= 8;
    }
    crypto_auth_hmacsha256_update(&state_, encoded.data(), encoded.size());
  }
  void field(target::SecretView value) {
    integer(value.size());
    crypto_auth_hmacsha256_update(&state_, reinterpret_cast<const unsigned char*>(value.data()),
                                  value.size());
  }
  void field(std::string_view value) {
    field(std::as_bytes(std::span{value.data(), value.size()}));
  }
  target::Sha256Digest finish() {
    target::Sha256Digest digest;
    crypto_auth_hmacsha256_final(&state_, reinterpret_cast<unsigned char*>(digest.bytes.data()));
    return digest;
  }

private:
  crypto_auth_hmacsha256_state state_{};
};
bool bounded(std::string_view value) {
  return !value.empty() && value.size() <= 1024;
}

class SodiumToken final : public target::TokenCryptoPort {
public:
  explicit SodiumToken(std::vector<SymmetricKey> keys) : keys_(std::move(keys)) {}
  target::Outcome<target::TokenVerifier> make_verifier(target::SecretView token,
                                                       target::TokenPurpose purpose,
                                                       const target::CryptoKeyRef& key) override {
    const auto* material = find(key);
    if (material == nullptr) {
      return invalid(target::ErrorCode::UpstreamUnavailable);
    }
    if (token.empty() || token.size() > 65536 ||
        (purpose != target::TokenPurpose::Access && purpose != target::TokenPurpose::Refresh &&
         purpose != target::TokenPurpose::WorkloadBootstrap)) {
      return invalid();
    }
    Mac mac(material->bytes());
    mac.field("kiarabot.identity.token.v1");
    mac.field(key.value);
    mac.integer(static_cast<std::uint64_t>(purpose));
    mac.field(token);
    return target::TokenVerifier{purpose, key, mac.finish()};
  }
  target::Outcome<bool> verify(target::SecretView token,
                               const target::TokenVerifier& stored) override {
    auto result = make_verifier(token, stored.purpose, stored.key);
    if (const auto* error = std::get_if<target::Failure>(&result)) {
      return *error;
    }
    const auto& computed = std::get<target::TokenVerifier>(result);
    return sodium_memcmp(computed.tag.bytes.data(), stored.tag.bytes.data(), 32) == 0;
  }
  target::Outcome<target::RequestCommitment>
  commit_request(const target::SecretReceiptScope& scope,
                 const target::SecretRequestParameters& parameters, target::SecretView secret,
                 const target::CryptoKeyRef& key) override {
    const auto* material = find(key);
    if (material == nullptr) {
      return invalid(target::ErrorCode::UpstreamUnavailable);
    }
    if (scope.index() != parameters.index() || secret.size() > 65536) {
      return invalid();
    }
    Mac mac(material->bytes());
    mac.field("kiarabot.identity.request.v1");
    mac.field(key.value);
    mac.integer(scope.index());
    bool valid = true;
    std::visit(
        [&](const auto& typed) {
          if constexpr (requires { typed.control_peer; }) {
            valid = bounded(typed.control_peer.value);
            mac.field(typed.control_peer.value);
          } else {
            valid = bounded(typed.actor.value) && bounded(typed.credential.value);
            mac.field(typed.actor.value);
            mac.field(typed.credential.value);
          }
          if constexpr (requires { typed.normalized_login; }) {
            valid = valid && bounded(typed.normalized_login);
            mac.field(typed.normalized_login);
          }
          if constexpr (requires { typed.session; }) {
            valid = valid && bounded(typed.session.value);
            mac.field(typed.session.value);
          }
          valid = valid && bounded(typed.idempotency_key);
          mac.field(typed.idempotency_key);
        },
        scope);
    if (!valid) {
      return invalid();
    }
    mac.integer(parameters.index());
    std::visit(
        [&](const auto& typed) {
          if constexpr (requires { typed.input_rotation; }) {
            mac.integer(typed.input_rotation);
          }
          if constexpr (requires { typed.expected_version; }) {
            mac.integer(typed.expected_version.value);
          }
        },
        parameters);
    mac.field(secret);
    return target::RequestCommitment{key, mac.finish()};
  }

private:
  const target::SecretBuffer* find(const target::CryptoKeyRef& reference) const {
    for (const auto& key : keys_) {
      if (key.reference == reference) {
        return key.material.get();
      }
    }
    return nullptr;
  }
  const std::vector<SymmetricKey> keys_;
};
} // namespace

core::target::Outcome<std::unique_ptr<core::target::TokenCryptoPort>>
make_token_crypto(std::vector<SymmetricKey> keys) {
  if (sodium_init() < 0) {
    return invalid(core::target::ErrorCode::UpstreamUnavailable);
  }
  if (keys.empty() || keys.size() > 16) {
    return invalid();
  }
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const auto& key = keys[index];
    if (!bounded(key.reference.value) || !key.material || key.material->bytes().size() != 32) {
      return invalid();
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (keys[previous].reference == key.reference) {
        return invalid();
      }
    }
  }
  // Copy through our secure allocator rather than trusting an arbitrary SecretBuffer subclass.
  for (auto& key : keys) {
    auto copied = copy_secret(key.material->bytes());
    if (const auto* error = std::get_if<core::target::Failure>(&copied)) {
      return *error;
    }
    key.material = std::move(std::get<core::target::OwnedSecret>(copied));
  }
  return std::unique_ptr<core::target::TokenCryptoPort>{
      std::make_unique<SodiumToken>(std::move(keys))};
}
} // namespace kiarabot::infrastructure::identity_crypto
