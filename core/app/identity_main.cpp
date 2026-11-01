#include <iostream>

#include "kiarabot/core/application/target/identity_application.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "postgres_service.hpp"

namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace composition = kiarabot::core::composition;

int main(int argc, char* argv[]) {
  try {
    return platform::run_service(
        target::Service::Identity,
        [](const platform::ProcessConfig& config) -> target::Outcome<platform::ServiceComposition> {
          // Until the production security composition is connected, never ignore an explicit
          // security declaration and start the placeholder with unvalidated key material.
          if (config.identity_security) {
            return target::Failure{target::ErrorCode::CapabilityNotSupported,
                                   target::Acceptance::NotAccepted,
                                   target::Recovery::Diagnose,
                                   {},
                                   {}};
          }
          return composition::compose_database_service<target::IdentityUnitOfWork>(
              config, kiarabot::infrastructure::postgres::open_identity,
              [](auto&) { return target::make_identity_application(); });
        },
        argc, argv);
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
