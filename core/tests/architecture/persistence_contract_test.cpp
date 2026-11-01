#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace target = kiarabot::core::target;
static_assert(std::is_same_v<decltype(std::declval<target::UnitOfWork&>().current_time()),
                             target::Outcome<target::TimePoint>>);
static_assert(std::is_same_v<decltype(std::declval<target::IdentityUnitOfWorkFactory&>().begin()),
                             target::Outcome<std::unique_ptr<target::IdentityUnitOfWork>>>);
static_assert(std::is_same_v<decltype(std::declval<target::RuntimeUnitOfWorkFactory&>().begin()),
                             target::Outcome<std::unique_ptr<target::RuntimeUnitOfWork>>>);
static_assert(std::is_same_v<decltype(std::declval<target::SchedulerUnitOfWorkFactory&>().begin()),
                             target::Outcome<std::unique_ptr<target::SchedulerUnitOfWork>>>);
static_assert(std::is_same_v<decltype(std::declval<target::ModelUnitOfWorkFactory&>().begin()),
                             target::Outcome<std::unique_ptr<target::ModelUnitOfWork>>>);
static_assert(
    std::is_same_v<decltype(std::declval<target::ModelIntent>().version.value), std::uint64_t>);
static_assert(
    std::is_same_v<decltype(std::declval<target::ScheduledTask>().version.value), std::uint64_t>);
// These internal values are persisted by P03 resource_kind CHECK constraints, unlike wire IDs.
constexpr std::array kOriginalKinds{
    target::ResourceKind::Account,    target::ResourceKind::Adapter,
    target::ResourceKind::PluginType, target::ResourceKind::Installation,
    target::ResourceKind::Instance,   target::ResourceKind::Host,
    target::ResourceKind::Namespace,  target::ResourceKind::Task,
    target::ResourceKind::Model,      target::ResourceKind::Principal,
    target::ResourceKind::Session,    target::ResourceKind::Action,
    target::ResourceKind::Operation,  target::ResourceKind::Credential,
    target::ResourceKind::Assignment};
static_assert([] {
  for (std::size_t i = 0; i < kOriginalKinds.size(); ++i) {
    if (static_cast<std::size_t>(kOriginalKinds[i]) != i) {
      return false;
    }
  }
  return static_cast<int>(target::ResourceKind::Catalog) == 15;
}());
int main() {
  const target::ReceiptKey account{
      {"subject"}, "update", {target::ResourceKind::Account, {"same-id"}}, "same-key"};
  auto model = account;
  model.resource.kind = target::ResourceKind::Model;
  // Equal ID strings in distinct resource kinds must not alias a command receipt scope.
  auto catalog = account;
  catalog.resource.kind = target::ResourceKind::Catalog;
  // Catalog never aliases a child resource even when a malicious ID string is identical.
  return account == model || account == catalog || model == catalog ? 1 : 0;
}
