#include <iostream>

#include "kiarabot/control/application/target/application.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

int main(int argc, char* argv[]) {
  try {
    const auto application = kiarabot::control::target::make_application();
    return kiarabot::core::platform::run_service(*application, argc, argv);
  } catch (...) {
    std::cerr << "{\"service\":\"control\",\"error\":\"INTERNAL\",\"stage\":\"startup\"}\n";
    return 1;
  }
}
