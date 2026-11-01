// clang-format off
#include "kiarabot/core/ports/target/identity_assignment_fingerprint.hpp"

#include <type_traits>
#include <utility>
// clang-format on

namespace t = kiarabot::core::target;
static_assert(std::is_abstract_v<t::IdentityAssignmentFingerprintPort>);
static_assert(
    std::is_same_v<decltype(std::declval<t::IdentityAssignmentFingerprintPort&>().set_assignment(
                       std::declval<const t::ExactAssignmentSpec&>(),
                       std::optional<t::AssignmentId>{}, std::optional<t::ResourceVersion>{})),
                   t::Outcome<t::Sha256Digest>>);
static_assert(
    std::is_same_v<decltype(std::declval<t::IdentityAssignmentFingerprintPort&>().revoke_assignment(
                       std::declval<const t::AssignmentId&>(), t::ResourceVersion{1})),
                   t::Outcome<t::Sha256Digest>>);
static_assert(t::kSetAssignmentFingerprintDomain == "kiarabot.identity.SetAssignment.v1");
static_assert(t::kRevokeAssignmentFingerprintDomain == "kiarabot.identity.RevokeAssignment.v1");
static_assert(t::kSetAssignmentFingerprintDomain != t::kRevokeAssignmentFingerprintDomain);
int main() {}
