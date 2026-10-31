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

  // One Core snapshot is shared by every live stream of the same access token, and detached
  // streams stop polling until their next resume.
  int snapshots = 0;
  kiarabot::control::EventStream shared(
      [&](const auto&) -> kiarabot::core::PortResult<Json> {
        ++snapshots;
        return projection;
      },
      [&] { return now; });
  shared.set_max_streams_per_token(2);
  const Json shared_hello{
      {"type", "hello"}, {"access_token", std::string(64, 'c')}, {"last_sequence", 0u}};
  const auto first_ready = shared.handshake("plugins", shared_hello);
  const auto second_ready = shared.handshake("plugins", shared_hello);
  check(first_ready.close_code == 0 && second_ready.close_code == 0);
  snapshots = 0;
  shared.tick();
  check(snapshots == 1);
  check(shared.handshake("plugins", shared_hello).close_code == 1013);
  const std::string detached = second_ready.frames[0].at("session_id");
  shared.detach(detached);
  snapshots = 0;
  shared.tick();
  check(snapshots == 1);
  projection["plugins"][0]["state"] = "PLUGIN_STATE_RUNNING";
  const auto resumed = shared.handshake("plugins", Json{{"type", "resume"},
                                                        {"access_token", std::string(64, 'c')},
                                                        {"session_id", detached},
                                                        {"last_sequence", 0u}});
  check(resumed.close_code == 0 && resumed.frames.size() == 2);
  check(resumed.frames[1].at("payload").at("state") == "PLUGIN_STATE_RUNNING");
  std::cout << "stream binding/replay/retention/revocation/snapshot-sharing passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
