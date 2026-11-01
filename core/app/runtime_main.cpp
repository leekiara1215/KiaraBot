#include <iostream>

#include "kiarabot/core/application/target/runtime_application.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

int main(int argc, char* argv[]) {
  try {
    const auto application = kiarabot::core::target::make_runtime_application();
    return kiarabot::core::platform::run_service(*application, argc, argv);
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
