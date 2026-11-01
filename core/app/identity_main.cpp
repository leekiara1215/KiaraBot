#include <iostream>

#include "identity_composition.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace target = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace composition = kiarabot::core::composition;

int main(int argc, char* argv[]) {
  try {
    return platform::run_service(target::Service::Identity, composition::compose_identity_service,
                                 argc, argv);
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
