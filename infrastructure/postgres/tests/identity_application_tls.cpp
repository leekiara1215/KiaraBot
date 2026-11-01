#include "identity_application_tls.hpp"

#include <chrono>
#include <ctime>
#include <memory>
#include <stdexcept>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace kiarabot::infrastructure::postgres::test {
namespace {
namespace target = core::target;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Endpoint = std::unique_ptr<SSL, decltype(&SSL_free)>;
void ensure(bool value) {
  if (!value) {
    throw std::runtime_error("owned fixture TLS construction failed");
  }
}
Key key() {
  Key result(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  ensure(result != nullptr);
  return result;
}
void extension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context{};
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> item(
      X509V3_EXT_conf_nid(nullptr, &context, nid, value), X509_EXTENSION_free);
  ensure(item && X509_add_ext(certificate, item.get(), -1) == 1);
}
Certificate certificate(EVP_PKEY* public_key, X509* issuer, EVP_PKEY* signer, long serial,
                        bool root, bool client,
                        const char* client_san = "URI:spiffe://kiarabot/control") {
  Certificate result(X509_new(), X509_free);
  ensure(result && X509_set_version(result.get(), 2) == 1 &&
         ASN1_INTEGER_set(X509_get_serialNumber(result.get()), serial) == 1 &&
         X509_gmtime_adj(X509_getm_notBefore(result.get()), -60) != nullptr &&
         X509_gmtime_adj(X509_getm_notAfter(result.get()), 3600) != nullptr &&
         X509_set_pubkey(result.get(), public_key) == 1);
  auto* name = X509_get_subject_name(result.get());
  const char* common_name = root ? "Owned integration CA" : client ? "Control" : "localhost";
  ensure(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>(common_name), -1, -1,
                                    0) == 1);
  ensure(X509_set_issuer_name(result.get(), issuer ? X509_get_subject_name(issuer) : name) == 1);
  auto* authority = issuer ? issuer : result.get();
  extension(result.get(), authority, NID_basic_constraints,
            root ? "critical,CA:TRUE,pathlen:1" : "critical,CA:FALSE");
  extension(result.get(), authority, NID_key_usage,
            root ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
  extension(result.get(), authority, NID_subject_key_identifier, "hash");
  extension(result.get(), authority, NID_authority_key_identifier, "keyid:always");
  if (!root) {
    extension(result.get(), authority, NID_ext_key_usage, client ? "clientAuth" : "serverAuth");
    extension(result.get(), authority, NID_subject_alt_name, client ? client_san : "DNS:localhost");
  }
  ensure(X509_sign(result.get(), signer, nullptr) > 0);
  return result;
}
Context context(const SSL_METHOD* method, X509* leaf, EVP_PKEY* private_key, X509* root) {
  Context result(SSL_CTX_new(method), SSL_CTX_free);
  ensure(result && SSL_CTX_set_min_proto_version(result.get(), TLS1_3_VERSION) == 1 &&
         SSL_CTX_use_certificate(result.get(), leaf) == 1 &&
         SSL_CTX_use_PrivateKey(result.get(), private_key) == 1 &&
         SSL_CTX_check_private_key(result.get()) == 1 &&
         X509_STORE_add_cert(SSL_CTX_get_cert_store(result.get()), root) == 1);
  SSL_CTX_set_verify(result.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  SSL_CTX_set_verify_depth(result.get(), 7);
  return result;
}
target::TimePoint timestamp(const ASN1_TIME* time) {
  std::tm utc{};
  ensure(ASN1_TIME_to_tm(time, &utc) == 1);
  const auto seconds = ::timegm(&utc);
  ensure(seconds > 0);
  return target::TimePoint{std::chrono::seconds{seconds}};
}
} // namespace
ControlTlsEvidence platform_tls_evidence(const target::SubjectId& subject,
                                         target::Service service) {
  const char* name = service == target::Service::Control   ? "control"
                     : service == target::Service::Runtime ? "runtime"
                     : service == target::Service::Model   ? "model"
                                                           : nullptr;
  ensure(name != nullptr);
  const std::string registered_san = std::string{"spiffe://kiarabot/"} + name;
  const auto extension_san = "URI:" + registered_san;
  auto root_key = key();
  auto root = certificate(root_key.get(), nullptr, root_key.get(), 1, true, false);
  auto control_key = key();
  auto control = certificate(control_key.get(), root.get(), root_key.get(), 2, false, true,
                             extension_san.c_str());
  auto server_key = key();
  auto server = certificate(server_key.get(), root.get(), root_key.get(), 3, false, false);
  auto client_context = context(TLS_client_method(), control.get(), control_key.get(), root.get());
  auto server_context = context(TLS_server_method(), server.get(), server_key.get(), root.get());
  Endpoint client(SSL_new(client_context.get()), SSL_free);
  Endpoint accepted(SSL_new(server_context.get()), SSL_free);
  ensure(client && accepted && SSL_set1_host(client.get(), "localhost") == 1);
  BIO* client_bio = nullptr;
  BIO* server_bio = nullptr;
  const bool paired = BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) == 1;
  std::unique_ptr<BIO, decltype(&BIO_free)> client_owner(client_bio, BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> server_owner(server_bio, BIO_free);
  ensure(paired);
  SSL_set_bio(client.get(), client_owner.get(), client_owner.get());
  client_owner.release();
  SSL_set_bio(accepted.get(), server_owner.get(), server_owner.get());
  server_owner.release();
  SSL_set_connect_state(client.get());
  SSL_set_accept_state(accepted.get());
  for (int attempt = 0; attempt < 1000 && (!SSL_is_init_finished(client.get()) ||
                                           !SSL_is_init_finished(accepted.get()));
       ++attempt) {
    for (auto* endpoint : {client.get(), accepted.get()}) {
      if (!SSL_is_init_finished(endpoint)) {
        const int result = SSL_do_handshake(endpoint);
        const int error = SSL_get_error(endpoint, result);
        ensure(result == 1 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
      }
    }
  }
  ensure(SSL_is_init_finished(client.get()) && SSL_is_init_finished(accepted.get()) &&
         SSL_get_verify_result(client.get()) == X509_V_OK &&
         SSL_get_verify_result(accepted.get()) == X509_V_OK);
  Certificate actual(SSL_get1_peer_certificate(accepted.get()), X509_free);
  ensure(actual && X509_cmp(actual.get(), control.get()) == 0);
  const int size = i2d_X509(actual.get(), nullptr);
  ensure(size > 0 && size <= 65536);
  std::vector<std::byte> evidence(static_cast<std::size_t>(size));
  auto* output = reinterpret_cast<unsigned char*>(evidence.data());
  ensure(i2d_X509(actual.get(), &output) == size);
  target::Sha256Digest fingerprint;
  unsigned int fingerprint_size = 0;
  ensure(X509_digest(actual.get(), EVP_sha256(),
                     reinterpret_cast<unsigned char*>(fingerprint.bytes.data()),
                     &fingerprint_size) == 1 &&
         fingerprint_size == fingerprint.bytes.size());
  std::unique_ptr<BIO, decltype(&BIO_free)> pem(BIO_new(BIO_s_mem()), BIO_free);
  ensure(pem && PEM_write_bio_X509(pem.get(), root.get()) == 1);
  char* public_data = nullptr;
  const auto public_size = BIO_get_mem_data(pem.get(), &public_data);
  ensure(public_size > 0);
  return {std::string(public_data, static_cast<std::size_t>(public_size)),
          std::move(evidence),
          {subject, fingerprint, registered_san, service,
           timestamp(X509_get0_notBefore(actual.get())),
           timestamp(X509_get0_notAfter(actual.get()))}};
}
ControlTlsEvidence control_tls_evidence(const target::SubjectId& subject) {
  return platform_tls_evidence(subject, target::Service::Control);
}

} // namespace kiarabot::infrastructure::postgres::test
