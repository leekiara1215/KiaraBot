// clang-format off
#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"

#include <cassert>
#include <memory>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/pem.h>
#include <sodium.h>
// clang-format on

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace t = kiarabot::core::target;
class Clock final : public t::SecurityClockPort {
public:
  t::SecurityClockSample value{
      t::TimePoint{std::chrono::seconds{1000}}, {}, std::chrono::milliseconds{2000}, true};
  t::Outcome<t::SecurityClockSample> sample() override {
    return value;
  }
};
int main() try {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> pub(BIO_new(BIO_s_mem()), BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> priv(BIO_new(BIO_s_mem()), BIO_free);
  assert(key && pub && priv);
  assert(PEM_write_bio_PUBKEY(pub.get(), key.get()) == 1);
  assert(PEM_write_bio_PrivateKey(priv.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ==
         1);
  char* public_pointer = nullptr;
  char* private_pointer = nullptr;
  const auto public_size = BIO_get_mem_data(pub.get(), &public_pointer);
  const auto private_size = BIO_get_mem_data(priv.get(), &private_pointer);
  std::string public_pem(public_pointer, static_cast<std::size_t>(public_size));
  auto private_copy = crypto::copy_secret(
      std::as_bytes(std::span{private_pointer, static_cast<std::size_t>(private_size)}));
  Clock clock;
  crypto::DelegationConfig config{"kiarabot-identity", {}};
  config.keys.push_back({{"key-1"}, public_pem, std::move(std::get<t::OwnedSecret>(private_copy))});
  auto factory = crypto::make_delegation_crypto(std::move(config), clock);
  auto& port = std::get<std::unique_ptr<t::DelegationCryptoPort>>(factory);
  t::DelegationClaims claims{
      "kiarabot-identity",
      t::Service::Runtime,
      {"user"},
      {"control"},
      {{"control"}},
      {{t::Permission::AccountRead, {t::ResourceKind::Account, {"account"}}}},
      {"proof-1"},
      {},
      {18446744073709551615ULL},
      clock.value.utc,
      clock.value.utc,
      clock.value.utc + std::chrono::seconds{56},
      0,
      {{"credential"}, {18446744073709551615ULL}},
      t::SessionId{"session"}};
  t::VerifiedPeer peer{{t::PlatformPeer{t::Service::Control, {"control"}},
                        {{"control"},
                         {},
                         "spiffe://control",
                         t::Service::Control,
                         clock.value.utc - std::chrono::seconds{1},
                         clock.value.utc + std::chrono::hours{1}},
                        {{"control-credential"}, {1}}},
                       clock.value.utc};
  auto signed_result = port->sign(claims, {"key-1"});
  auto& proof = std::get<t::OwnedSecret>(signed_result);
  auto verified = port->verify(proof->bytes(), t::Service::Runtime, peer, clock.value);
  assert(std::holds_alternative<t::VerifiedDelegation>(verified));
  assert(std::get<t::VerifiedDelegation>(verified).claims.source_credential ==
         claims.source_credential);
  assert(std::get<t::VerifiedDelegation>(verified).claims.source_session == claims.source_session);
  assert(std::get<t::VerifiedDelegation>(verified).claims.authorization_version.value ==
         18446744073709551615ULL);
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Model, peer, clock.value)));
  auto bad_clock = clock.value;
  bad_clock.trusted = false;
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, peer, bad_clock)));
  bad_clock = clock.value;
  bad_clock.maximum_known_skew = std::chrono::milliseconds{2001};
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, peer, bad_clock)));
  auto changed_peer = peer;
  changed_peer.binding.certificate.fingerprint.bytes[0] = std::byte{1};
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, changed_peer, clock.value)));
  changed_peer = peer;
  changed_peer.verified_at -= std::chrono::seconds{3};
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, changed_peer, clock.value)));
  auto invalid = claims;
  invalid.source_credential.version.value = 0;
  assert(std::holds_alternative<t::Failure>(port->sign(invalid, {"key-1"})));
  invalid = claims;
  invalid.expires_at += std::chrono::nanoseconds{1};
  assert(std::holds_alternative<t::Failure>(port->sign(invalid, {"key-1"})));
  invalid = claims;
  invalid.permissions.push_back(invalid.permissions.front());
  assert(std::holds_alternative<t::Failure>(port->sign(invalid, {"key-1"})));
  std::string tampered(reinterpret_cast<const char*>(proof->bytes().data()), proof->bytes().size());
  tampered[tampered.size() - 5] = tampered[tampered.size() - 5] == 'A' ? 'B' : 'A';
  assert(std::holds_alternative<t::Failure>(
      port->verify(std::as_bytes(std::span{tampered.data(), tampered.size()}), t::Service::Runtime,
                   peer, clock.value)));
  // Re-sign mutated claims with the real trusted key so rejection exercises claims, not merely
  // signature failure. Test private PEM is ephemeral and erased after use.
  const std::string original(reinterpret_cast<const char*>(proof->bytes().data()),
                             proof->bytes().size());
  const auto decoded = jwt::decode(original);
  std::string private_pem(private_pointer, static_cast<std::size_t>(private_size));
  auto resigned = [&](nlohmann::json::object_t payload, bool unsigned_token, const std::string& kid,
                      bool url) {
    auto builder = jwt::create();
    const jwt::claim type_claim(std::string{"JWT"});
    builder.set_header_claim("typ", type_claim).set_key_id(kid);
    for (const auto& [name, value] : payload) {
      builder.set_payload_claim(name, jwt::claim(value));
    }
    if (url) {
      builder.set_header_claim("jku", jwt::claim(std::string{"https://untrusted.invalid/key"}));
    }
    std::string encoded;
    if (unsigned_token) {
      encoded = builder.sign(jwt::algorithm::none{});
    } else {
      encoded = builder.sign(jwt::algorithm::ed25519(public_pem, private_pem));
    }
    return port->verify(std::as_bytes(std::span{encoded.data(), encoded.size()}),
                        t::Service::Runtime, peer, clock.value);
  };
  auto changed_payload = decoded.get_payload_json();
  changed_payload["iss"] = nlohmann::json(std::string{"other"});
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  changed_payload = decoded.get_payload_json();
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "unknown", false)));
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, true, "key-1", false)));
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", true)));
  auto delegation = changed_payload.at("delegation").get<nlohmann::json::object_t>();
  delegation["source_credential"] = nlohmann::json(nlohmann::json::array_t{
      nlohmann::json(std::string{"credential"}), nlohmann::json(std::string{"0"})});
  changed_payload["delegation"] = nlohmann::json(delegation);
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  delegation["source_credential"] = nlohmann::json(1.0);
  changed_payload["delegation"] = nlohmann::json(delegation);
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  delegation["source_credential"] =
      nlohmann::json(nlohmann::json::array_t{nlohmann::json(std::string{"credential"}),
                                             nlohmann::json(std::string{"18446744073709551616"})});
  changed_payload["delegation"] = nlohmann::json(delegation);
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  changed_payload = decoded.get_payload_json();
  changed_payload["iat"] = nlohmann::json(1000.5);
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  changed_payload = decoded.get_payload_json();
  changed_payload["exp"] = nlohmann::json(1e30);
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  changed_payload = decoded.get_payload_json();
  changed_payload["nbf"] = nlohmann::json(std::string{"1000"});
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  changed_payload = decoded.get_payload_json();
  changed_payload["exp"] = nlohmann::json(std::numeric_limits<std::uint64_t>::max());
  assert(std::holds_alternative<t::Failure>(resigned(changed_payload, false, "key-1", false)));
  bad_clock = clock.value;
  bad_clock.utc = t::TimePoint::max();
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, peer, bad_clock)));
  changed_peer = peer;
  changed_peer.verified_at = t::TimePoint::min();
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, changed_peer, clock.value)));
  sodium_memzero(private_pem.data(), private_pem.size());
  auto later = clock.value;
  later.utc += std::chrono::seconds{59};
  changed_peer = peer;
  changed_peer.verified_at = later.utc;
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, changed_peer, later)));
} catch (...) {
  return 1;
}
