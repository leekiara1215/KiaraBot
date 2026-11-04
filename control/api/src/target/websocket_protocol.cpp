#include "kiarabot/control/target/websocket_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::control::target {
namespace {
using Json = nlohmann::json;
namespace codec = core::platform::target_codec;
constexpr std::uint64_t kMaximumSequence = 9007199254740991ULL;
WebSocketFailure invalid() {
  return {v1::INVALID_ARGUMENT, false};
}
bool uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') {
        return false;
      }
    } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}
bool clean(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return c >= 32 && c != 127; });
}
bool selected_kind(const WebSocketSelection& route, v1::ResourceKind kind) {
  if (route.channel == "health" || route.channel == "logs") {
    return kind == v1::RESOURCE_CATALOG;
  }
  if (route.channel == "adapters") {
    return kind == v1::RESOURCE_ADAPTER;
  }
  if (route.channel == "plugins") {
    return kind == v1::RESOURCE_INSTANCE;
  }
  if (route.channel == "tasks") {
    return kind == v1::RESOURCE_TASK || kind == v1::RESOURCE_INSTANCE;
  }
  if (route.channel == "messages") {
    return kind == v1::RESOURCE_ACCOUNT;
  }
  if (route.channel == "control") {
    return kind != v1::RESOURCE_KIND_UNSPECIFIED && kind != v1::RESOURCE_CATALOG;
  }
  if (route.owner == "host") {
    return kind == v1::RESOURCE_HOST;
  }
  if (route.owner == "model") {
    return kind == v1::RESOURCE_MODEL;
  }
  if (route.owner == "scheduler") {
    return kind == v1::RESOURCE_TASK || kind == v1::RESOURCE_INSTANCE;
  }
  return kind == v1::RESOURCE_ACCOUNT || kind == v1::RESOURCE_ADAPTER ||
         kind == v1::RESOURCE_INSTALLATION || kind == v1::RESOURCE_INSTANCE ||
         kind == v1::RESOURCE_HOST;
}
bool scope(const Json& input, const WebSocketSelection& route, v1::ResourceScope& output) {
  if (!input.is_object() || input.size() != 1 || !input.contains("resources") ||
      !input["resources"].is_array() || input["resources"].empty() ||
      input["resources"].size() > 127) {
    return false;
  }
  std::vector<std::pair<v1::ResourceKind, std::string>> resources;
  for (const auto& item : input["resources"]) {
    v1::ResourceKind kind;
    if (!item.is_object() || item.size() != 2 || !item.contains("kind") ||
        !item["kind"].is_string() || !item.contains("id") || !item["id"].is_string() ||
        !v1::ResourceKind_Parse(item["kind"].get<std::string>(), &kind) ||
        !selected_kind(route, kind)) {
      return false;
    }
    const auto id = item["id"].get<std::string>();
    if (kind == v1::RESOURCE_CATALOG ? id != "control" : !uuid(id)) {
      return false;
    }
    resources.emplace_back(kind, id);
  }
  std::sort(resources.begin(), resources.end());
  if (std::adjacent_find(resources.begin(), resources.end()) != resources.end() ||
      ((route.owner == "host" || route.channel == "health" || route.channel == "logs") &&
       resources.size() != 1)) {
    return false;
  }
  for (const auto& [kind, id] : resources) {
    auto* resource = output.add_resources();
    resource->set_kind(kind);
    resource->set_id(id);
  }
  return true;
}
Json scope_json(const v1::ResourceScope& scope) {
  Json resources = Json::array();
  for (const auto& item : scope.resources()) {
    resources.push_back({{"kind", v1::ResourceKind_Name(item.kind())}, {"id", item.id()}});
  }
  return {{"resources", resources}};
}
bool uint64(const Json& input, std::uint64_t& output) {
  if (!input.is_string()) {
    return false;
  }
  const auto value = input.get<std::string>();
  if (value.empty() || value.size() > 20 || (value.size() > 1 && value[0] == '0')) {
    return false;
  }
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
std::optional<Json> cursor_json(const v1::StreamCursor& cursor) {
  WebSocketSelection route{"operations", cursor.source_service(), {}};
  v1::ResourceScope canonical;
  if ((route.owner != "runtime" && route.owner != "scheduler" && route.owner != "model" &&
       route.owner != "host") ||
      !clean(cursor.epoch(), 1, 128) || !scope(scope_json(cursor.scope()), route, canonical) ||
      !websocket_same_scope(cursor.scope(), canonical)) {
    return {};
  }
  return Json{{"source_service", cursor.source_service()},
              {"epoch", cursor.epoch()},
              {"scope", scope_json(canonical)},
              {"sequence", std::to_string(cursor.sequence())}};
}
std::optional<std::string> timestamp(const google::protobuf::Timestamp& value) {
  const auto decoded = codec::from_wire(value);
  if (!std::holds_alternative<core::target::TimePoint>(decoded)) {
    return {};
  }
  const auto duration = std::get<core::target::TimePoint>(decoded).time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto point = static_cast<std::time_t>(seconds.count());
  struct tm utc {};
  if (!gmtime_r(&point, &utc)) {
    return {};
  }
  char buffer[32]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
    return {};
  }
  std::string result = buffer;
  if (value.nanos() != 0) {
    auto fraction = std::to_string(1000000000 + value.nanos()).substr(1);
    while (!fraction.empty() && fraction.back() == '0') {
      fraction.pop_back();
    }
    result += '.' + fraction;
  }
  return result + 'Z';
}
std::optional<Json> error(const v1::Error& value) {
  const auto code = codec::from_wire(value.code());
  if (!std::holds_alternative<core::target::ErrorCode>(code)) {
    return {};
  }
  const auto stable = codec::make_error(std::get<core::target::ErrorCode>(code), value.request_id(),
                                        value.trace_id(), value.retryable());
  if (!std::holds_alternative<v1::Error>(stable)) {
    return {};
  }
  // Re-render from the stable enum; never forward upstream arbitrary text/details.
  const auto& safe = std::get<v1::Error>(stable);
  return Json{{"code", v1::ErrorCode_Name(safe.code())}, {"message", safe.message()},
              {"request_id", safe.request_id()},         {"trace_id", safe.trace_id()},
              {"retryable", safe.retryable()},           {"details", Json::object()}};
}
} // namespace

