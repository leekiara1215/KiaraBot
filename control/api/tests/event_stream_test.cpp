#include <iostream>
#include <stdexcept>

#include "kiarabot/control/event_stream.hpp"

namespace {
using Json = kiarabot::core::ControlJson;
using namespace std::chrono_literals;
void check(bool value) {
  if (!value) {
    throw std::runtime_error("stream assertion failed");
  }
}
} // namespace
int main() try {
  auto now = kiarabot::core::WallTime(1700000000s);
  bool revoked = false;
  Json projection{{"user_id", "user"},
                  {"session_id", "auth-session"},
                  {"permissions", Json::array({"plugin.read", "health.read"})},
                  {"plugins", Json::array({Json{{"id", "test.plugin"},
                                                {"version", "1"},
                                                {"state", "PLUGIN_STATE_RUNNING"}}})}};
  kiarabot::control::EventStream events(
      [&](const auto&) -> kiarabot::core::PortResult<Json> {
        if (revoked) {
          kiarabot::v1::Error error;
          error.set_code(kiarabot::v1::UNAUTHENTICATED);
          return error;
        }
        return projection;
      },
      [&] { return now; });
  const Json hello{
      {"type", "hello"}, {"access_token", std::string(64, 'a')}, {"last_sequence", 0u}};
  check(events.handshake("logs", hello).close_code == 4403);
  check(events.handshake("messages", hello).close_code == 4403);
  check(events.handshake("plugins", Json{{"type", "hello"}}).close_code == 4400);
  auto ready = events.handshake("plugins", hello);
  check(ready.close_code == 0 && ready.frames.size() == 1);
  const std::string id = ready.frames[0].at("session_id");
  projection["plugins"][0]["state"] = "PLUGIN_STATE_DISABLED";
  events.tick();
  auto live = events.poll(id, 0);
  check(live.frames.size() == 1 && live.frames[0].at("sequence") == 1);
  check(events.poll(id, 1).frames.empty());
  events.detach(id);
  Json resume{{"type", "resume"},
              {"access_token", std::string(64, 'b')},
              {"session_id", id},
              {"last_sequence", 0u}};
  auto replay = events.handshake("plugins", resume);
  check(replay.frames.size() == 2 && replay.frames[0].at("last_sequence") == 0);
  check(replay.frames[1] == live.frames[0]);
  events.detach(id);
  check(events.handshake("control", resume).close_code == 4403);
  projection["user_id"] = "other";
  check(events.handshake("plugins", resume).close_code == 4403);
  projection["user_id"] = "user";
  check(events.handshake("plugins", resume).close_code == 0);
  now += 601s;
  events.tick();
  check(events.poll(id, 0).frames[0].at("reason") == "CURSOR_EXPIRED");
  check(events.poll(id, 1).frames.empty());
  // A backlog larger than the socket queue is delivered in bounded batches.
  for (int index = 0; index < 70; ++index) {
    projection["plugins"][0]["version"] = std::to_string(index);
    events.tick();
  }
  const auto batch = events.poll(id, 1);
  check(batch.frames.size() == 32);
  const auto next = events.poll(id, batch.frames.back().at("sequence"));
  check(next.frames.size() == 32);
  revoked = true;
  events.tick();
  check(events.poll(id, 1).close_code == 4401);
  std::cout << "stream binding/replay/retention/revocation passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
