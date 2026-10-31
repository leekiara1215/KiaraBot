#include "kiarabot/control/event_stream.hpp"

#include <algorithm>
#include <map>

namespace kiarabot::control {
namespace {
using Json = core::ControlJson;
const std::map<std::string, std::string> kPermissions{
    {"health", "health.read"},  {"logs", "log.read"},   {"adapters", "adapter.read"},
    {"plugins", "plugin.read"}, {"tasks", "task.read"}, {"messages", "message.read"}};
const std::map<std::string, std::string> kEvents{{"health", "health.updated"},
                                                 {"logs", "log.updated"},
                                                 {"adapters", "adapter.updated"},
                                                 {"plugins", "plugin.updated"},
                                                 {"tasks", "task.updated"}};
StreamReply resync(const std::string& reason) {
  return {{{{"type", "RESYNC_REQUIRED"}, {"reason", reason}}}, 1000};
}
bool allowed(const Json& snapshot, const std::string& channel) {
  if (channel == "control") {
    return true;
  }
  if (!kPermissions.contains(channel)) {
    return false;
  }
  const auto& permissions = snapshot.at("permissions");
  return std::find(permissions.begin(), permissions.end(), Json(kPermissions.at(channel))) !=
         permissions.end();
}
unsigned short close_code(const kiarabot::v1::Error& error) {
  return error.code() == kiarabot::v1::UNAUTHENTICATED     ? 4401
         : error.code() == kiarabot::v1::PERMISSION_DENIED ? 4403
                                                           : 1011;
}
} // namespace
EventStream::EventStream(Source source, core::RuntimeClock clock)
    : source_(std::move(source)), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = [] { return std::chrono::system_clock::now(); };
  }
}
void EventStream::set_max_streams_per_token(std::uint32_t value) {
  max_streams_per_token_ = std::max<std::uint32_t>(1, value);
}
std::size_t EventStream::active_streams(const std::string& token) const {
  return static_cast<std::size_t>(
      std::count_if(sessions_.begin(), sessions_.end(), [&](const auto& entry) {
        return entry.second.token == token && entry.second.detached_at == core::WallTime{} &&
               !entry.second.failure;
      }));
}
core::PortResult<Json> EventStream::snapshot(const std::string& token) {
  return source_({token, "stream", "stream", "", clock_() + std::chrono::seconds(30)});
}
StreamReply EventStream::handshake(const std::string& channel, const Json& frame) try {
  if (!frame.is_object() || !frame.contains("access_token") ||
      !frame.at("access_token").is_string() || !frame.contains("type") ||
      !frame.contains("last_sequence") || !frame.at("last_sequence").is_number_unsigned()) {
    return {{}, 4400};
  }
  const auto type = frame.at("type").get<std::string>();
  const auto token = frame.at("access_token").get<std::string>();
  const auto sequence = frame.at("last_sequence").get<std::uint64_t>();
  if ((type != "hello" && type != "resume") || token.size() < 20 || token.size() > 4096 ||
      (type == "hello" && sequence != 0) || sequence > 9007199254740991ULL) {
    return {{}, 4400};
  }
  const auto current = snapshot(token);
  if (const auto* error = std::get_if<kiarabot::v1::Error>(&current)) {
    return {{}, close_code(*error)};
  }
  const auto& projection = std::get<Json>(current);
  if (!allowed(projection, channel)) {
    return {{}, 4403};
  }
  std::string id;
  if (type == "hello") {
    if (sessions_.size() >= 1024) {
      return {{}, 1011};
    }
    if (active_streams(token) >= max_streams_per_token_) {
      return {{}, 1013};
    }
    id = instance_ + core::control_secret();
    sessions_[id] = Session{channel, token, projection, {}, 0, 0, 0, {}};
  } else {
    id = frame.at("session_id").get<std::string>();
    if (id.size() < 32 || id.size() > 256) {
      return {{}, 4400};
    }
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
      return resync(id.starts_with(instance_) ? "CURSOR_EXPIRED" : "SERVER_RESTART");
    }
    auto& session = found->second;
    if (session.channel != channel || session.previous.at("user_id") != projection.at("user_id") ||
        session.previous.at("session_id") != projection.at("session_id") ||
        session.previous.at("permissions") != projection.at("permissions")) {
      return {{}, 4403};
    }
    if (session.detached_at == core::WallTime{}) {
      return resync("INVALID_CURSOR");
    }
    // New access tokens from the same REST session may resume after refresh.
    session.token = token;
    session.failure = 0;
    update(session, current);
    if (session.failure) {
      return {{}, session.failure};
    }
    if (sequence < session.oldest) {
      return resync("CURSOR_EXPIRED");
    }
    if (sequence > session.sequence) {
      return resync("INVALID_CURSOR");
    }
    session.detached_at = {};
  }
  StreamReply reply{{{{"type", "SESSION_READY"}, {"session_id", id}, {"last_sequence", sequence}}},
                    0};
  const auto replay = poll(id, sequence);
  reply.frames.insert(reply.frames.end(), replay.frames.begin(), replay.frames.end());
  reply.close_code = replay.close_code;
  return reply;
} catch (...) {
  return {{}, 4400};
}
void EventStream::update(Session& session) {
  update(session, snapshot(session.token));
}
void EventStream::update(Session& session, const core::PortResult<Json>& current) {
  if (const auto* error = std::get_if<kiarabot::v1::Error>(&current)) {
    session.failure = close_code(*error);
    return;
  }
  const auto& projection = std::get<Json>(current);
  if (session.previous.at("permissions") != projection.at("permissions") ||
      !allowed(projection, session.channel)) {
    session.failure = 4403;
    return;
  }
  const auto now = clock_();
  for (const auto& [channel, event] : kEvents) {
    if ((session.channel != "control" && channel != session.channel) ||
        !allowed(projection, channel)) {
      continue;
    }
    const auto items = projection.value(channel, Json::array());
    const auto previous = session.previous.value(channel, Json::array());
    for (const auto& item : items) {
      if (std::find(previous.begin(), previous.end(), item) != previous.end()) {
        continue;
      }
      Json body = item;
      if (channel == "plugins") {
        body = {
            {"id", item.at("id")}, {"version", item.at("version")}, {"state", item.at("state")}};
      }
      session.events.push_back({now,
                                {{"schema_version", "kiarabot.stream.v1"},
                                 {"event_type", event},
                                 {"sequence", ++session.sequence},
                                 {"occurred_at", core::control_timestamp(now)},
                                 {"payload", body}}});
    }
  }
  session.previous = projection;
  while (!session.events.empty() &&
         (session.events.front().occurred + std::chrono::minutes(10) <= now ||
          session.events.size() > 4096)) {
    session.oldest = session.events.front().frame.at("sequence");
    session.events.pop_front();
  }
}
void EventStream::tick() {
  const auto now = clock_();
  std::erase_if(sessions_, [&](const auto& entry) {
    return entry.second.detached_at != core::WallTime{} &&
           entry.second.detached_at + std::chrono::minutes(10) <= now;
  });
  // One Core snapshot per access token per tick; detached sessions are refreshed by resume.
  std::unordered_map<std::string, core::PortResult<Json>> snapshots;
  for (auto& [id, session] : sessions_) {
    (void)id;
    if (session.failure || session.detached_at != core::WallTime{}) {
      continue;
    }
    const auto found = snapshots.find(session.token);
    if (found == snapshots.end()) {
      auto value = snapshot(session.token);
      const auto [position, inserted] = snapshots.emplace(session.token, std::move(value));
      (void)inserted;
      update(session, position->second);
    } else {
      update(session, found->second);
    }
  }
}
StreamReply EventStream::poll(const std::string& id, std::uint64_t sequence) {
  const auto found = sessions_.find(id);
  if (found == sessions_.end()) {
    return resync("CURSOR_EXPIRED");
  }
  const auto& session = found->second;
  if (session.failure) {
    return {{}, session.failure};
  }
  if (sequence < session.oldest) {
    return resync("CURSOR_EXPIRED");
  }
  StreamReply reply;
  for (const auto& event : session.events) {
    if (event.frame.at("sequence").get<std::uint64_t>() > sequence) {
      reply.frames.push_back(event.frame);
      if (reply.frames.size() == 32) {
        break;
      }
    }
  }
  return reply;
}
void EventStream::detach(const std::string& id) {
  if (sessions_.contains(id)) {
    sessions_.at(id).detached_at = clock_();
  }
}
} // namespace kiarabot::control
