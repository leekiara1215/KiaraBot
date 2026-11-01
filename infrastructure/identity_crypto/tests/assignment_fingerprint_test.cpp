#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <string>

#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/infrastructure/identity_crypto/assignment_fingerprint.hpp"

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
  auto made = crypto::make_assignment_fingerprints();
  auto& port = *std::get<std::unique_ptr<t::IdentityAssignmentFingerprintPort>>(made);
  const t::AssignmentId id{"assignment"};
  const t::ResourceVersion expected{7};
  const t::ResourceRef first{t::ResourceKind::Account, {"resource-one"}};
  const t::ResourceRef second{t::ResourceKind::Account, {"resource-two"}};
  const t::ExactAssignmentSpec role{{"owner"}, t::RoleAssignmentSpec{t::FixedRole::Admin, {first}}};
  const auto original = std::get<t::Sha256Digest>(port.set_assignment(role, id, expected));
  // Golden values independently calculated from the documented u64-BE framing using hashlib.
  assert(hex(original) == "818b104fd2361c37a9b11a4b4b6af1480603a41dbe4ca32f1525820e81013742");
  const auto revoke = std::get<t::Sha256Digest>(port.revoke_assignment(id, expected));
  assert(hex(revoke) == "ebb942b8c5ab042356f532e16a49e4427e60731082d1ca3f02ecc083dbc9bda3");
  assert(original != revoke);
  assert(original != std::get<t::Sha256Digest>(port.set_assignment(role, {}, {})));
  assert(original !=
         std::get<t::Sha256Digest>(port.set_assignment(role, id, t::ResourceVersion{8})));
  assert(original !=
         std::get<t::Sha256Digest>(port.set_assignment(role, t::AssignmentId{"other"}, expected)));

  auto changed = role;
  changed.subject = {"other"};
  assert(original != std::get<t::Sha256Digest>(port.set_assignment(changed, id, expected)));
  changed = role;
  std::get<t::RoleAssignmentSpec>(changed.basis).role = t::FixedRole::Operator;
  assert(original != std::get<t::Sha256Digest>(port.set_assignment(changed, id, expected)));
  changed = role;
  std::get<t::RoleAssignmentSpec>(changed.basis).scope[0].kind = t::ResourceKind::Adapter;
  assert(original != std::get<t::Sha256Digest>(port.set_assignment(changed, id, expected)));

  t::ExactAssignmentSpec explicit_spec{
      role.subject,
      t::ExplicitAssignmentSpec{{t::Permission::ResourceRead, t::Permission::AccountRead},
                                {second, first}}};
  const auto explicit_hash =
      std::get<t::Sha256Digest>(port.set_assignment(explicit_spec, id, expected));
  auto& basis = std::get<t::ExplicitAssignmentSpec>(explicit_spec.basis);
  std::reverse(basis.permissions.begin(), basis.permissions.end());
  std::reverse(basis.scope.begin(), basis.scope.end());
  assert(explicit_hash ==
         std::get<t::Sha256Digest>(port.set_assignment(explicit_spec, id, expected)));
  basis.permissions.pop_back();
  assert(explicit_hash !=
         std::get<t::Sha256Digest>(port.set_assignment(explicit_spec, id, expected)));
  basis.permissions.push_back(basis.permissions.front());
  invalid(port.set_assignment(explicit_spec, id, expected));
  explicit_spec = {role.subject, t::ExplicitAssignmentSpec{{t::Permission::Unspecified}, {first}}};
  invalid(port.set_assignment(explicit_spec, id, expected));
  explicit_spec = {role.subject, t::ExplicitAssignmentSpec{{}, {first}}};
  invalid(port.set_assignment(explicit_spec, id, expected));

  invalid(port.set_assignment(role, id, {}));
  invalid(port.set_assignment(role, {}, expected));
  invalid(port.set_assignment(role, t::AssignmentId{}, expected));
  invalid(port.set_assignment(role, id, t::ResourceVersion{0}));
  invalid(port.revoke_assignment({}, expected));
  invalid(port.revoke_assignment(id, {0}));
  changed = role;
  changed.subject.value = std::string{"owner\0suffix", 12};
  invalid(port.set_assignment(changed, id, expected));
  changed.subject.value.assign(1025, 'x');
  invalid(port.set_assignment(changed, id, expected));
  changed = role;
  std::get<t::RoleAssignmentSpec>(changed.basis).scope.clear();
  invalid(port.set_assignment(changed, id, expected));
  changed = role;
  std::get<t::RoleAssignmentSpec>(changed.basis).scope.push_back(first);
  invalid(port.set_assignment(changed, id, expected));
  changed = role;
  std::get<t::RoleAssignmentSpec>(changed.basis).scope.front().id = {"*"};
  invalid(port.set_assignment(changed, id, expected));
  changed = role;
  auto& scopes = std::get<t::RoleAssignmentSpec>(changed.basis).scope;
  scopes.clear();
  for (int index = 0; index < 128; ++index) {
    scopes.push_back({t::ResourceKind::Account, {std::to_string(index)}});
  }
  assert(std::holds_alternative<t::Sha256Digest>(port.set_assignment(changed, id, expected)));
  scopes.push_back(second);
  invalid(port.set_assignment(changed, id, expected));
  assert(std::holds_alternative<t::Sha256Digest>(
      port.revoke_assignment(id, {std::numeric_limits<std::uint64_t>::max()})));
} catch (...) {
  return 1;
}
