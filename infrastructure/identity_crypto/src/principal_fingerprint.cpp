#include "kiarabot/infrastructure/identity_crypto/principal_fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include <openssl/err.h>
#include <openssl/evp.h>

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
constexpr std::size_t kMaximumAudiences = 16;
t::Failure invalid() {
  return t::rejected(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
t::Failure unavailable() {
  ERR_clear_error();
  return t::rejected(t::ErrorCode::UpstreamUnavailable, t::Recovery::Diagnose);
}
bool text(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string_view::npos;
}
bool audiences_valid(const std::vector<t::Service>& audiences) {
  if (audiences.empty() || audiences.size() > kMaximumAudiences) {
    return false;
  }
  std::vector<t::Service> sorted = audiences;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return false;
  }
  return std::all_of(sorted.begin(), sorted.end(), [](const auto service) {
    return t::valid_service(service) && service != t::Service::Control;
  });
}
struct DigestFailure {};
class Digest {
public:
  explicit Digest(std::string_view domain) : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
      throw DigestFailure{};
    }
    string(domain);
  }
  void integer(std::uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[bytes.size() - index - 1] = static_cast<unsigned char>(value & 0xffU);
      value >>= 8;
    }
    update(bytes.data(), bytes.size());
  }
  void string(std::string_view value) {
    integer(static_cast<std::uint64_t>(value.size()));
    if (!value.empty()) {
      update(value.data(), value.size());
    }
  }
  void services(std::vector<t::Service> services) {
    std::sort(services.begin(), services.end());
    integer(static_cast<std::uint64_t>(services.size()));
    for (const auto service : services) {
      integer(static_cast<std::uint64_t>(service));
    }
  }
  t::Sha256Digest finish() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_.get(), bytes.data(), &size) != 1 || size != 32) {
      throw DigestFailure{};
    }
    t::Sha256Digest result{};
    std::transform(bytes.begin(), bytes.begin() + size, result.bytes.begin(),
                   [](unsigned char value) { return static_cast<std::byte>(value); });
    return result;
  }

private:
  void update(const void* bytes, std::size_t size) {
    if (EVP_DigestUpdate(context_.get(), bytes, size) != 1) {
      throw DigestFailure{};
    }
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};
class Fingerprints final : public t::IdentityPrincipalFingerprintPort {
public:
  t::Outcome<t::Sha256Digest>
  register_principal(t::PrincipalKind kind, const t::ResourceRef& binding,
                     const std::vector<t::Service>& allowed_services) override {
    try {
      if (!t::known_principal_kind(kind) || kind == t::PrincipalKind::Human ||
          !t::valid_resource_ref(binding) || !text(binding.id.value) ||
          !audiences_valid(allowed_services)) {
        return invalid();
      }
      Digest digest{t::kRegisterPrincipalFingerprintDomain};
      digest.integer(static_cast<std::uint64_t>(kind));
      digest.integer(static_cast<std::uint64_t>(binding.kind));
      digest.string(binding.id.value);
      digest.services(allowed_services);
      return digest.finish();
    } catch (...) {
      return unavailable();
    }
  }
  t::Outcome<t::Sha256Digest> revoke_credential(const t::CredentialId& target,
                                                const t::ResourceVersion& expected) override {
    try {
      if (!text(target.value) || expected.value == 0) {
        return invalid();
      }
      Digest digest{t::kRevokeCredentialFingerprintDomain};
      digest.string(target.value);
      digest.integer(expected.value);
      return digest.finish();
    } catch (...) {
      return unavailable();
    }
  }
};
} // namespace

core::target::Outcome<std::unique_ptr<core::target::IdentityPrincipalFingerprintPort>>
make_principal_fingerprints() {
  try {
    Digest probe{core::target::kRegisterPrincipalFingerprintDomain};
    static_cast<void>(probe.finish());
    return std::unique_ptr<core::target::IdentityPrincipalFingerprintPort>{
        std::make_unique<Fingerprints>()};
  } catch (...) {
    return unavailable();
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
