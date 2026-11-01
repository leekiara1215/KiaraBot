#include <cstdlib>
#include <type_traits>

#include "kiarabot/core/domain/target/persistence.hpp"

namespace target = kiarabot::core::target;
static_assert(
    std::is_same_v<std::variant_alternative_t<11, target::ReplayResult>, target::ModelIntent>);
static_assert(
    std::is_same_v<std::variant_alternative_t<12, target::ReplayResult>, target::SessionMetadata>);
static_assert(std::is_same_v<std::variant_alternative_t<13, target::ReplayResult>,
                             target::RegisteredPrincipalSnapshot>);
void check(bool condition) {
  if (!condition) {
    std::abort();
  }
}
int main() {
  const target::TimePoint created{std::chrono::seconds{100}};
  const target::ResourceRef resource{target::ResourceKind::Principal, {"principal"}};
  target::RegisteredPrincipalSnapshot registered{
      {{"principal"}, target::PrincipalKind::Human, resource, {1}, target::LifecycleState::Active},
      {{{"initial-credential"}, {1}},
       {"principal"},
       resource,
       target::CredentialStatusState::Pending,
       created,
       std::nullopt,
       created}};
  target::ReplayResult original = registered;
  check(original.index() == 13);
  // Subsequent current-state changes cannot change the stored original public success.
  registered.principal.version = {2};
  registered.principal.state = target::LifecycleState::Disabled;
  registered.initial_credential.ref.version = {2};
  registered.initial_credential.state = target::CredentialStatusState::Active;
  registered.initial_credential.expires_at = created + std::chrono::hours{1};
  const auto& replay = std::get<target::RegisteredPrincipalSnapshot>(original);
  check(replay.principal.id.value == "principal" && replay.principal.version.value == 1 &&
        replay.principal.state == target::LifecycleState::Active);
  check(replay.initial_credential.ref.id.value == "initial-credential" &&
        replay.initial_credential.ref.version.value == 1 &&
        replay.initial_credential.state == target::CredentialStatusState::Pending &&
        !replay.initial_credential.expires_at && replay.initial_credential.created_at == created &&
        replay.initial_credential.checked_at == created);
}
