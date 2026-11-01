#include <iostream>
#include <string>

#include "kiarabot/core/runtime/platform/service_config.hpp"

int main(int argc, char* argv[]) {
  if (argc != 2 && argc != 3) {
    return 2;
  }
  const auto now = argc == 3 ? std::chrono::system_clock::from_time_t(std::stoll(argv[2]))
                             : std::chrono::system_clock::now();
  const auto result = kiarabot::core::platform::load_service_config(
      argv[1], kiarabot::core::target::Service::Identity, now);
  if (const auto* failure = std::get_if<kiarabot::core::platform::ConfigFailure>(&result)) {
    std::cerr << failure->reason << '\n';
    return failure->exit_code;
  }
  return 0;
}
