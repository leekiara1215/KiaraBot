#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>

#include "kiarabot/infrastructure/identity_crypto/principal_fingerprint.hpp"

namespace t = kiarabot::core::target;
namespace crypto = kiarabot::infrastructure::identity_crypto;

std::string hex(const t::Sha256Digest& digest) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  for (const auto byte : digest.bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    result += digits[value >> 4];
    result += digits[value & 15];
  }
  return result;
}
void invalid(const t::Outcome<t::Sha256Digest>& result) {
  const auto& error = std::get<t::Failure>(result);
  assert(error.code == t::ErrorCode::InvalidArgument &&
         error.recovery == t::Recovery::CorrectInput &&
         error.acceptance == t::Acceptance::NotAccepted);
}
int main() try {
  auto made = crypto::make_principal_fingerprints();
  auto& port = *std::get<std::unique_ptr<t::IdentityPrincipalFingerprintPort>>(made);
  const t::ResourceRef adapter{t::ResourceKind::Adapter, {"22222222-2222-4222-8222-222222222222"}};
  const std::vector<t::Service> services{t::Service::Runtime, t::Service::Scheduler};
  // Golden value independently calculated from the documented u64-BE framing using hashlib.
  const auto original = std::get<t::Sha256Digest>(
      port.register_principal(t::PrincipalKind::Adapter, adapter, services));
  assert(hex(original) == "0f5fafcf85f89593d033a459b2e65d383deb6ab55d590814c19d987df430fcde");
  auto reordered = services;
  std::reverse(reordered.begin(), reordered.end());
  assert(std::get<t::Sha256Digest>(
             port.register_principal(t::PrincipalKind::Adapter, adapter, reordered)) == original);
  assert(std::get<t::Sha256Digest>(port.register_principal(
             t::PrincipalKind::Plugin,
             {t::ResourceKind::Instance, {"22222222-2222-4222-8222-222222222222"}},
             {t::Service::Identity})) != original);
  assert(std::get<t::Sha256Digest>(port.register_principal(
             t::PrincipalKind::Adapter,
             {t::ResourceKind::Adapter, {"33333333-3333-4333-8333-333333333333"}}, services)) !=
         original);
  assert(std::get<t::Sha256Digest>(port.register_principal(t::PrincipalKind::Adapter, adapter,
                                                           {t::Service::Runtime})) != original);

  // RevokeCredential domain: credential id and expected version are both part of the digest.
  const t::CredentialId credential{"44444444-4444-4444-8444-444444444444"};
  const auto revoke = std::get<t::Sha256Digest>(port.revoke_credential(credential, {7}));
  assert(hex(revoke) == "3464a767b752e195001cc0a8336b8ca3cfdd427f1c72459c906c7629edbe3f78");
  assert(revoke != std::get<t::Sha256Digest>(port.revoke_credential(credential, {8})));
  assert(revoke != std::get<t::Sha256Digest>(
                       port.revoke_credential(t::CredentialId{"other-credential"}, {7})));
  invalid(port.revoke_credential(credential, {0}));
  invalid(port.revoke_credential({}, {7}));

  // Invalid shapes: human registration, empty/duplicate/Control audience, invalid resource.
  invalid(port.register_principal(t::PrincipalKind::Human, adapter, services));
  invalid(port.register_principal(t::PrincipalKind::Adapter, adapter, {}));
  invalid(port.register_principal(t::PrincipalKind::Adapter, adapter,
                                  {t::Service::Runtime, t::Service::Runtime}));
  invalid(port.register_principal(t::PrincipalKind::Adapter, adapter,
                                  {t::Service::Runtime, t::Service::Control}));
  invalid(
      port.register_principal(t::PrincipalKind::Adapter, {t::ResourceKind::Adapter, {}}, services));
  return 0;
} catch (...) {
  return 1;
}
