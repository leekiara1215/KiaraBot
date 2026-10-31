#include <iostream>
#include <thread>

#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

int main(int argc, char* argv[]) {
  if (argc != 2 || std::string_view(argv[1]).empty() || std::string_view(argv[1]) == ":memory:" ||
      std::string_view(argv[1]).starts_with("file:")) {
    return 2;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (true) {
    const auto opened = kiarabot::infrastructure::SqliteRuntimeStore::open(argv[1]);
    const auto* error = std::get_if<kiarabot::core::StorageError>(&opened);
    if (!error) {
      std::cout << "operation=migrate outcome=completed schema=6\n";
      return 0;
    }
    if (*error != kiarabot::core::StorageError::Conflict ||
        std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "operation=migrate outcome=failed error_code="
                << (*error == kiarabot::core::StorageError::Corrupt ? "INTERNAL"
                                                                    : "UPSTREAM_UNAVAILABLE")
                << '\n';
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
