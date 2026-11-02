#include <iostream>

#if defined(KIARABOT_HOST_COMPOSITION)
int run_host_service(int argc, char* argv[]);
#else
#include "kiarabot/core/application/target/host_application.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"
#endif

int main(int argc, char* argv[]) {
  try {
#if defined(KIARABOT_HOST_COMPOSITION)
    return run_host_service(argc, argv);
#else
    const auto application = kiarabot::core::target::make_host_application();
    return kiarabot::core::platform::run_service(*application, argc, argv);
#endif
  } catch (...) {
    std::cerr << "operation=start outcome=failed error_code=INTERNAL\n";
    return 1;
  }
}
