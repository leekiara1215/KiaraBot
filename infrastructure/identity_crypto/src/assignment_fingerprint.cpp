#include "kiarabot/infrastructure/identity_crypto/assignment_fingerprint.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <utility>

#include <openssl/err.h>
#include <openssl/evp.h>

#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace kiarabot::infrastructure::identity_crypto {
namespace {
namespace t = core::target;
constexpr std::size_t kMaximumResources = 128;
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
bool resources_valid(const std::vector<t::ResourceRef>& resources) {
  if (resources.empty() || resources.size() > kMaximumResources ||
      !t::unique_policy_resources(resources)) {
    return false;
  }
  return std::all_of(resources.begin(), resources.end(),
                     [](const auto& resource) { return text(resource.id.value); });
}
bool spec_valid(const t::ExactAssignmentSpec& spec) {
  if (!text(spec.subject.value)) {
    return false;
  }
  if (const auto* role = std::get_if<t::RoleAssignmentSpec>(&spec.basis)) {
    return t::known_role(role->role) && resources_valid(role->scope);
  }
  const auto& explicit_spec = std::get<t::ExplicitAssignmentSpec>(spec.basis);
  if (!resources_valid(explicit_spec.scope) || explicit_spec.permissions.empty() ||
      explicit_spec.permissions.size() > t::kPermissionNames.size()) {
    return false;
  }
  auto permissions = explicit_spec.permissions;
  std::sort(permissions.begin(), permissions.end());
  return std::all_of(permissions.begin(), permissions.end(), t::known_permission) &&
         std::adjacent_find(permissions.begin(), permissions.end()) == permissions.end();
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
  void resources(std::vector<t::ResourceRef> resources) {
    std::sort(resources.begin(), resources.end(), t::PolicyResourceLess{});
    integer(static_cast<std::uint64_t>(resources.size()));
    for (const auto& resource : resources) {
      integer(static_cast<std::uint64_t>(resource.kind));
      string(resource.id.value);
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
class Fingerprints final : public t::IdentityAssignmentFingerprintPort {
public:
  t::Outcome<t::Sha256Digest> set_assignment(const t::ExactAssignmentSpec& spec,
                                             std::optional<t::AssignmentId> target,
                                             std::optional<t::ResourceVersion> expected) override {
    try {
      if (!spec_valid(spec) || target.has_value() != expected.has_value() ||
          (target && !text(target->value)) || (expected && expected->value == 0)) {
        return invalid();
      }
      Digest digest{t::kSetAssignmentFingerprintDomain};
      digest.integer(target.has_value() ? 1 : 0);
      if (target) {
        digest.string(target->value);
      }
      digest.string(spec.subject.value);
      digest.integer(static_cast<std::uint64_t>(spec.basis.index()));
      if (const auto* role = std::get_if<t::RoleAssignmentSpec>(&spec.basis)) {
        digest.integer(static_cast<std::uint64_t>(role->role));
        digest.resources(role->scope);
      } else {
        const auto& explicit_spec = std::get<t::ExplicitAssignmentSpec>(spec.basis);
        auto permissions = explicit_spec.permissions;
        std::sort(permissions.begin(), permissions.end());
        digest.integer(static_cast<std::uint64_t>(permissions.size()));
        for (const auto permission : permissions) {
          digest.integer(static_cast<std::uint64_t>(permission));
        }
        digest.resources(explicit_spec.scope);
      }
      digest.integer(expected.has_value() ? 1 : 0);
      if (expected) {
        digest.integer(expected->value);
      }
      return digest.finish();
    } catch (...) {
      return unavailable();
    }
  }
  t::Outcome<t::Sha256Digest> revoke_assignment(const t::AssignmentId& target,
                                                t::ResourceVersion expected) override {
    try {
      if (!text(target.value) || expected.value == 0) {
        return invalid();
      }
      Digest digest{t::kRevokeAssignmentFingerprintDomain};
      digest.string(target.value);
      digest.integer(expected.value);
      return digest.finish();
    } catch (...) {
      return unavailable();
    }
  }
};
} // namespace

core::target::Outcome<std::unique_ptr<core::target::IdentityAssignmentFingerprintPort>>
make_assignment_fingerprints() {
  try {
    Digest probe{t::kSetAssignmentFingerprintDomain};
    static_cast<void>(probe.finish());
    return std::unique_ptr<core::target::IdentityAssignmentFingerprintPort>{
        std::make_unique<Fingerprints>()};
  } catch (...) {
    return unavailable();
  }
}
} // namespace kiarabot::infrastructure::identity_crypto
