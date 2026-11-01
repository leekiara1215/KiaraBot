// clang-format off
#include "kiarabot/infrastructure/identity_crypto/verification_keys.hpp"

#include <memory>
#include <utility>

#include <openssl/err.h>
#include <openssl/pem.h>
// clang-format on

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
t::Failure failure(t::ErrorCode code) {
  ERR_clear_error();
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
class VerificationKeys final : public t::VerificationKeyMaterialPort {
public:
  VerificationKeys(t::VerificationKeySet material, t::SecurityClockPort& clock)
      : material_(std::move(material)), clock_(clock) {}
  t::Outcome<t::VerificationKeySet> snapshot() override {
    try {
      auto sampled = clock_.sample();
      if (const auto* error = std::get_if<t::Failure>(&sampled)) {
        return *error;
      }
      const auto& now = std::get<t::SecurityClockSample>(sampled);
      if (!now.trusted || now.maximum_known_skew < std::chrono::milliseconds::zero() ||
          now.maximum_known_skew > t::kMaximumKnownClockSkew || now.utc < t::TimePoint{} ||
          now.utc >= material_.keys.front().valid_until) {
        return failure(t::ErrorCode::UpstreamUnavailable);
      }
      return material_;
    } catch (...) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
  }

private:
  const t::VerificationKeySet material_;
  t::SecurityClockPort& clock_;
};
} // namespace
core::target::Outcome<DelegationCryptoBundle>
make_delegation_crypto_bundle(DelegationConfig config, core::target::SecurityClockPort& clock) {
  try {
    if (config.keys.size() != 1 || config.keys.front().public_key_pem.empty() ||
        config.keys.front().public_key_pem.size() > 8192) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    const auto& key = config.keys.front();
    std::unique_ptr<BIO, decltype(&BIO_free)> input(
        BIO_new_mem_buf(key.public_key_pem.data(), static_cast<int>(key.public_key_pem.size())),
        BIO_free);
    if (!input) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> parsed(
        PEM_read_bio_PUBKEY(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!parsed || EVP_PKEY_is_a(parsed.get(), "ED25519") != 1) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    t::PublicVerificationKey projected{key.reference, {}, key.version, key.valid_until};
    std::size_t size = projected.ed25519_public_key.size();
    if (EVP_PKEY_get_raw_public_key(
            parsed.get(), reinterpret_cast<unsigned char*>(projected.ed25519_public_key.data()),
            &size) != 1 ||
        size != projected.ed25519_public_key.size()) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    t::VerificationKeySet material{{projected}, config.version, config.keyset_resource};
    // Validation, private/public pair checks and secure ownership all use this SAME configuration.
    // The projection is never returned unless the mature crypto factory accepts the entire input.
    auto crypto = make_delegation_crypto(std::move(config), clock);
    if (const auto* error = std::get_if<t::Failure>(&crypto)) {
      return *error;
    }
    auto keys = std::make_unique<VerificationKeys>(std::move(material), clock);
    return DelegationCryptoBundle{
        std::move(std::get<std::unique_ptr<t::DelegationCryptoPort>>(crypto)), std::move(keys)};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  } catch (...) {
    return failure(t::ErrorCode::InvalidArgument);
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
