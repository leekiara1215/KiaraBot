#include <iostream>
#include <memory>

#include "kiarabot/control/application/target/application.hpp"
#include "kiarabot/control/application/target/readiness.hpp"
#include "kiarabot/control/target/https_endpoint.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

int main(int argc, char* argv[]) {
  try {
    namespace platform = kiarabot::core::platform;
    namespace target = kiarabot::core::target;
    return platform::run_service(
        target::Service::Control,
        [](const platform::ProcessConfig& config) -> target::Outcome<platform::ServiceComposition> {
          auto readiness = std::make_shared<kiarabot::control::target::ControlReadiness>();
          return platform::ServiceComposition{
              kiarabot::control::target::make_application(readiness),
              {},
              {},
              kiarabot::control::target::make_https_endpoint(config, readiness)};
        },
        argc, argv);
  } catch (...) {
    std::cerr << "{\"service\":\"control\",\"error\":\"INTERNAL\",\"stage\":\"startup\"}\n";
    return 1;
  }
}
