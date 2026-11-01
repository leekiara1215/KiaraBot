#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "kiarabot/core/application/target/host_application.hpp"
#include "kiarabot/core/application/target/identity_application.hpp"
#include "kiarabot/core/application/target/model_application.hpp"
#include "kiarabot/core/application/target/runtime_application.hpp"
#include "kiarabot/core/application/target/scheduler_application.hpp"

namespace target = kiarabot::core::target;
void verify(std::unique_ptr<target::ServiceReadinessPort> application, target::Service expected) {
  const auto result = application->readiness();
  const bool gate = std::any_of(
      result.dependencies.begin(), result.dependencies.end(), [](const auto& dependency) {
        return dependency.dependency == target::Dependency::BusinessImplementation &&
               dependency.state == target::DependencyState::NotImplemented;
      });
  if (application->service() != expected || result.ready || !gate) {
    std::cerr << "service must keep its own identity and unimplemented gate\n";
    std::exit(1);
  }
}
int main() {
  verify(target::make_identity_application(), target::Service::Identity);
  verify(target::make_runtime_application(), target::Service::Runtime);
  verify(target::make_host_application(), target::Service::Host);
  verify(target::make_scheduler_application(), target::Service::Scheduler);
  verify(target::make_model_application(), target::Service::Model);
}
