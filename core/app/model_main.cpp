#include <iostream>

#include "kiarabot/core/runtime/platform/service_process.hpp"
#include "model_composition.hpp"
#include "postgres_service.hpp"

namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace composition = kiarabot::core::composition;

int main(int argc, char* argv[]) {
  try {
    return platform::run_service(
        target::Service::Model,
        [](const platform::ProcessConfig& config) {
          return composition::compose_model_service(config);
        },
        argc, argv);
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
