#include <type_traits>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace target = kiarabot::core::target;
static_assert(!std::is_convertible_v<target::InstallationId, target::InstanceId>);
static_assert(!std::is_convertible_v<target::ExecutionGeneration, target::ConnectionGeneration>);
int main() {
  target::Outcome<int> unavailable =
      target::Failure{target::ErrorCode::UpstreamUnavailable, target::Acceptance::Unknown,
                      target::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  const auto& failure = std::get<target::Failure>(unavailable);
  return failure.acceptance == target::Acceptance::Unknown &&
                 failure.recovery == target::Recovery::QuerySameIdentity
             ? 0
             : 1;
}
