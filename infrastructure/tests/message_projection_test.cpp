#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "kiarabot/infrastructure/message_projection.hpp"

namespace {
using namespace std::chrono_literals;

void check(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

kiarabot::core::MessageMetadata message(const std::string& id) {
  return {id, "conversation-" + id, "received"};
}
} // namespace

int main() try {
  auto now = kiarabot::core::RuntimeTimePoint(1700000000s);
  kiarabot::infrastructure::BufferedMessageProjection projection([&] { return now; }, 2);

  check(projection.read().empty(), "empty projection");
  check(projection.write(message("m1")), "write m1");
  check(projection.write(message("m2")), "write m2");
  auto records = projection.read();
  check(records.size() == 2 && records.front().message_id == "m1" &&
            records.back().direction == "received",
        "insertion order");

  // A redelivered message must not duplicate a notification.
  check(projection.write(message("m1")), "duplicate write");
  check(projection.read().size() == 2, "duplicate suppressed");

  // The bound evicts the oldest entry first.
  check(projection.write(message("m3")), "write m3");
  records = projection.read();
  check(records.size() == 2 && records.front().message_id == "m2" &&
            records.back().message_id == "m3",
        "bounded projection");

  // Entries older than the ten-minute replay window are dropped.
  now += 11min;
  check(projection.read().empty(), "retention expiry");
  check(projection.write(message("m4")), "write m4");
  check(projection.read().size() == 1, "post-expiry write");

  // The projection carries metadata only.
  const auto final_records = projection.read();
  check(final_records.size() == 1, "final projection size");
  const auto& only = final_records.front();
  check(only.message_id == "m4" && only.conversation_id == "conversation-m4", "metadata fields");
  std::cout << "message projection checks passed\n";
  return 0;
} catch (const std::exception& failure) {
  std::cerr << "message projection test failed: " << failure.what() << '\n';
  return 1;
}
