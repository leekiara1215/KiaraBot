#include <iostream>

#include "kiarabot/core/runtime.h"

int main() {
  kiarabot::core::Runtime runtime;
  const auto result =
      runtime.ingest(kiarabot::core::EventRecord{"startup-check", "startup", "", 0, false});
  if (result != kiarabot::core::EventResult::accepted) return 1;
  std::cout << "kiarabot-core ready; inbox=" << runtime.inbox_size() << "\n";
  return 0;
}
