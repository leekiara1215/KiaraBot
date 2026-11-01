#include <iostream>
#include <string>

#include "kiarabot/core/runtime/platform/service_config.hpp"

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 4) {
    return 2;
  }
  const auto now = argc >= 3 ? std::chrono::system_clock::from_time_t(std::stoll(argv[2]))
                             : std::chrono::system_clock::now();
  using Service = kiarabot::core::target::Service;
  auto service = Service::Identity;
  if (argc == 4) {
    const std::string requested = argv[3];
    if (requested == "host") {
      service = Service::Host;
    } else if (requested == "control") {
      service = Service::Control;
    } else if (requested == "runtime") {
      service = Service::Runtime;
    } else if (requested == "scheduler") {
      service = Service::Scheduler;
    } else if (requested == "model") {
      service = Service::Model;
    } else {
      return 2;
    }
  }
  const auto result = kiarabot::core::platform::load_service_config(argv[1], service, now);
  if (const auto* failure = std::get_if<kiarabot::core::platform::ConfigFailure>(&result)) {
    std::cerr << failure->reason << '\n';
    return failure->exit_code;
  }
  const auto& config = std::get<kiarabot::core::platform::ProcessConfig>(result);
  std::cout << "identity_security=" << (config.identity_security ? "true" : "false") << '\n';
  return 0;
}
