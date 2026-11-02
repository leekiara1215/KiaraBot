#include <iostream>

#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "scheduler_composition.hpp"

namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace composition = kiarabot::core::composition;

int main(int argc, char* argv[]) {
  try {
    return platform::run_service(target::Service::Scheduler, composition::compose_scheduler_service,
                                 argc, argv);
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
