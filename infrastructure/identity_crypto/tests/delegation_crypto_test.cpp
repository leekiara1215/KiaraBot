// clang-format off
#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"

#include <cassert>
#include <memory>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
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
// Mature OpenSSL signed leaf objects provide distinct actual certificate fingerprints. Chain
// verification/TLS extraction are covered by PeerCertificatePort, not by this delegated proof test.
t::Sha256Digest leaf_fingerprint(EVP_PKEY* key, long serial) {
  std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
  assert(certificate && X509_set_version(certificate.get(), 2) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) == 1);
  assert(ASN1_TIME_set_string(X509_getm_notBefore(certificate.get()), "19700101000000Z") == 1);
  assert(ASN1_TIME_set_string(X509_getm_notAfter(certificate.get()), "19700102000000Z") == 1);
  assert(X509_set_pubkey(certificate.get(), key) == 1);
  auto* name = X509_get_subject_name(certificate.get());
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>("delegation-test-leaf"),
                                    -1, -1, 0) == 1);
  assert(X509_set_issuer_name(certificate.get(), name) == 1);
  assert(X509_sign(certificate.get(), key, nullptr) > 0);
  t::Sha256Digest fingerprint;
  unsigned size = 0;
  assert(X509_digest(certificate.get(), EVP_sha256(),
                     reinterpret_cast<unsigned char*>(fingerprint.bytes.data()), &size) == 1 &&
         size == 32);
  return fingerprint;
}
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
      1,
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
  const auto control_fingerprint = leaf_fingerprint(key.get(), 1);
  const auto runtime_fingerprint = leaf_fingerprint(key.get(), 2);
  assert(control_fingerprint != runtime_fingerprint);
  claims.client_certificate = control_fingerprint;
  peer.binding.certificate.fingerprint = control_fingerprint;
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
  auto recipient = peer;
  recipient.binding.peer = t::PlatformPeer{t::Service::Runtime, {"runtime"}};
  recipient.binding.certificate.subject = {"runtime"};
  recipient.binding.certificate.platform_service = t::Service::Runtime;
  recipient.binding.certificate.fingerprint = runtime_fingerprint;
  recipient.binding.certificate.registered_san = "spiffe://runtime";
  recipient.binding.credential = {{"runtime-credential"}, {1}};
  assert(std::holds_alternative<t::Failure>(
      port->verify(proof->bytes(), t::Service::Runtime, recipient, clock.value)));
  auto parent = port->verify_exchange_parent(proof->bytes(), recipient, clock.value);
  assert(std::holds_alternative<t::VerifiedDelegation>(parent));
  assert(std::get<t::VerifiedDelegation>(parent).claims.client_certificate == control_fingerprint);
  assert(std::get<t::VerifiedDelegation>(parent).claims.direct_actor == t::SubjectId{"control"});
  auto invalid_recipient = recipient;
  invalid_recipient.binding.peer = t::AdapterPeer{{"adapter"}, {"runtime"}};
  invalid_recipient.binding.certificate.platform_service.reset();
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), invalid_recipient, clock.value)));
  invalid_recipient = recipient;
  invalid_recipient.binding.peer = t::PlatformPeer{t::Service::Model, {"runtime"}};
  invalid_recipient.binding.certificate.platform_service = t::Service::Model;
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), invalid_recipient, clock.value)));
  invalid_recipient = recipient;
  invalid_recipient.binding.certificate.platform_service = t::Service::Control;
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), invalid_recipient, clock.value)));
  invalid_recipient = recipient;
  invalid_recipient.binding.credential.version.value = 0;
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), invalid_recipient, clock.value)));
  invalid_recipient = recipient;
  invalid_recipient.verified_at -= std::chrono::seconds{3};
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), invalid_recipient, clock.value)));
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
  auto second_hop = claims;
  second_hop.audience = t::Service::Model;
  second_hop.direct_actor = {"runtime"};
  second_hop.actor_chain.push_back({"runtime"});
  second_hop.business_hops = 2;
  auto second_signed = port->sign(second_hop, {"key-1"});
  auto second_peer = peer;
  second_peer.binding.peer = t::PlatformPeer{t::Service::Runtime, {"runtime"}};
  second_peer.binding.certificate.subject = {"runtime"};
  second_peer.binding.certificate.platform_service = t::Service::Runtime;
  assert(std::holds_alternative<t::VerifiedDelegation>(
      port->verify(std::get<t::OwnedSecret>(second_signed)->bytes(), t::Service::Model, second_peer,
                   clock.value)));
  auto third_hop = second_hop;
  third_hop.actor_chain.push_back({"model"});
  third_hop.direct_actor = {"model"};
  third_hop.business_hops = 3;
  assert(std::holds_alternative<t::Failure>(port->sign(third_hop, {"key-1"})));
  auto zero_hop = claims;
  zero_hop.business_hops = 0;
  assert(std::holds_alternative<t::Failure>(port->sign(zero_hop, {"key-1"})));
  auto mismatch_hop = claims;
  mismatch_hop.business_hops = 2;
  assert(std::holds_alternative<t::Failure>(port->sign(mismatch_hop, {"key-1"})));
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
  auto malformed_hops = delegation;
  malformed_hops["business_hops"] = nlohmann::json(std::string{"3"});
  auto malicious_payload = changed_payload;
  malicious_payload["delegation"] = nlohmann::json(malformed_hops);
  assert(std::holds_alternative<t::Failure>(resigned(malicious_payload, false, "key-1", false)));
  malformed_hops = delegation;
  malformed_hops["actor_chain"] = nlohmann::json(nlohmann::json::array_t{
      nlohmann::json("control"), nlohmann::json("runtime"), nlohmann::json("model")});
  malicious_payload["delegation"] = nlohmann::json(malformed_hops);
  assert(std::holds_alternative<t::Failure>(resigned(malicious_payload, false, "key-1", false)));
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
  auto exchange_signed = [&](nlohmann::json::object_t payload, const std::string& kid,
                             bool reuse_original_signature = false) {
    auto builder = jwt::create();
    builder.set_type("JWT").set_key_id(kid);
    for (const auto& [name, value] : payload) {
      builder.set_payload_claim(name, jwt::claim(value));
    }
    auto encoded = builder.sign(jwt::algorithm::ed25519(public_pem, private_pem));
    if (reuse_original_signature) {
      // Deliberately substitute the original proof's signature after mature-library encoding
      // of the changed certificate claim; this is an unsigned wire mutation, not a JWT signer.
      encoded.replace(encoded.rfind('.') + 1, std::string::npos,
                      original.substr(original.rfind('.') + 1));
    }
    return port->verify_exchange_parent(std::as_bytes(std::span{encoded.data(), encoded.size()}),
                                        recipient, clock.value);
  };
  auto exchange_payload = decoded.get_payload_json();
  assert(std::holds_alternative<t::Failure>(exchange_signed(exchange_payload, "unknown")));
  exchange_payload["aud"] = nlohmann::json("model");
  assert(std::holds_alternative<t::Failure>(exchange_signed(exchange_payload, "key-1")));
  exchange_payload = decoded.get_payload_json();
  auto exchange_claims = exchange_payload.at("delegation").get<nlohmann::json::object_t>();
  exchange_claims["business_hops"] = nlohmann::json("0");
  exchange_payload["delegation"] = nlohmann::json(exchange_claims);
  assert(std::holds_alternative<t::Failure>(exchange_signed(exchange_payload, "key-1")));
  exchange_claims["business_hops"] = nlohmann::json("2");
  exchange_claims["actor_chain"] =
      nlohmann::json(nlohmann::json::array_t{nlohmann::json("control"), nlohmann::json("runtime")});
  exchange_claims["direct_actor"] = nlohmann::json("runtime");
  exchange_payload["delegation"] = nlohmann::json(exchange_claims);
  assert(std::holds_alternative<t::Failure>(exchange_signed(exchange_payload, "key-1")));
  exchange_payload = decoded.get_payload_json();
  exchange_claims = exchange_payload.at("delegation").get<nlohmann::json::object_t>();
  const std::string other_fingerprint(64, 'a');
  exchange_claims["client_certificate"] = nlohmann::json(other_fingerprint);
  exchange_payload["delegation"] = nlohmann::json(exchange_claims);
  // A correctly re-signed previous-peer binding is preserved for Core's authoritative check;
  // this adapter cannot decide which Control registration/fingerprint is current.
  assert(std::holds_alternative<t::Failure>(exchange_signed(exchange_payload, "key-1", true)));
  auto different_previous = exchange_signed(exchange_payload, "key-1");
  assert(std::holds_alternative<t::VerifiedDelegation>(different_previous));
  const auto& previous_claims = std::get<t::VerifiedDelegation>(different_previous).claims;
  assert(previous_claims.direct_actor == t::SubjectId{"control"});
  for (auto byte : previous_claims.client_certificate.bytes) {
    assert(byte == std::byte{0xaa});
  }
  assert(std::holds_alternative<t::Failure>(port->verify_exchange_parent(
      std::as_bytes(std::span{tampered.data(), tampered.size()}), recipient, clock.value)));
  bad_clock = clock.value;
  bad_clock.trusted = false;
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), recipient, bad_clock)));
  bad_clock = clock.value;
  bad_clock.maximum_known_skew = std::chrono::milliseconds{2001};
  assert(std::holds_alternative<t::Failure>(
      port->verify_exchange_parent(proof->bytes(), recipient, bad_clock)));
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
