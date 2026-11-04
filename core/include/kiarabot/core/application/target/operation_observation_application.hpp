#pragma once

#include <functional>
#include <memory>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

struct OperationObservationCommand {
  CommandContext context{};
  OperationObservationQuery query;
};
struct OperationObservationDependencies {
  Service owner;
  // Adapts an owner-specific factory without sharing databases or retaining a UoW.
  std::function<Outcome<std::unique_ptr<UnitOfWork>>()> begin;
  // Reuses the existing pure exact-scope/current-decision vocabulary. The method
  // and audience identify the owner; the adapter must call Identity online.
  RuntimeAuthorizationPort& authorization;
  std::function<TimePoint()> now;
};

class OperationObservationApplication {
public:
  virtual ~OperationObservationApplication() = default;
  virtual Outcome<OperationObservationPage> query(const OperationObservationCommand& command) = 0;
};

// Control platform peer only. No Runtime-origin/instance/platform-peer allowlist
// fallback. Owner authorization finishes before a local, read-only UoW is opened.
Outcome<std::unique_ptr<OperationObservationApplication>>
make_operation_observation_application(OperationObservationDependencies dependencies);

} // namespace kiarabot::core::target
