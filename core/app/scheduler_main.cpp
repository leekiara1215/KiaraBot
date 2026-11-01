#include <iostream>

#include "kiarabot/core/application/target/scheduler_application.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "postgres_service.hpp"

namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace composition = kiarabot::core::composition;

int main(int argc, char* argv[]) {
  try {
    return platform::run_service(
        target::Service::Scheduler,
        [](const platform::ProcessConfig& config) {
          return composition::compose_database_service<target::SchedulerUnitOfWork>(
              config, kiarabot::infrastructure::postgres::open_scheduler,
              [](auto&) { return target::make_scheduler_application(); });
        },
        argc, argv);
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
