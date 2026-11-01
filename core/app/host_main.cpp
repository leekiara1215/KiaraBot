#include "kiarabot/core/application/target/host_application.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

int main(int argc, char* argv[]) {
  const auto application = kiarabot::core::target::make_host_application();
  return kiarabot::core::platform::run_service(*application, argc, argv);
}
