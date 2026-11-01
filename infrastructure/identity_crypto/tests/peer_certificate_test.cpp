// clang-format off
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"

#include <cassert>
#include <memory>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
// clang-format on

namespace crypto = kiarabot::infrastructure::identity_crypto;
namespace t = kiarabot::core::target;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
void extension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context{};
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> ext(
      X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
  assert(ext && X509_add_ext(certificate, ext.get(), -1) == 1);
}
Cert certificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* signer, bool ca, bool client_auth = true) {
  Cert cert(X509_new(), X509_free);
  assert(cert && X509_set_version(cert.get(), 2) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ca ? 1 : 2) == 1);
  assert(ASN1_TIME_set_string(X509_getm_notBefore(cert.get()), "20260101000000Z") == 1);
  assert(ASN1_TIME_set_string(X509_getm_notAfter(cert.get()), "20300101000000Z") == 1);
  assert(X509_set_pubkey(cert.get(), key) == 1);
  auto* name = X509_get_subject_name(cert.get());
  assert(X509_NAME_add_entry_by_txt(
             name, "CN", MBSTRING_ASC,
             reinterpret_cast<const unsigned char*>(ca ? "test CA" : "test peer"), -1, -1, 0) == 1);
  assert(X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : name) == 1);
  extension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
            ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  extension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
            ca ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
  extension(cert.get(), issuer ? issuer : cert.get(), NID_subject_key_identifier, "hash");
  if (!ca) {
    extension(cert.get(), issuer, NID_authority_key_identifier, "keyid:always");
    extension(cert.get(), issuer, NID_ext_key_usage, client_auth ? "clientAuth" : "serverAuth");
    extension(cert.get(), issuer, NID_subject_alt_name, "URI:spiffe://kiarabot/control");
  }
  assert(X509_sign(cert.get(), signer, nullptr) > 0);
  return cert;
}
std::vector<std::byte> der(X509* certificate) {
  const int size = i2d_X509(certificate, nullptr);
  assert(size > 0);
  std::vector<std::byte> result(static_cast<std::size_t>(size));
  auto* pointer = reinterpret_cast<unsigned char*>(result.data());
  assert(i2d_X509(certificate, &pointer) == size);
  return result;
}
std::string pem(X509* certificate) {
  std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
  assert(output && PEM_write_bio_X509(output.get(), certificate) == 1);
  char* value = nullptr;
  const long size = BIO_get_mem_data(output.get(), &value);
  assert(size > 0);
  return {value, static_cast<std::size_t>(size)};
}
int main() try {
  Key ca_key(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  Key leaf_key(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  assert(ca_key && leaf_key);
  auto ca = certificate(ca_key.get(), nullptr, ca_key.get(), true);
  auto leaf = certificate(leaf_key.get(), ca.get(), ca_key.get(), false);
  auto leaf_der = der(leaf.get());
  auto root_pem = pem(ca.get());
  auto factory = crypto::make_peer_certificate_crypto({{root_pem}, crypto::PeerSanKind::Uri});
  auto& port = std::get<std::unique_ptr<t::PeerCertificatePort>>(factory);
  const t::TimePoint now{std::chrono::seconds{1793491200}};
  t::Sha256Digest fingerprint;
  unsigned int length = 0;
  assert(X509_digest(leaf.get(), EVP_sha256(),
                     reinterpret_cast<unsigned char*>(fingerprint.bytes.data()), &length) == 1);
  t::RegisteredPeer expected{t::PlatformPeer{t::Service::Control, {"control"}},
                             {{"control"},
                              fingerprint,
                              "spiffe://kiarabot/control",
                              t::Service::Control,
                              now - std::chrono::hours{1},
                              now + std::chrono::hours{1}},
                             {{"credential"}, {1}}};
  assert(std::holds_alternative<t::VerifiedPeer>(port->verify(leaf_der, expected, now)));
  auto changed = expected;
  changed.certificate.registered_san += "x";
  assert(std::holds_alternative<t::Failure>(port->verify(leaf_der, changed, now)));
  changed = expected;
  changed.certificate.fingerprint.bytes[0] ^= std::byte{1};
  assert(std::holds_alternative<t::Failure>(port->verify(leaf_der, changed, now)));
  changed = expected;
  changed.certificate.platform_service = t::Service::Runtime;
  assert(std::holds_alternative<t::Failure>(port->verify(leaf_der, changed, now)));
  assert(std::holds_alternative<t::Failure>(
      port->verify(leaf_der, expected, now + std::chrono::hours{2})));
  changed = expected;
  changed.credential.version.value = 0;
  assert(std::holds_alternative<t::Failure>(port->verify(leaf_der, changed, now)));
  changed = expected;
  changed.certificate.registered_san.push_back('\0');
  assert(std::holds_alternative<t::Failure>(port->verify(leaf_der, changed, now)));
  changed = expected;
  changed.certificate.not_after = now + std::chrono::hours{24 * 365 * 10};
  assert(std::holds_alternative<t::Failure>(
      port->verify(leaf_der, changed, now + std::chrono::hours{24 * 365 * 5})));
  assert(std::holds_alternative<t::Failure>(
      crypto::make_peer_certificate_crypto({{root_pem + "garbage"}, crypto::PeerSanKind::Uri})));
  auto server_leaf = certificate(leaf_key.get(), ca.get(), ca_key.get(), false, false);
  auto server_expected = expected;
  assert(X509_digest(
             server_leaf.get(), EVP_sha256(),
             reinterpret_cast<unsigned char*>(server_expected.certificate.fingerprint.bytes.data()),
             &length) == 1);
  assert(std::holds_alternative<t::Failure>(
      port->verify(der(server_leaf.get()), server_expected, now)));
  changed = expected;
  // Deliberately corrupt an internal enum to verify fail-closed boundary handling.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  std::get<t::PlatformPeer>(changed.peer).service = static_cast<t::Service>(99);
  assert(std::holds_alternative<t::Failure>(port->verify(leaf_der, changed, now)));
  assert(std::holds_alternative<t::Failure>(port->verify({}, expected, now)));
  std::vector<std::byte> oversized(65537);
  assert(std::holds_alternative<t::Failure>(port->verify(oversized, expected, now)));
  auto trailing = leaf_der;
  trailing.push_back(std::byte{0});
  assert(std::holds_alternative<t::Failure>(port->verify(trailing, expected, now)));
  auto duplicate = leaf_der;
  duplicate.insert(duplicate.end(), leaf_der.begin(), leaf_der.end());
  assert(std::holds_alternative<t::Failure>(port->verify(duplicate, expected, now)));
  Key other_key(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  auto other_ca = certificate(other_key.get(), nullptr, other_key.get(), true);
  auto other_factory =
      crypto::make_peer_certificate_crypto({{pem(other_ca.get())}, crypto::PeerSanKind::Uri});
  auto& other = std::get<std::unique_ptr<t::PeerCertificatePort>>(other_factory);
  assert(std::holds_alternative<t::Failure>(other->verify(leaf_der, expected, now)));
} catch (...) {
  return 1;
}
