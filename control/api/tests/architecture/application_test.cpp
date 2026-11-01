#include <cstdlib>
#include <iostream>

#include "kiarabot/control/application/target/application.hpp"

namespace target = kiarabot::core::target;

void require(bool accepted, const char* scenario) {
  if (!accepted) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}

int main() {
  const auto application = kiarabot::control::target::make_application();
  require(application->service() == target::Service::Control,
          "Control must retain its own service identity");
  auto snapshot = application->readiness();
  require(!snapshot.ready, "P02 cannot advertise ready without business use cases");
  bool denies_missing_business = false;
  bool requires_identity = false;
  bool requires_credentials = false;
  for (const auto& dependency : snapshot.dependencies) {
    denies_missing_business |=
        dependency.dependency == target::Dependency::BusinessImplementation &&
        dependency.state == target::DependencyState::NotImplemented;
    requires_identity |= dependency.dependency == target::Dependency::Identity &&
                         dependency.state != target::DependencyState::Available;
    requires_credentials |= dependency.dependency == target::Dependency::Credentials &&
                            dependency.state != target::DependencyState::Available;
    require(dependency.dependency != target::Dependency::DatabaseSchema,
            "Control must not acquire an authoritative business database");
  }
  require(denies_missing_business && requires_identity && requires_credentials,
          "Control must retain business, Identity and credential gates");
  snapshot.ready = true;
  snapshot.dependencies.clear();
  require(!application->readiness().ready,
          "caller changes to a readiness snapshot must not enable the application");
  require(!application->readiness().dependencies.empty(), "caller cannot erase application gates");
}
