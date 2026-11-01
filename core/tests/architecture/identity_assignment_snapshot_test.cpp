// clang-format off
#include "kiarabot/core/domain/target/persistence.hpp"

#include <cassert>
#include <type_traits>

#include "kiarabot/core/domain/target/identity_policy.hpp"
// clang-format on

namespace t = kiarabot::core::target;
static_assert(std::variant_size_v<t::ReplayResult> == 15);
static_assert(std::is_same_v<std::variant_alternative_t<0, t::ReplayResult>, t::EmptySuccess>);
static_assert(
    std::is_same_v<std::variant_alternative_t<1, t::ReplayResult>, t::ResourceMutationResult>);
static_assert(std::is_same_v<std::variant_alternative_t<2, t::ReplayResult>, t::Operation>);
static_assert(std::is_same_v<std::variant_alternative_t<3, t::ReplayResult>, t::Principal>);
static_assert(std::is_same_v<std::variant_alternative_t<4, t::ReplayResult>, t::LoginSession>);
static_assert(std::is_same_v<std::variant_alternative_t<5, t::ReplayResult>, t::ResourceGrant>);
static_assert(std::is_same_v<std::variant_alternative_t<6, t::ReplayResult>, t::BotAccount>);
static_assert(
    std::is_same_v<std::variant_alternative_t<7, t::ReplayResult>, t::PluginInstallation>);
static_assert(std::is_same_v<std::variant_alternative_t<8, t::ReplayResult>, t::PluginInstance>);
static_assert(std::is_same_v<std::variant_alternative_t<9, t::ReplayResult>, t::ScheduledTask>);
static_assert(std::is_same_v<std::variant_alternative_t<10, t::ReplayResult>, t::Occurrence>);
static_assert(std::is_same_v<std::variant_alternative_t<11, t::ReplayResult>, t::ModelIntent>);
static_assert(std::is_same_v<std::variant_alternative_t<12, t::ReplayResult>, t::SessionMetadata>);
static_assert(std::is_same_v<std::variant_alternative_t<13, t::ReplayResult>,
                             t::RegisteredPrincipalSnapshot>);
static_assert(std::is_same_v<std::variant_alternative_t<14, t::ReplayResult>, t::ExactAssignment>);
static_assert(!std::is_constructible_v<t::ReplayResult, t::SecretReplayPayload>);

int main() try {
  const t::SubjectId owner{"11111111-1111-4111-8111-111111111111"};
  const t::ResourceRef first{t::ResourceKind::Account, {"22222222-2222-4222-8222-222222222222"}};
  const t::ResourceRef second{t::ResourceKind::Account, {"33333333-3333-4333-8333-333333333333"}};
  t::ExactAssignment assignment{
      {"44444444-4444-4444-8444-444444444444"},
      owner,
      {},
      {{t::Permission::AccountRead, first}, {t::Permission::AccountRead, second}},
      {7}};
  assert(std::holds_alternative<std::monostate>(t::validate_exact_assignment(assignment)));
  const t::CommandReceipt receipt{{owner,
                                   "SetAssignment",
                                   {t::ResourceKind::Assignment, {assignment.id.value}},
                                   "original-key"},
                                  {},
                                  assignment,
                                  t::TimePoint{std::chrono::seconds{1000}},
                                  t::TimePoint{std::chrono::seconds{1000}} +
                                      t::kMinimumReceiptRetention};
  assert(receipt.result.index() == 14);
  assignment.subject = {"55555555-5555-4555-8555-555555555555"};
  assignment.version.value = 8;
  assignment.explicit_permissions.clear();
  const auto& original = std::get<t::ExactAssignment>(receipt.result);
  assert(original.subject == owner && original.version.value == 7);
  assert(original.roles.empty() && original.explicit_permissions.size() == 2);
  assert(original.explicit_permissions[0].resource == first);
  assert(original.explicit_permissions[1].resource == second);
  const t::ExactAssignment role{{"66666666-6666-4666-8666-666666666666"},
                                owner,
                                {{t::FixedRole::Viewer, first}, {t::FixedRole::Viewer, second}},
                                {},
                                {1}};
  assert(std::holds_alternative<std::monostate>(t::validate_exact_assignment(role)));
  const t::ReplayResult role_result = role;
  const auto& stored_role = std::get<t::ExactAssignment>(role_result);
  assert(stored_role.roles == role.roles && stored_role.explicit_permissions.empty());
} catch (...) {
  return 1;
}