WebSocketResult<WebSocketSelection> websocket_route(std::string_view target) {
  WebSocketSelection result;
  if (!target.starts_with("/ws/") || target.size() > 256) {
    return invalid();
  }
  const auto separator = target.find('?');
  result.channel =
      target.substr(4, separator == std::string_view::npos ? target.size() - 4 : separator - 4);
  if (result.channel == "operations" || result.channel == "control" || result.channel == "logs") {
    constexpr std::string_view prefix = "owner_service=";
    const auto query =
        separator == std::string_view::npos ? std::string_view{} : target.substr(separator + 1);
    if (!query.starts_with(prefix)) {
      return invalid();
    }
    result.owner = query.substr(prefix.size());
    const bool allowed = result.channel == "operations"
                             ? (result.owner == "runtime" || result.owner == "scheduler" ||
                                result.owner == "model" || result.owner == "host")
                         : result.channel == "logs"
                             ? (result.owner == "identity" || result.owner == "runtime")
                             : (result.owner == "control" || result.owner == "runtime" ||
                                result.owner == "scheduler" || result.owner == "identity");
    if (!allowed) {
      return invalid();
    }
  } else {
    if (separator != std::string_view::npos) {
      return invalid();
    }
    if (result.channel == "health") {
      result.owner = "control";
    } else if (result.channel == "tasks") {
      result.owner = "scheduler";
    } else if (result.channel == "adapters" || result.channel == "plugins" ||
               result.channel == "messages") {
      result.owner = "runtime";
    } else {
      return WebSocketFailure{v1::NOT_FOUND, false};
    }
  }
  return result;
}
WebSocketResult<WebSocketFirstFrame> websocket_first_frame(std::string_view text,
                                                           const WebSocketSelection& route) {
  try {
    if (text.empty() || text.size() > 65536) {
      return invalid();
    }
    // Reject duplicate JSON keys instead of accepting last-key-wins token/scope ambiguity.
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    auto input = Json::parse(
        text,
        [&](int, Json::parse_event_t event, Json& parsed) {
          if (event == Json::parse_event_t::object_start) {
            keys.emplace_back();
          }
          if (event == Json::parse_event_t::key &&
              !keys.back().insert(parsed.get<std::string>()).second) {
            duplicate = true;
          }
          if (event == Json::parse_event_t::object_end) {
            keys.pop_back();
          }
          return true;
        },
        false);
    if (duplicate || !input.is_object() || !input.contains("type") || !input["type"].is_string() ||
        !input.contains("access_token") || !input["access_token"].is_string() ||
        !input.contains("last_sequence") || !input["last_sequence"].is_number_unsigned() ||
        !input.contains("scope")) {
      return invalid();
    }
    WebSocketFirstFrame result;
    const auto type = input["type"].get<std::string>();
    if (type != "hello" && type != "resume") {
      return invalid();
    }
    result.resume = type == "resume";
    result.access_token = input["access_token"].get<std::string>();
    result.last_sequence = input["last_sequence"].get<std::uint64_t>();
    if (!clean(result.access_token, 20, 4096) || result.last_sequence > kMaximumSequence ||
        !scope(input["scope"], route, result.scope)) {
      return invalid();
    }
    if (!result.resume) {
      if (result.last_sequence != 0 || input.contains("session_id") ||
          input.contains("owner_cursor")) {
        return invalid();
      }
    } else {
      if (!input.contains("session_id") || !input["session_id"].is_string()) {
        return invalid();
      }
      result.session_id = input["session_id"].get<std::string>();
      if (!clean(result.session_id, 32, 256)) {
        return invalid();
      }
      if (route.channel == "operations") {
        if (!input.contains("owner_cursor") || !input["owner_cursor"].is_object()) {
          return invalid();
        }
        const auto& selected = input["owner_cursor"];
        v1::StreamCursor cursor;
        if (selected.size() != 4 || !selected.contains("source_service") ||
            !selected["source_service"].is_string() ||
            selected["source_service"].get<std::string>() != route.owner ||
            !selected.contains("epoch") || !selected["epoch"].is_string() ||
            !selected.contains("scope") || !selected.contains("sequence")) {
          return invalid();
        }
        std::uint64_t sequence;
        cursor.set_source_service(route.owner);
        cursor.set_epoch(selected["epoch"].get<std::string>());
        if (!clean(cursor.epoch(), 1, 128) ||
            !scope(selected["scope"], route, *cursor.mutable_scope()) ||
            !websocket_same_scope(cursor.scope(), result.scope) ||
            !uint64(selected["sequence"], sequence)) {
          return invalid();
        }
        cursor.set_sequence(sequence);
        result.owner_cursor = std::move(cursor);
      } else if (input.contains("owner_cursor")) {
        return invalid();
      }
    }
    return result;
  } catch (...) {
    return invalid();
  }
}
bool websocket_origin(std::string_view origin, bool present,
                      const std::vector<std::string>& allowed, bool allow_missing) {
  if (!present) {
    return allow_missing;
  }
  return origin.starts_with("https://") && origin.size() <= 512 &&
         std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
}
bool websocket_same_scope(const v1::ResourceScope& left, const v1::ResourceScope& right) {
  if (left.resources_size() != right.resources_size()) {
    return false;
  }
  for (int i = 0; i < left.resources_size(); ++i) {
    if (left.resources(i).kind() != right.resources(i).kind() ||
        left.resources(i).id() != right.resources(i).id()) {
      return false;
    }
  }
  return true;
}
bool websocket_same_cursor(const v1::StreamCursor& left, const v1::StreamCursor& right) {
  return left.source_service() == right.source_service() && left.epoch() == right.epoch() &&
         left.sequence() == right.sequence() && websocket_same_scope(left.scope(), right.scope());
}
WebSocketResult<std::string> websocket_cursor_json(const v1::StreamCursor& cursor) {
  const auto result = cursor_json(cursor);
  return result ? WebSocketResult<std::string>{result->dump()}
                : WebSocketResult<std::string>{WebSocketFailure{v1::INTERNAL, false}};
}
WebSocketResult<std::string> websocket_operation_event(const v1::OperationChange& change,
                                                       std::uint64_t sequence) {
  try {
    if (sequence == 0 || sequence > kMaximumSequence || !change.has_operation() ||
        !change.has_cursor()) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
    const auto& input = change.operation();
    const auto selected = cursor_json(change.cursor());
    const auto observed = timestamp(input.observed_at());
    if (!selected || !observed || !input.has_ref() || !uuid(input.ref().operation_id()) ||
        input.ref().owner_service() != change.cursor().source_service() ||
        !input.ref().has_resource() || input.version() == 0 ||
        input.state() < v1::OPERATION_ACCEPTED || input.state() > v1::OPERATION_EXPIRED ||
        input.execution_outcome() < v1::EXECUTION_NOT_STARTED ||
        input.execution_outcome() > v1::EXECUTION_UNKNOWN || input.participants_size() > 16) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
    const auto& resource = input.ref().resource();
    if (!std::any_of(change.cursor().scope().resources().begin(),
                     change.cursor().scope().resources().end(), [&](const auto& item) {
                       return item.kind() == resource.kind() && item.id() == resource.id();
                     })) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
    Json payload{
        {"ref",
         {{"owner_service", input.ref().owner_service()},
          {"operation_id", input.ref().operation_id()},
          {"resource", {{"kind", v1::ResourceKind_Name(resource.kind())}, {"id", resource.id()}}}}},
        {"version", std::to_string(input.version())},
        {"state", v1::OperationState_Name(input.state())},
        {"execution_outcome", v1::ExecutionOutcome_Name(input.execution_outcome())},
        {"observed_at", *observed},
        {"stalled", input.stalled()},
        {"participants", Json::array()}};
    if (input.has_reason()) {
      const auto stable = error(input.reason());
      if (!stable) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      payload["reason"] = *stable;
    }
    if (input.has_next_attempt_at()) {
      const auto time = timestamp(input.next_attempt_at());
      if (!time) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      payload["next_attempt_at"] = *time;
    }
    if (input.state() == v1::OPERATION_BLOCKED &&
        (!input.has_reason() || !input.has_next_attempt_at())) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
    std::set<std::string> participants;
    for (const auto& member : input.participants()) {
      if (!clean(member.participant(), 1, 128) ||
          !participants.insert(member.participant()).second ||
          member.state() < v1::OPERATION_ACCEPTED || member.state() > v1::OPERATION_EXPIRED) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      Json item{{"participant", member.participant()},
                {"state", v1::OperationState_Name(member.state())}};
      if (member.has_reason()) {
        const auto stable = error(member.reason());
        if (!stable) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        item["reason"] = {{"code", (*stable)["code"]}};
      }
      if (member.has_next_attempt_at()) {
        const auto time = timestamp(member.next_attempt_at());
        if (!time) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        item["next_attempt_at"] = *time;
      }
      payload["participants"].push_back(std::move(item));
    }
    const auto result = Json{
        {"schema_version", "kiarabot.stream.v1"},
        {"event_type", "operation.updated"},
        {"sequence", sequence},
        {"occurred_at", *observed},
        {"payload", std::move(payload)},
        {"owner_cursor",
         *selected}}.dump();
    if (result.size() > 65536) {
      return WebSocketFailure{v1::RATE_LIMITED, false};
    }
    return result;
  } catch (...) {
    return WebSocketFailure{v1::INTERNAL, false};
  }
}
unsigned short websocket_close_code(WebSocketFailure failure) {
  switch (failure.code) {
  case v1::INVALID_ARGUMENT:
    return 4400;
  case v1::UNAUTHENTICATED:
    return 4401;
  case v1::PERMISSION_DENIED:
    return 4403;
  case v1::RATE_LIMITED:
    return 1013;
  default:
    return 1011;
  }
}
} // namespace kiarabot::control::target
