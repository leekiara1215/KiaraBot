// clang-format off
#include "kiarabot/infrastructure/identity_crypto/verification_keys.hpp"

#include <cassert>
#include <memory>
#include <string>

#include <openssl/pem.h>
// clang-format on

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace t = kiarabot::core::target;
class Clock final : public t::SecurityClockPort {
public:
  t::SecurityClockSample value{
      t::TimePoint{std::chrono::seconds{1000}}, {}, std::chrono::milliseconds{0}, true};
  t::Outcome<t::SecurityClockSample> sample() override {
    if (unavailable) {
      return t::Failure{t::ErrorCode::UpstreamUnavailable,
                        t::Acceptance::NotAccepted,
                        t::Recovery::Diagnose,
                        {},
                        {}};
    }
    return value;
  }
  bool unavailable = false;
};
struct Fixture {
  using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
  Key key{EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free};
  Bio public_bio{BIO_new(BIO_s_mem()), BIO_free};
  Bio private_bio{BIO_new(BIO_s_mem()), BIO_free};
  Clock clock;
  Fixture() {
    assert(key && public_bio && private_bio);
    assert(PEM_write_bio_PUBKEY(public_bio.get(), key.get()) == 1);
    assert(PEM_write_bio_PrivateKey(private_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                    nullptr) == 1);
  }
  crypto::DelegationConfig configuration(bool signing = true) {
    char* public_bytes = nullptr;
    const long public_size = BIO_get_mem_data(public_bio.get(), &public_bytes);
    assert(public_size > 0);
    crypto::DelegationKey configured{{"key"},
                                     {public_bytes, static_cast<std::size_t>(public_size)},
                                     {},
                                     {7},
                                     t::TimePoint{std::chrono::seconds{1056}}};
    if (signing) {
      char* private_bytes = nullptr;
      const long private_size = BIO_get_mem_data(private_bio.get(), &private_bytes);
      assert(private_size > 0);
      auto copied = crypto::copy_secret(
          std::as_bytes(std::span{private_bytes, static_cast<std::size_t>(private_size)}));
      configured.private_key_pem = std::move(std::get<t::OwnedSecret>(copied));
    }
    crypto::DelegationConfig result{
        "identity", {}, {11}, {t::ResourceKind::Catalog, {"55555555-5555-4555-8555-555555555555"}}};
    result.keys.push_back(std::move(configured));
    return result;
  }
};
int main() try {
  Fixture fixture;
  auto created = crypto::make_delegation_crypto_bundle(fixture.configuration(), fixture.clock);
  auto& bundle = std::get<crypto::DelegationCryptoBundle>(created);
  auto snapshot = bundle.verification_keys->snapshot();
  const auto& current = std::get<t::VerificationKeySet>(snapshot);
  assert(current.version.value == 11 && current.keys.size() == 1);
  assert(current.keys[0].reference.value == "key" && current.keys[0].version.value == 7);
  assert(current.keys[0].valid_until == t::TimePoint{std::chrono::seconds{1056}});
  assert(current.keyset_resource == fixture.configuration(false).keyset_resource);
  std::array<std::byte, 32> raw{};
  std::size_t size = raw.size();
  assert(EVP_PKEY_get_raw_public_key(fixture.key.get(),
                                     reinterpret_cast<unsigned char*>(raw.data()), &size) == 1 &&
         size == 32);
  assert(current.keys[0].ed25519_public_key == raw);
  t::DelegationClaims claims{
      "identity",
      t::Service::Runtime,
      {"human"},
      {"control"},
      {{"control"}},
      {{t::Permission::AccountRead, {t::ResourceKind::Account, {"account"}}}},
      {"proof"},
      {},
      {1},
      fixture.clock.value.utc,
      fixture.clock.value.utc,
      t::TimePoint{std::chrono::seconds{1056}},
      1,
      {{"credential"}, {1}},
      t::SessionId{"session"}};
  t::VerifiedPeer control{{t::PlatformPeer{t::Service::Control, {"control"}},
                           {{"control"},
                            {},
                            "spiffe://control",
                            t::Service::Control,
                            t::TimePoint{std::chrono::seconds{900}},
                            t::TimePoint{std::chrono::seconds{2000}}},
                           {{"control-key"}, {1}}},
                          fixture.clock.value.utc};
  auto signed_proof = bundle.crypto->sign(claims, {"key"});
  const auto& proof = std::get<t::OwnedSecret>(signed_proof);
  assert(std::holds_alternative<t::VerifiedDelegation>(
      bundle.crypto->verify(proof->bytes(), t::Service::Runtime, control, fixture.clock.value)));
  claims.expires_at += std::chrono::nanoseconds{1};
  assert(std::holds_alternative<t::Failure>(bundle.crypto->sign(claims, {"key"})));
  claims.expires_at = t::TimePoint{std::chrono::seconds{1057}};
  claims.issued_at = t::TimePoint{std::chrono::seconds{1001}};
  claims.not_before = claims.issued_at;
  fixture.clock.value.utc = claims.issued_at;
  // A valid 56-second NumericDate proof still cannot outlive the configured key.
  assert(std::holds_alternative<t::Failure>(bundle.crypto->sign(claims, {"key"})));
  fixture.clock.value.utc = t::TimePoint{std::chrono::seconds{1000}};
  claims.issued_at = fixture.clock.value.utc;
  claims.not_before = claims.issued_at;
  claims.expires_at = t::TimePoint{std::chrono::seconds{1056}};
  // Rebuild a public PEM exclusively from the published raw32; mature verifier roundtrip must work.
  Fixture::Key reconstructed(EVP_PKEY_new_raw_public_key_ex(
                                 nullptr, "ED25519", nullptr,
                                 reinterpret_cast<const unsigned char*>(raw.data()), raw.size()),
                             EVP_PKEY_free);
  Fixture::Bio exported(BIO_new(BIO_s_mem()), BIO_free);
  assert(reconstructed && exported &&
         PEM_write_bio_PUBKEY(exported.get(), reconstructed.get()) == 1);
  char* published = nullptr;
  const long published_size = BIO_get_mem_data(exported.get(), &published);
  auto verifier_config = fixture.configuration(false);
  verifier_config.keys[0].public_key_pem.assign(published,
                                                static_cast<std::size_t>(published_size));
  auto verify_only =
      crypto::make_delegation_crypto_bundle(std::move(verifier_config), fixture.clock);
  auto& verifier = std::get<crypto::DelegationCryptoBundle>(verify_only);
  assert(std::holds_alternative<t::VerifiedDelegation>(
      verifier.crypto->verify(proof->bytes(), t::Service::Runtime, control, fixture.clock.value)));
  assert(std::holds_alternative<t::Failure>(verifier.crypto->sign(claims, {"key"})));
  auto runtime = control;
  runtime.binding.peer = t::PlatformPeer{t::Service::Runtime, {"runtime"}};
  runtime.binding.certificate.subject = {"runtime"};
  runtime.binding.certificate.platform_service = t::Service::Runtime;
  assert(std::holds_alternative<t::VerifiedDelegation>(
      bundle.crypto->verify_exchange_parent(proof->bytes(), runtime, fixture.clock.value)));
  auto modified = std::get<t::VerificationKeySet>(bundle.verification_keys->snapshot());
  modified.keys[0].ed25519_public_key[0] ^= std::byte{1};
  modified.version = {99};
  assert(std::get<t::VerificationKeySet>(bundle.verification_keys->snapshot()).version.value == 11);
  assert(std::get<t::VerificationKeySet>(bundle.verification_keys->snapshot())
             .keys[0]
             .ed25519_public_key == raw);
  fixture.clock.value.utc = t::TimePoint{std::chrono::seconds{1055}};
  assert(std::holds_alternative<t::VerificationKeySet>(bundle.verification_keys->snapshot()));
  fixture.clock.value.utc = t::TimePoint{std::chrono::seconds{1056}};
  control.verified_at = fixture.clock.value.utc;
  runtime.verified_at = fixture.clock.value.utc;
  assert(std::get<t::Failure>(bundle.verification_keys->snapshot()).code ==
         t::ErrorCode::UpstreamUnavailable);
  assert(std::holds_alternative<t::Failure>(bundle.crypto->sign(claims, {"key"})));
  assert(std::holds_alternative<t::Failure>(
      bundle.crypto->verify(proof->bytes(), t::Service::Runtime, control, fixture.clock.value)));
  assert(std::holds_alternative<t::Failure>(
      bundle.crypto->verify_exchange_parent(proof->bytes(), runtime, fixture.clock.value)));
  assert(std::holds_alternative<t::Failure>(
      crypto::make_delegation_crypto_bundle(fixture.configuration(), fixture.clock)));
  fixture.clock.value.utc = t::TimePoint{std::chrono::seconds{1000}};
  for (unsigned scenario = 0; scenario < 14; ++scenario) {
    auto invalid = fixture.configuration();
    switch (scenario) {
    case 0:
      invalid.version = {};
      break;
    case 1:
      invalid.keys[0].version = {};
      break;
    case 2:
      invalid.keys[0].valid_until = {};
      break;
    case 3:
      invalid.keys[0].valid_until = t::TimePoint::max();
      break;
    case 4:
      invalid.keyset_resource.kind = t::ResourceKind::Principal;
      break;
    case 5:
      invalid.keyset_resource.id.value = "all";
      break;
    case 6:
      invalid.keyset_resource.id.value = "keys";
      break;
    case 7:
      invalid.keys[0].public_key_pem += "trailing";
      break;
    case 8:
      invalid.keys.clear();
      break;
    case 9:
      invalid.keyset_resource.id.value[0] = 'z';
      break;
    case 10:
      invalid.keyset_resource.id.value = "00000000-0000-0000-0000-000000000000";
      break;
    case 11:
      invalid.keyset_resource.id.value = "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA";
      break;
    case 12:
      invalid.keyset_resource.id.value[14] = '3';
      break;
    case 13:
      invalid.keyset_resource.id.value[19] = '7';
      break;
    default:
      assert(false);
    }
    assert(std::holds_alternative<t::Failure>(
        crypto::make_delegation_crypto_bundle(std::move(invalid), fixture.clock)));
  }
  auto multiple = fixture.configuration();
  auto other = fixture.configuration(false);
  multiple.keys.push_back(std::move(other.keys.front()));
  assert(std::holds_alternative<t::Failure>(
      crypto::make_delegation_crypto_bundle(std::move(multiple), fixture.clock)));
  fixture.clock.value.trusted = false;
  assert(std::get<t::Failure>(bundle.verification_keys->snapshot()).code ==
         t::ErrorCode::UpstreamUnavailable);
  assert(std::holds_alternative<t::Failure>(
      crypto::make_delegation_crypto_bundle(fixture.configuration(), fixture.clock)));
  fixture.clock.value.trusted = true;
  fixture.clock.unavailable = true;
  assert(std::get<t::Failure>(bundle.verification_keys->snapshot()).code ==
         t::ErrorCode::UpstreamUnavailable);
} catch (...) {
  return 1;
}
