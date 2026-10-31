#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <unordered_map>

#include "kiarabot/core/application/control_service.hpp"

namespace kiarabot::control {
struct StreamReply {
  std::vector<core::ControlJson> frames;
  unsigned short close_code{0};
};
// Single executor ownership; all callers must use the server io_context thread.
class EventStream {
public:
  using Source = std::function<core::PortResult<core::ControlJson>(const core::ControlMetadata&)>;
  explicit EventStream(Source source, core::RuntimeClock clock = {});
  StreamReply handshake(const std::string& channel, const core::ControlJson& frame);
  StreamReply poll(const std::string& id, std::uint64_t sequence);
  void detach(const std::string& id);
  // Live sessions are refreshed once per tick, sharing one Core snapshot per access token.
  // Detached sessions stop polling immediately and are refreshed by the next resume instead.
  void tick();
  void set_max_streams_per_token(std::uint32_t value);

private:
  struct Event {
    core::WallTime occurred;
    core::ControlJson frame;
  };
  struct Session {
    std::string channel;
    std::string token;
    core::ControlJson previous;
    core::WallTime detached_at{};
    std::uint64_t sequence{0};
    std::uint64_t oldest{0};
    unsigned short failure{0};
    std::deque<Event> events;
  };
  core::PortResult<core::ControlJson> snapshot(const std::string& token);
  void update(Session& session);
  void update(Session& session, const core::PortResult<core::ControlJson>& current);
  std::size_t active_streams(const std::string& token) const;
  std::string instance_{core::control_secret().substr(0, 32)};
  Source source_;
  core::RuntimeClock clock_;
  std::unordered_map<std::string, Session> sessions_;
  std::uint32_t max_streams_per_token_{16};
};
} // namespace kiarabot::control
