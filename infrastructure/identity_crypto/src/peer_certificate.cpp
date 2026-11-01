// clang-format off
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"

#include <algorithm>
#include <ctime>
#include <limits>
#include <memory>
#include <string_view>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/core_names.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
// clang-format on

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Store = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>;
using Context = std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)>;
using Names = std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)>;
struct StackFree {
  void operator()(STACK_OF(X509) * stack) const {
    sk_X509_free(stack);
  }
};
using Stack = std::unique_ptr<STACK_OF(X509), StackFree>;
t::Failure failure(t::ErrorCode code = t::ErrorCode::Unauthenticated) {
  ERR_clear_error();
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
bool service(t::Service value) {
  switch (value) {
  case t::Service::Identity:
  case t::Service::Runtime:
  case t::Service::Host:
  case t::Service::Scheduler:
  case t::Service::Model:
  case t::Service::Control:
    return true;
  }
  return false;
}
bool bounded(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string_view::npos;
}
bool strong(X509* certificate) {
  int digest = NID_undef;
  int key_algorithm = NID_undef;
  if (OBJ_find_sigid_algs(X509_get_signature_nid(certificate), &digest, &key_algorithm) != 1 ||
      (digest != NID_undef && digest != NID_sha256 && digest != NID_sha384 &&
       digest != NID_sha512)) {
    return false;
  }
  EVP_PKEY* key = X509_get0_pubkey(certificate);
  if (!key) {
    return false;
  }
  if (EVP_PKEY_is_a(key, "ED25519") == 1) {
    return true;
  }
  if (EVP_PKEY_is_a(key, "RSA") == 1) {
    return EVP_PKEY_get_bits(key) >= 2048;
  }
  if (EVP_PKEY_is_a(key, "EC") == 1) {
    char group[80]{};
    std::size_t size = 0;
    return EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group),
                                          &size) == 1 &&
           std::string_view{group, size} == "prime256v1";
  }
  return false;
}
bool shape(const t::RegisteredPeer& expected) {
  if (!bounded(expected.certificate.subject.value) || !bounded(expected.credential.id.value) ||
      !expected.credential.version.value || !bounded(expected.certificate.registered_san)) {
    return false;
  }
  return std::visit(
      [&](const auto& peer) {
        if (peer.subject != expected.certificate.subject) {
          return false;
        }
        if constexpr (requires { peer.service; }) {
          return service(peer.service) && expected.certificate.platform_service == peer.service;
        } else if constexpr (requires { peer.adapter; }) {
          return !expected.certificate.platform_service && bounded(peer.adapter.value);
        } else {
          return !expected.certificate.platform_service && bounded(peer.instance.value);
        }
      },
      expected.peer);
}
class PeerCertificates final : public t::PeerCertificatePort {
public:
  PeerCertificates(Store store, PeerSanKind kind) : store_(std::move(store)), kind_(kind) {}
  t::Outcome<t::VerifiedPeer> verify(std::span<const std::byte> encoded,
                                     const t::RegisteredPeer& expected, t::TimePoint now) override {
    try {
      if (encoded.empty() || encoded.size() > 65536 || !shape(expected) ||
          expected.certificate.not_before > now || expected.certificate.not_after <= now ||
          expected.certificate.not_after <= expected.certificate.not_before) {
        return failure();
      }
      const auto* cursor = reinterpret_cast<const unsigned char*>(encoded.data());
      const auto* end = cursor + encoded.size();
      std::vector<Certificate> certificates;
      while (cursor != end) {
        if (certificates.size() == 8) {
          return failure();
        }
        const auto* begin = cursor;
        Certificate certificate(d2i_X509(nullptr, &cursor, static_cast<long>(end - cursor)),
                                X509_free);
        if (!certificate || !strong(certificate.get()) || cursor <= begin) {
          return failure();
        }
        // d2i accepts some BER encodings; require exact canonical DER for stable evidence.
        const int size = i2d_X509(certificate.get(), nullptr);
        if (size <= 0 ||
            static_cast<std::size_t>(size) != static_cast<std::size_t>(cursor - begin)) {
          return failure();
        }
        std::vector<unsigned char> canonical(static_cast<std::size_t>(size));
        auto* output = canonical.data();
        if (i2d_X509(certificate.get(), &output) != size ||
            !std::equal(canonical.begin(), canonical.end(), begin)) {
          return failure();
        }
        for (const auto& previous : certificates) {
          if (X509_cmp(previous.get(), certificate.get()) == 0) {
            return failure();
          }
        }
        certificates.push_back(std::move(certificate));
      }
      t::Sha256Digest fingerprint;
      unsigned int fingerprint_size = 0;
      if (X509_digest(certificates.front().get(), EVP_sha256(),
                      reinterpret_cast<unsigned char*>(fingerprint.bytes.data()),
                      &fingerprint_size) != 1 ||
          fingerprint_size != fingerprint.bytes.size() ||
          fingerprint != expected.certificate.fingerprint) {
        return failure();
      }
      Names names(static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(
                      certificates.front().get(), NID_subject_alt_name, nullptr, nullptr)),
                  GENERAL_NAMES_free);
      if (!names) {
        return failure();
      }
      // An explicit clientAuth EKU is required; absent EKU is not unrestricted client use.
      std::unique_ptr<EXTENDED_KEY_USAGE, decltype(&EXTENDED_KEY_USAGE_free)> eku(
          static_cast<EXTENDED_KEY_USAGE*>(
              X509_get_ext_d2i(certificates.front().get(), NID_ext_key_usage, nullptr, nullptr)),
          EXTENDED_KEY_USAGE_free);
      bool client_auth = false;
      if (!eku) {
        return failure();
      }
      for (int index = 0; index < sk_ASN1_OBJECT_num(eku.get()); ++index) {
        if (OBJ_obj2nid(sk_ASN1_OBJECT_value(eku.get(), index)) == NID_client_auth) {
          client_auth = true;
        }
      }
      if (!client_auth) {
        return failure();
      }
      const int desired = kind_ == PeerSanKind::Uri ? GEN_URI : GEN_DNS;
      unsigned matches = 0;
      for (int index = 0; index < sk_GENERAL_NAME_num(names.get()); ++index) {
        const auto* name = sk_GENERAL_NAME_value(names.get(), index);
        if (name->type != desired) {
          continue;
        }
        const auto* text = name->d.ia5;
        const int length = ASN1_STRING_length(text);
        if (length <= 0) {
          return failure();
        }
        std::string_view value(reinterpret_cast<const char*>(ASN1_STRING_get0_data(text)),
                               static_cast<std::size_t>(length));
        if (value.find('\0') != std::string_view::npos) {
          return failure();
        }
        if (value == expected.certificate.registered_san) {
          ++matches;
        }
      }
      if (matches != 1) {
        return failure();
      }
      Stack chain(sk_X509_new_null());
      Context context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
      if (!chain || !context) {
        return failure(t::ErrorCode::UpstreamUnavailable);
      }
      for (std::size_t index = 1; index < certificates.size(); ++index) {
        if (sk_X509_push(chain.get(), certificates[index].get()) == 0) {
          return failure(t::ErrorCode::UpstreamUnavailable);
        }
      }
      if (X509_STORE_CTX_init(context.get(), store_.get(), certificates.front().get(),
                              chain.get()) != 1) {
        return failure(t::ErrorCode::UpstreamUnavailable);
      }
      auto* parameters = X509_STORE_CTX_get0_param(context.get());
      const auto seconds = std::chrono::floor<std::chrono::seconds>(now.time_since_epoch()).count();
      if (!std::in_range<std::time_t>(seconds)) {
        return failure();
      }
      X509_VERIFY_PARAM_set_time(parameters, static_cast<std::time_t>(seconds));
      X509_VERIFY_PARAM_set_depth(parameters, 7);
      if (X509_VERIFY_PARAM_set_purpose(parameters, X509_PURPOSE_SSL_CLIENT) != 1 ||
          X509_VERIFY_PARAM_set_flags(parameters, X509_V_FLAG_X509_STRICT |
                                                      X509_V_FLAG_CHECK_SS_SIGNATURE) != 1 ||
          X509_verify_cert(context.get()) != 1) {
        return failure();
      }
      ERR_clear_error();
      return t::VerifiedPeer{expected, now};
    } catch (const std::bad_alloc&) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
  }

