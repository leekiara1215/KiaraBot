// clang-format off
#include "kiarabot/core/ports/target/identity_verification_keys.hpp"

#include <cassert>
#include <type_traits>
#include <utility>
// clang-format on

namespace t = kiarabot::core::target;
static_assert(std::is_abstract_v<t::VerificationKeyMaterialPort>);
static_assert(std::is_same_v<decltype(std::declval<t::VerificationKeyMaterialPort&>().snapshot()),
                             t::Outcome<t::VerificationKeySet>>);
static_assert(std::is_same_v<decltype(t::PublicVerificationKey::ed25519_public_key),
                             std::array<std::byte, 32>>);
int main() try {
  const t::PublicVerificationKey key{{"key-1"}, {}, {1}, t::TimePoint{std::chrono::seconds{1000}}};
  const t::VerificationKeySet keys{
      {key}, {1}, {t::ResourceKind::Catalog, {"00000000-0000-4000-8000-000000000001"}}};
  assert(keys.keys.front().ed25519_public_key.size() == 32);
  assert(keys.version.value == 1);
  assert(keys.keyset_resource.kind == t::ResourceKind::Catalog);
} catch (...) {
  return 1;
}
