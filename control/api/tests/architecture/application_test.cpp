#include <cstdlib>
#include <iostream>
#include <memory>

#include "kiarabot/control/application/target/application.hpp"

namespace target = kiarabot::core::target;

void require(bool accepted, const char* scenario) {
  if (!accepted) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}

int main() {
  // Without composition observations the historical placeholder must stay unavailable.
  const auto placeholder = kiarabot::control::target::make_application();
  require(placeholder->service() == target::Service::Control,
          "Control must retain its own service identity");
  require(!placeholder->readiness().ready,
          "uncomposed Control cannot advertise ready without dependencies");

  auto state = std::make_shared<kiarabot::control::target::ControlReadiness>();
  const auto application = kiarabot::control::target::make_application(state);
  require(!application->readiness().ready, "fresh observations must not be ready");
  bool identity_unverified = false;
  bool credentials_unverified = false;
  for (const auto& dependency : application->readiness().dependencies) {
    identity_unverified |= dependency.dependency == target::Dependency::Identity &&
                           dependency.state != target::DependencyState::Available;
    credentials_unverified |= dependency.dependency == target::Dependency::Credentials &&
                              dependency.state != target::DependencyState::Available;
    require(dependency.dependency != target::Dependency::DatabaseSchema,
            "Control must not acquire an authoritative business database");
  }
  require(identity_unverified && credentials_unverified,
          "Control retains Identity and credential gates");

  state->business.store(true);
  state->credentials.store(true);
  require(!application->readiness().ready,
          "Identity channel must be reachable before Control readiness");
  state->identity.store(true);
  const auto ready = application->readiness();
  require(ready.ready, "Control becomes ready after business/credentials/Identity are observed");
  for (const auto& dependency : ready.dependencies) {
    require(dependency.state == target::DependencyState::Available,
            "ready snapshot reports all observed dependencies available");
  }
  state->identity.store(false);
  require(!application->readiness().ready, "losing Identity readiness must gate Control");
  state->identity.store(true);

  auto changed = application->readiness();
  changed.ready = false;
  changed.dependencies.clear();
  require(application->readiness().ready && !application->readiness().dependencies.empty(),
          "caller changes to a readiness snapshot must not enable/clear the application");
}