private:
  const Store store_;
  const PeerSanKind kind_;
};
} // namespace
core::target::Outcome<std::unique_ptr<core::target::PeerCertificatePort>>
make_peer_certificate_crypto(PeerTrustConfig config) {
  try {
    if ((config.san_kind != PeerSanKind::Uri && config.san_kind != PeerSanKind::Dns) ||
        config.trust_roots_pem.empty() || config.trust_roots_pem.size() > 16) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    Store store(X509_STORE_new(), X509_STORE_free);
    if (!store) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& root : config.trust_roots_pem) {
      if (!root.starts_with("-----BEGIN CERTIFICATE-----\n") ||
          root.find('\0') != std::string::npos || root.size() > 65536) {
        return failure(t::ErrorCode::InvalidArgument);
      }
      std::unique_ptr<BIO, decltype(&BIO_free)> input(
          BIO_new_mem_buf(root.data(), static_cast<int>(root.size())), BIO_free);
      if (!input) {
        return failure(t::ErrorCode::UpstreamUnavailable);
      }
      Certificate certificate(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
      char remainder[256];
      int count = 0;
      bool trailing = false;
      while ((count = BIO_read(input.get(), remainder, sizeof(remainder))) > 0) {
        for (int index = 0; index < count; ++index) {
          if (remainder[index] != ' ' && remainder[index] != '\n' && remainder[index] != '\r' &&
              remainder[index] != '\t') {
            trailing = true;
          }
        }
      }
      if (!certificate || trailing || !strong(certificate.get()) ||
          X509_check_ca(certificate.get()) <= 0 ||
          X509_STORE_add_cert(store.get(), certificate.get()) != 1) {
        return failure(t::ErrorCode::InvalidArgument);
      }
    }
    return std::unique_ptr<t::PeerCertificatePort>{
        std::make_unique<PeerCertificates>(std::move(store), config.san_kind)};
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
