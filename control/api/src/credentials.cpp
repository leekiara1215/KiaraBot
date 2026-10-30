#include <iostream>

#include "kiarabot/core/control_service.hpp"

int main(int argc, char* argv[]) try {
  if (argc == 2 && std::string_view(argv[1]) == "--key") {
    std::cout << kiarabot::core::control_secret() << '\n';
    return 0;
  }
  if (argc != 1) {
    return 2;
  }
  std::string password;
  if (!std::getline(std::cin, password) || password.empty() || password.size() > 1024) {
    return 2;
  }
  std::cout << kiarabot::core::control_password_hash(password) << '\n';
  return 0;
} catch (...) {
  return 1;
}
