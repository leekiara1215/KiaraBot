#include "kiarabot/core/runtime/platform/peer_certificate.hpp"

#include <algorithm>
#include <memory>
#include <new>
#include <string_view>

#include <grpc/grpc_security_constants.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace kiarabot::core::platform {
namespace {
constexpr std::size_t kMaximumChain = 65536;
constexpr std::size_t kMaximumCertificates = 8;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

target::Failure denied() {
  return {target::ErrorCode::Unauthenticated,
          target::Acceptance::NotAccepted,
          target::Recovery::Authenticate,
          {},
          {}};
}
std::string_view view(grpc::string_ref value) {
  return {value.data(), value.size()};
}
bool whitespace(char value) {
  return value == ' ' || value == '\r' || value == '\n' || value == '\t';
}
void trim(std::string_view& value) {
  while (!value.empty() && whitespace(value.front())) {
    value.remove_prefix(1);
  }
}
bool append_pem(std::string_view pem, std::vector<Certificate>& certificates, bool chain_property) {
  if (pem.size() > kMaximumChain || pem.find('\0') != std::string_view::npos) {
    return false;
  }
  trim(pem);
  while (!pem.empty()) {
    constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view kEnd = "-----END CERTIFICATE-----";
    if (!pem.starts_with(kBegin)) {
      return false;
    }
    const auto end = pem.find(kEnd);
    if (end == std::string_view::npos) {
      return false;
    }
    const auto length = end + kEnd.size();
    const Bio bio{BIO_new_mem_buf(pem.data(), static_cast<int>(length)), BIO_free};
    if (!bio) {
      return false;
    }
    Certificate certificate{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free};
    if (!certificate) {
      return false;
    }
    const auto duplicate =
        std::find_if(certificates.begin(), certificates.end(), [&](const Certificate& existing) {
          return X509_cmp(existing.get(), certificate.get()) == 0;
        });
    // OpenSSL server chain can exclude the leaf, while other implementations include it.
    // Permit only that one transport representation difference, not repeated intermediates.
    if (duplicate != certificates.end()) {
      if (!chain_property || duplicate != certificates.begin()) {
        return false;
      }
      chain_property = false;
    } else {
      if (certificates.size() == kMaximumCertificates) {
        return false;
      }
      certificates.push_back(std::move(certificate));
    }
    pem.remove_prefix(length);
    trim(pem);
  }
  return true;
}
struct ErrorQueue {
  ~ErrorQueue() {
    ERR_clear_error();
  }
};
} // namespace

target::Outcome<PeerCertificateEvidence>
extract_peer_certificate(const grpc::AuthContext& context) {
  const ErrorQueue error_queue;
  try {
    if (!context.IsPeerAuthenticated()) {
      return denied();
    }
    const auto transport = context.FindPropertyValues(GRPC_TRANSPORT_SECURITY_TYPE_PROPERTY_NAME);
    const auto leaf = context.FindPropertyValues(GRPC_X509_PEM_CERT_PROPERTY_NAME);
    const auto chain = context.FindPropertyValues(GRPC_X509_PEM_CERT_CHAIN_PROPERTY_NAME);
    if (transport.size() != 1 || leaf.size() != 1 || chain.size() > 1 ||
        (view(transport.front()) != GRPC_SSL_TRANSPORT_SECURITY_TYPE &&
         view(transport.front()) != GRPC_TLS_TRANSPORT_SECURITY_TYPE)) {
      return denied();
    }
    if (leaf.front().size() > kMaximumChain ||
        (!chain.empty() && chain.front().size() > kMaximumChain - leaf.front().size())) {
      return denied();
    }
    std::vector<Certificate> certificates;
    if (!append_pem(view(leaf.front()), certificates, false) || certificates.size() != 1 ||
        (!chain.empty() && !append_pem(view(chain.front()), certificates, true))) {
      return denied();
    }
    PeerCertificateEvidence result;
    unsigned int digest_size = 0;
    if (X509_digest(certificates.front().get(), EVP_sha256(),
                    reinterpret_cast<unsigned char*>(result.leaf_fingerprint.bytes.data()),
                    &digest_size) != 1 ||
        digest_size != result.leaf_fingerprint.bytes.size()) {
      return denied();
    }
    for (const auto& certificate : certificates) {
      const auto length = i2d_X509(certificate.get(), nullptr);
      if (length <= 0 ||
          static_cast<std::size_t>(length) > kMaximumChain - result.der_chain.size()) {
        return denied();
      }
      const auto offset = result.der_chain.size();
      result.der_chain.resize(offset + static_cast<std::size_t>(length));
      auto* output = reinterpret_cast<unsigned char*>(result.der_chain.data() + offset);
      if (i2d_X509(certificate.get(), &output) != length) {
        return denied();
      }
    }
    return result;
  } catch (const std::bad_alloc&) {
    return target::Failure{target::ErrorCode::UpstreamUnavailable,
                           target::Acceptance::NotAccepted,
                           target::Recovery::Diagnose,
                           {},
                           {}};
  }
}

} // namespace kiarabot::core::platform
