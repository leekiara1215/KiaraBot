#include <stdexcept>

#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace target = kiarabot::core::target;
void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("grant metadata projection assertion failed");
  }
}
int main() {
  const target::ResourceRef first{target::ResourceKind::Account,
                                  {"11111111-1111-4111-8111-111111111111"}};
  const target::ResourceRef second{target::ResourceKind::Account,
                                   {"22222222-2222-4222-8222-222222222222"}};
  const target::ExactAssignment roles{
      {"33333333-3333-4333-8333-333333333333"},
      {"44444444-4444-4444-8444-444444444444"},
      {{target::FixedRole::Admin, first}, {target::FixedRole::Admin, second}},
      {},
      {7}};
  const auto role_result = target::to_resource_grant_metadata(roles);
  const auto* role = std::get_if<target::ResourceGrant>(&role_result);
  require(role && role->id == roles.id && role->subject == roles.subject &&
          role->version == roles.version && role->role == target::FixedRole::Admin &&
          !role->scopes.empty());
  bool first_read = false;
  bool second_read = false;
  for (const auto& scope : role->scopes) {
    require(scope.resource == first || scope.resource == second);
    require(scope.permission != "message.read" && scope.permission != "action.inspect");
    if (scope.permission == "resource.read") {
      first_read = first_read || scope.resource == first;
      second_read = second_read || scope.resource == second;
    }
  }
  require(first_read && second_read);
  const target::ExactAssignment explicit_grant{
      roles.id, roles.subject, {}, {{target::Permission::MessageRead, first}}, {8}};
  const auto explicit_result = target::to_resource_grant_metadata(explicit_grant);
  const auto* explicit_metadata = std::get_if<target::ResourceGrant>(&explicit_result);
  require(explicit_metadata && !explicit_metadata->role && explicit_metadata->scopes.size() == 1 &&
          explicit_metadata->scopes.front().resource == first &&
          explicit_metadata->scopes.front().permission == "message.read");
  auto corrupt = explicit_grant;
  corrupt.explicit_permissions.push_back({target::Permission::ResourceRead, second});
  const auto invalid = target::to_resource_grant_metadata(corrupt);
  require(std::get<target::Failure>(invalid).code == target::ErrorCode::Internal);
}
