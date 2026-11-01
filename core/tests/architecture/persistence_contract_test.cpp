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
int main() {
  const target::ReceiptKey account{
      {"subject"}, "update", {target::ResourceKind::Account, {"same-id"}}, "same-key"};
  auto model = account;
  model.resource.kind = target::ResourceKind::Model;
  // Equal ID strings in distinct resource kinds must not alias a command receipt scope.
  return account == model ? 1 : 0;
}
