#include "kiarabot/control/target/websocket_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::control::target {
namespace {
using Json = nlohmann::json;
namespace codec = core::platform::target_codec;

// Safe UTC bounds for every timestamp this transport renders. Owner storage rechecks its own
// transaction clock; these bounds only reject structurally impossible or overflowing values
// before they reach the browser.
constexpr std::int64_t kMinimumEpochSeconds = 946684800;  // 2000-01-01T00:00:00Z
constexpr std::int64_t kMaximumEpochSeconds = 7258118400; // 2200-01-01T00:00:00Z
constexpr std::size_t kMaximumIdentifierBytes = 128;
constexpr std::size_t kMaximumScopeResources = 127;

WebSocketFailure invalid() {
  return {v1::INVALID_ARGUMENT, false};
}
WebSocketFailure internal() {
  return {v1::INTERNAL, false};
}
bool printable(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool identifier(std::string_view value) {
  return printable(value, 1, kMaximumIdentifierBytes);
}
// Canonical decimal uint64: no sign, no leading zero except "0", at most 20 digits.
bool decimal_uint64(std::string_view value, std::uint64_t& output) {
  if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0') ||
      !printable(value, 1, 20)) {
    return false;
  }
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
// Positive decimal uint64 used by TargetPositiveUint64 ({version}).
bool positive_uint64(std::string_view value, std::uint64_t& output) {
  return decimal_uint64(value, output) && output != 0;
}
bool decimal_uint64_json(const Json& value, std::uint64_t& output) {
  return value.is_string() && decimal_uint64(value.get_ref<const std::string&>(), output);
}
bool timestamp(const google::protobuf::Timestamp& value) {
  if (value.nanos() < 0 || value.nanos() >= 1000000000 || value.seconds() < kMinimumEpochSeconds ||
      value.seconds() >= kMaximumEpochSeconds) {
    return false;
  }
  return std::holds_alternative<core::target::TimePoint>(codec::from_wire(value));
}
std::optional<std::string> timestamp_text(const google::protobuf::Timestamp& value) {
  if (!timestamp(value)) {
    return {};
  }
  const auto duration =
      std::get<core::target::TimePoint>(codec::from_wire(value)).time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto point = static_cast<std::time_t>(seconds.count());
  struct tm utc {};
  if (gmtime_r(&point, &utc) == nullptr) {
    return {};
  }
  char buffer[32]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc) == 0) {
    return {};
  }
  std::string result{buffer};
  if (value.nanos() != 0) {
    auto fraction = std::to_string(1000000000 + value.nanos()).substr(1);
    while (!fraction.empty() && fraction.back() == '0') {
      fraction.pop_back();
    }
    result += '.' + fraction;
  }
  return result + 'Z';
}
// Structural RFC3339 Z-form check used only to assert the emitted wire text.
bool timestamp_text(std::string_view value) {
  if (value.size() < 20 || value.back() != 'Z') {
    return false;
  }
  const auto digits = [&](std::size_t offset, std::size_t count) {
    if (offset + count > value.size() - 1) {
      return false;
    }
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(offset),
                       value.begin() + static_cast<std::ptrdiff_t>(offset + count),
                       [](char c) { return c >= '0' && c <= '9'; });
  };
  if (!digits(0, 4) || value[4] != '-' || !digits(5, 2) || value[7] != '-' || !digits(8, 2) ||
      value[10] != 'T' || !digits(11, 2) || value[13] != ':' || !digits(14, 2) ||
      value[16] != ':' || !digits(17, 2)) {
    return false;
  }
  std::size_t position = 19;
  if (position < value.size() - 1) {
    if (value[position] != '.') {
      return false;
    }
    ++position;
    const auto begin = position;
    while (position < value.size() - 1 && value[position] >= '0' && value[position] <= '9') {
      ++position;
    }
    if (position == begin || position - begin > 9 || position != value.size() - 1) {
      return false;
    }
  }
  const auto number = [&](std::size_t offset, std::size_t count) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < count; ++index) {
      result = result * 10 + static_cast<std::uint32_t>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = number(0, 4);
  return year >= 2000 && year <= 2199 && number(5, 2) >= 1 && number(5, 2) <= 12 &&
         number(8, 2) >= 1 && number(8, 2) <= 31 && number(11, 2) <= 23 && number(14, 2) <= 59 &&
         number(17, 2) <= 59;
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
  // Runtime exact operation resources follow RuntimeService.GetOperation scope kinds.
  return kind == v1::RESOURCE_ACCOUNT || kind == v1::RESOURCE_ADAPTER ||
         kind == v1::RESOURCE_INSTALLATION || kind == v1::RESOURCE_INSTANCE ||
         kind == v1::RESOURCE_HOST;
}
bool scope(const Json& input, const WebSocketSelection& route, v1::ResourceScope& output) {
  if (!input.is_object() || input.size() != 1 || !input.contains("resources") ||
      !input["resources"].is_array() || input["resources"].empty() ||
      input["resources"].size() > kMaximumScopeResources) {
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
    if (kind == v1::RESOURCE_CATALOG ? id != "control" : !identifier(id)) {
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
bool scope_json_valid(const Json& input) {
  if (!input.is_object() || input.size() != 1 || !input.contains("resources") ||
      !input["resources"].is_array()) {
    return false;
  }
  const auto& resources = input["resources"];
  if (resources.empty() || resources.size() > kMaximumScopeResources) {
    return false;
  }
  std::vector<std::pair<std::string, std::string>> seen;
  for (const auto& item : resources) {
    if (!item.is_object() || item.size() != 2 || !item.contains("kind") ||
        !item["kind"].is_string() || !item.contains("id") || !item["id"].is_string()) {
      return false;
    }
    v1::ResourceKind kind;
    if (!v1::ResourceKind_Parse(item["kind"].get<std::string>(), &kind)) {
      return false;
    }
    if (kind == v1::RESOURCE_KIND_UNSPECIFIED || kind == v1::RESOURCE_CATALOG ||
        !identifier(item["id"].get<std::string>())) {
      return false;
    }
    seen.emplace_back(item["kind"].get<std::string>(), item["id"].get<std::string>());
  }
  std::sort(seen.begin(), seen.end());
  return std::adjacent_find(seen.begin(), seen.end()) == seen.end();
}
std::optional<Json> cursor_json(const v1::StreamCursor& cursor) {
  WebSocketSelection route{"operations", cursor.source_service(), {}};
  v1::ResourceScope canonical;
  if ((route.owner != "runtime" && route.owner != "scheduler" && route.owner != "model" &&
       route.owner != "host") ||
      !identifier(cursor.epoch()) || !scope(scope_json(cursor.scope()), route, canonical) ||
      !websocket_same_scope(cursor.scope(), canonical)) {
    return {};
  }
  return Json{{"source_service", cursor.source_service()},
              {"epoch", cursor.epoch()},
              {"scope", scope_json(canonical)},
              {"sequence", std::to_string(cursor.sequence())}};
}
bool cursor_json_valid(const Json& input) {
  if (!input.is_object() || input.size() != 4 || !input.contains("source_service") ||
      !input["source_service"].is_string() || !input.contains("epoch") ||
      !input["epoch"].is_string() || !input.contains("scope") || !input.contains("sequence")) {
    return false;
  }
  const auto source = input["source_service"].get<std::string>();
  if (source != "runtime" && source != "scheduler" && source != "model" && source != "host") {
    return false;
  }
  if (!identifier(input["epoch"].get<std::string>()) || !scope_json_valid(input["scope"])) {
    return false;
  }
  std::uint64_t sequence = 0;
  return decimal_uint64_json(input["sequence"], sequence);
}
bool error_code(const std::string& name, v1::ErrorCode& output) {
  if (!v1::ErrorCode_Parse(name, &output)) {
    return false;
  }
  switch (output) {
  case v1::INVALID_ARGUMENT:
  case v1::UNAUTHENTICATED:
  case v1::PERMISSION_DENIED:
  case v1::NOT_FOUND:
  case v1::CONFLICT:
  case v1::CAPABILITY_NOT_SUPPORTED:
  case v1::RATE_LIMITED:
  case v1::TIMEOUT:
  case v1::UPSTREAM_UNAVAILABLE:
  case v1::PLUGIN_UNAVAILABLE:
  case v1::INTERNAL:
    return true;
  default:
    return false;
  }
}
// The frozen TargetError shape: stable code, approved closed-set message, correlation IDs only.
std::optional<Json> error(const v1::Error& value) {
  const auto code = codec::from_wire(value.code());
  if (!std::holds_alternative<core::target::ErrorCode>(code)) {
    return {};
  }
  if (!identifier(value.request_id()) || !identifier(value.trace_id())) {
    return {};
  }
  const auto stable = codec::make_error(std::get<core::target::ErrorCode>(code), value.request_id(),
                                        value.trace_id(), value.retryable());
  if (!std::holds_alternative<v1::Error>(stable)) {
    return {};
  }
  // Re-render from the stable enum; never forward upstream arbitrary text/details.
  const auto& safe = std::get<v1::Error>(stable);
  if (safe.details().size() != 0) {
    return {};
  }
  return Json{{"code", v1::ErrorCode_Name(safe.code())}, {"message", safe.message()},
              {"request_id", safe.request_id()},         {"trace_id", safe.trace_id()},
              {"retryable", safe.retryable()},           {"details", Json::object()}};
}
bool error_json_valid(const Json& input) {
  if (!input.is_object() || input.size() != 6 || !input.contains("code") ||
      !input["code"].is_string() || !input.contains("message") || !input["message"].is_string() ||
      !input.contains("request_id") || !input["request_id"].is_string() ||
      !input.contains("trace_id") || !input["trace_id"].is_string() ||
      !input.contains("retryable") || !input["retryable"].is_boolean() ||
      !input.contains("details") || !input["details"].is_object()) {
    return false;
  }
  v1::ErrorCode parsed;
  if (!error_code(input["code"].get<std::string>(), parsed)) {
    return false;
  }
  const auto code = codec::from_wire(parsed);
  if (!std::holds_alternative<core::target::ErrorCode>(code)) {
    return false;
  }
  const auto stable = codec::make_error(std::get<core::target::ErrorCode>(code),
                                        input["request_id"].get<std::string>(),
                                        input["trace_id"].get<std::string>(), false);
  if (!std::holds_alternative<v1::Error>(stable)) {
    return false;
  }
  if (input["message"].get<std::string>() != std::get<v1::Error>(stable).message()) {
    return false;
  }
  if (!identifier(input["request_id"].get<std::string>()) ||
      !identifier(input["trace_id"].get<std::string>())) {
    return false;
  }
  return std::all_of(input["details"].begin(), input["details"].end(),
                     [](const auto& item) { return item.is_string(); });
}
bool operation_state(const std::string& name, v1::OperationState& output) {
  return v1::OperationState_Parse(name, &output) && output >= v1::OPERATION_ACCEPTED &&
         output <= v1::OPERATION_EXPIRED;
}
bool execution_outcome(const std::string& name, v1::ExecutionOutcome& output) {
  return v1::ExecutionOutcome_Parse(name, &output) && output >= v1::EXECUTION_NOT_STARTED &&
         output <= v1::EXECUTION_UNKNOWN;
}
bool payload_json_valid(const Json& payload) {
  static const std::set<std::string> kPayloadKeys{
      "ref",    "version",         "state",  "execution_outcome", "participants", "observed_at",
      "reason", "next_attempt_at", "stalled"};
  if (!payload.is_object() || !payload.contains("ref") || !payload.contains("version") ||
      !payload.contains("state") || !payload.contains("execution_outcome") ||
      !payload.contains("observed_at") || !payload.contains("stalled")) {
    return false;
  }
  for (const auto& item : payload.items()) {
    if (!kPayloadKeys.contains(item.key())) {
      return false;
    }
  }
  const auto& ref = payload["ref"];
  if (!ref.is_object() || ref.size() != 3 || !ref.contains("owner_service") ||
      !ref["owner_service"].is_string() || !ref.contains("operation_id") ||
      !ref["operation_id"].is_string() || !ref.contains("resource") ||
      !identifier(ref["owner_service"].get<std::string>()) ||
      !identifier(ref["operation_id"].get<std::string>())) {
    return false;
  }
  const auto& resource = ref["resource"];
  if (!resource.is_object() || resource.size() != 2 || !resource.contains("kind") ||
      !resource["kind"].is_string() || !resource.contains("id") || !resource["id"].is_string()) {
    return false;
  }
  v1::ResourceKind kind;
  if (!v1::ResourceKind_Parse(resource["kind"].get<std::string>(), &kind) ||
      kind == v1::RESOURCE_KIND_UNSPECIFIED || !identifier(resource["id"].get<std::string>())) {
    return false;
  }
  std::uint64_t version = 0;
  if (!positive_uint64(payload["version"].get<std::string>(), version)) {
    return false;
  }
  v1::OperationState state;
  if (!payload["state"].is_string() ||
      !operation_state(payload["state"].get<std::string>(), state)) {
    return false;
  }
  v1::ExecutionOutcome outcome;
  if (!payload["execution_outcome"].is_string() ||
      !execution_outcome(payload["execution_outcome"].get<std::string>(), outcome)) {
    return false;
  }
  if (!payload["observed_at"].is_string() ||
      !timestamp_text(payload["observed_at"].get<std::string>())) {
    return false;
  }
  if (!payload["stalled"].is_boolean()) {
    return false;
  }
  const bool blocked = state == v1::OPERATION_BLOCKED;
  if (blocked && (!payload.contains("reason") || !payload.contains("next_attempt_at"))) {
    return false;
  }
  if (payload.contains("reason") &&
      (!payload["reason"].is_object() || !error_json_valid(payload["reason"]))) {
    return false;
  }
  if (payload.contains("next_attempt_at") &&
      (!payload["next_attempt_at"].is_string() ||
       !timestamp_text(payload["next_attempt_at"].get<std::string>()))) {
    return false;
  }
  if (!payload.contains("participants") || !payload["participants"].is_array() ||
      payload["participants"].size() > 16) {
    return false;
  }
  std::set<std::string> participants;
  for (const auto& item : payload["participants"]) {
    if (!item.is_object() || !item.contains("participant") || !item["participant"].is_string() ||
        !item.contains("state") || !item["state"].is_string() ||
        !identifier(item["participant"].get<std::string>())) {
      return false;
    }
    v1::OperationState member_state;
    if (!operation_state(item["state"].get<std::string>(), member_state) ||
        !participants.insert(item["participant"].get<std::string>()).second) {
      return false;
    }
    for (const auto& field : item.items()) {
      if (field.key() != "participant" && field.key() != "state" && field.key() != "reason" &&
          field.key() != "next_attempt_at") {
        return false;
      }
    }
    if (item.contains("reason") && !error_json_valid(item["reason"])) {
      return false;
    }
    if (item.contains("next_attempt_at") &&
        (!item["next_attempt_at"].is_string() ||
         !timestamp_text(item["next_attempt_at"].get<std::string>()))) {
      return false;
    }
  }
  return true;
}
Json operation_payload(const v1::Operation& input, const v1::StreamCursor& cursor) {
  const auto& resource = input.ref().resource();
  Json payload{
      {"ref",
       {{"owner_service", input.ref().owner_service()},
        {"operation_id", input.ref().operation_id()},
        {"resource", {{"kind", v1::ResourceKind_Name(resource.kind())}, {"id", resource.id()}}}}},
      {"version", std::to_string(input.version())},
      {"state", v1::OperationState_Name(input.state())},
      {"execution_outcome", v1::ExecutionOutcome_Name(input.execution_outcome())},
      {"observed_at", *timestamp_text(input.observed_at())},
      {"stalled", input.stalled()},
      {"participants", Json::array()}};
  static_cast<void>(cursor);
  if (input.has_reason()) {
    payload["reason"] = *error(input.reason());
  }
  if (input.has_next_attempt_at()) {
    payload["next_attempt_at"] = *timestamp_text(input.next_attempt_at());
  }
  for (const auto& member : input.participants()) {
    Json item{{"participant", member.participant()},
              {"state", v1::OperationState_Name(member.state())}};
    if (member.has_reason()) {
      item["reason"] = *error(member.reason());
    }
    if (member.has_next_attempt_at()) {
      item["next_attempt_at"] = *timestamp_text(member.next_attempt_at());
    }
    payload["participants"].push_back(std::move(item));
  }
  return payload;
}
} // namespace

WebSocketResult<WebSocketSelection> websocket_route(std::string_view target) {
  WebSocketSelection result;
  if (!target.starts_with("/ws/") || target.size() > 256 ||
      target.find('#') != std::string_view::npos) {
    return invalid();
  }
  const auto separator = target.find('?');
  if (separator != std::string_view::npos &&
      target.find('?', separator + 1) != std::string_view::npos) {
    return invalid();
  }
  result.channel =
      target.substr(4, separator == std::string_view::npos ? target.size() - 4 : separator - 4);
  if (result.channel.empty() ||
      !std::all_of(result.channel.begin(), result.channel.end(),
                   [](unsigned char c) { return c >= 'a' && c <= 'z'; })) {
    return WebSocketFailure{v1::NOT_FOUND, false};
  }
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
    if (text.empty() || text.size() > kMaximumFirstFrameBytes) {
      return invalid();
    }
    // Reject duplicate JSON keys instead of accepting last-key-wins token/scope ambiguity, and
    // bound nesting so a hostile first frame cannot drive parser recursion before authentication.
    std::vector<std::set<std::string>> keys;
    std::size_t depth = 0;
    bool duplicate = false;
    bool too_deep = false;
    const auto input = Json::parse(
        text,
        [&](int, Json::parse_event_t event, Json& parsed) {
          switch (event) {
          case Json::parse_event_t::object_start:
          case Json::parse_event_t::array_start:
            if (depth >= kMaximumJsonDepth) {
              too_deep = true;
              return false;
            }
            ++depth;
            if (event == Json::parse_event_t::object_start) {
              keys.emplace_back();
            }
            break;
          case Json::parse_event_t::object_end:
            if (depth == 0 || keys.empty()) {
              return false;
            }
            --depth;
            keys.pop_back();
            break;
          case Json::parse_event_t::array_end:
            if (depth == 0) {
              return false;
            }
            --depth;
            break;
          case Json::parse_event_t::key:
            if (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second) {
              duplicate = true;
              return false;
            }
            break;
          default:
            break;
          }
          return true;
        },
        false);
    if (duplicate || too_deep || depth != 0 || !input.is_object() || !input.contains("type") ||
        !input["type"].is_string() || !input.contains("access_token") ||
        !input["access_token"].is_string() || !input.contains("last_sequence") ||
        !input["last_sequence"].is_number_unsigned() || !input.contains("scope")) {
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
    if (!printable(result.access_token, 20, 4096) || result.last_sequence > kMaximumSafeSequence ||
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
      if (!printable(result.session_id, 32, 256)) {
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
        std::uint64_t sequence = 0;
        cursor.set_source_service(route.owner);
        cursor.set_epoch(selected["epoch"].get<std::string>());
        if (!identifier(cursor.epoch()) ||
            !scope(selected["scope"], route, *cursor.mutable_scope()) ||
            !websocket_same_scope(cursor.scope(), result.scope) ||
            !decimal_uint64_json(selected["sequence"], sequence)) {
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
    return allow_missing && !allowed.empty();
  }
  return origin.starts_with("https://") && origin.size() <= 512 &&
         std::all_of(origin.begin(), origin.end(),
                     [](unsigned char c) { return c >= 33 && c <= 126; }) &&
         std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
}

bool websocket_same_scope(const v1::ResourceScope& left, const v1::ResourceScope& right) {
  if (left.resources_size() != right.resources_size()) {
    return false;
  }
  for (int index = 0; index < left.resources_size(); ++index) {
    if (left.resources(index).kind() != right.resources(index).kind() ||
        left.resources(index).id() != right.resources(index).id()) {
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
                : WebSocketResult<std::string>{internal()};
}

WebSocketResult<std::string> websocket_operation_event(const v1::OperationChange& change,
                                                       std::uint64_t sequence) {
  try {
    if (sequence == 0 || sequence > kMaximumSafeSequence || !change.has_operation() ||
        !change.has_cursor()) {
      return internal();
    }
    const auto& input = change.operation();
    const auto& cursor = change.cursor();
    const auto selected = cursor_json(cursor);
    if (!selected || !timestamp(input.observed_at()) || !input.has_ref() ||
        !input.ref().has_resource() || input.ref().owner_service() != cursor.source_service() ||
        !identifier(input.ref().operation_id()) || !identifier(input.ref().resource().id()) ||
        input.ref().resource().kind() == v1::RESOURCE_KIND_UNSPECIFIED || input.version() == 0 ||
        input.state() < v1::OPERATION_ACCEPTED || input.state() > v1::OPERATION_EXPIRED ||
        input.execution_outcome() < v1::EXECUTION_NOT_STARTED ||
        input.execution_outcome() > v1::EXECUTION_UNKNOWN || input.participants_size() > 16) {
      return internal();
    }
    const auto& resource = input.ref().resource();
    const bool selected_resource =
        std::any_of(cursor.scope().resources().begin(), cursor.scope().resources().end(),
                    [&](const auto& item) {
                      return item.kind() == resource.kind() && item.id() == resource.id();
                    });
    if (!selected_resource) {
      return internal();
    }
    if (input.has_reason() && !error(input.reason())) {
      return internal();
    }
    if (input.has_next_attempt_at() && !timestamp(input.next_attempt_at())) {
      return internal();
    }
    if (input.state() == v1::OPERATION_BLOCKED &&
        (!input.has_reason() || !input.has_next_attempt_at())) {
      return internal();
    }
    std::set<std::string> participants;
    for (const auto& member : input.participants()) {
      if (!printable(member.participant(), 1, kMaximumIdentifierBytes) ||
          !participants.insert(member.participant()).second ||
          member.state() < v1::OPERATION_ACCEPTED || member.state() > v1::OPERATION_EXPIRED ||
          (member.has_reason() && !error(member.reason())) ||
          (member.has_next_attempt_at() && !timestamp(member.next_attempt_at()))) {
        return internal();
      }
    }
    const auto observed = timestamp_text(input.observed_at());
    if (!observed) {
      return internal();
    }
    const auto result = Json{
        {"schema_version", "kiarabot.stream.v1"},
        {"event_type", "operation.updated"},
        {"sequence", sequence},
        {"occurred_at", *observed},
        {"payload", operation_payload(input, cursor)},
        {"owner_cursor",
         *selected}}.dump();
    if (result.size() > kMaximumEventBytes || !websocket_valid_event(result)) {
      return internal();
    }
    return result;
  } catch (...) {
    return internal();
  }
}

WebSocketResult<std::string>
websocket_session_ready(std::string_view session_id, std::uint64_t last_sequence,
                        const std::optional<v1::StreamCursor>& owner_cursor) {
  try {
    if (!printable(session_id, 32, 256) || last_sequence > kMaximumSafeSequence) {
      return internal();
    }
    Json frame{{"type", "SESSION_READY"},
               {"session_id", std::string{session_id}},
               {"last_sequence", last_sequence}};
    if (owner_cursor) {
      const auto cursor = cursor_json(*owner_cursor);
      if (!cursor) {
        return internal();
      }
      frame["owner_cursor"] = *cursor;
    }
    return frame.dump();
  } catch (...) {
    return internal();
  }
}

WebSocketResult<std::string> websocket_resync_required(WebSocketResyncReason reason) {
  try {
    const char* name = nullptr;
    switch (reason) {
    case WebSocketResyncReason::CursorExpired:
      name = "CURSOR_EXPIRED";
      break;
    case WebSocketResyncReason::InvalidCursor:
      name = "INVALID_CURSOR";
      break;
    case WebSocketResyncReason::ServerRestart:
      name = "SERVER_RESTART";
      break;
    }
    if (name == nullptr) {
      return internal();
    }
    return Json{{"type", "RESYNC_REQUIRED"}, {"reason", name}}.dump();
  } catch (...) {
    return internal();
  }
}

bool websocket_valid_event(std::string_view json) {
  try {
    if (json.empty() || json.size() > kMaximumEventBytes) {
      return false;
    }
    const auto input = Json::parse(json, nullptr, false);
    if (input.is_discarded() || !input.is_object() || input.size() != 6) {
      return false;
    }
    static const std::set<std::string> kEnvelopeKeys{
        "schema_version", "event_type", "sequence", "occurred_at", "payload", "owner_cursor"};
    for (const auto& item : input.items()) {
      if (!kEnvelopeKeys.contains(item.key())) {
        return false;
      }
    }
    if (!input.contains("schema_version") || !input["schema_version"].is_string() ||
        input["schema_version"].get<std::string>() != "kiarabot.stream.v1") {
      return false;
    }
    if (!input.contains("event_type") || !input["event_type"].is_string() ||
        input["event_type"].get<std::string>() != "operation.updated") {
      return false;
    }
    if (!input.contains("sequence") || !input["sequence"].is_number_unsigned()) {
      return false;
    }
    const auto sequence = input["sequence"].get<std::uint64_t>();
    if (sequence == 0 || sequence > kMaximumSafeSequence) {
      return false;
    }
    if (!input.contains("occurred_at") || !input["occurred_at"].is_string() ||
        !timestamp_text(input["occurred_at"].get<std::string>())) {
      return false;
    }
    if (!input.contains("payload") || !payload_json_valid(input["payload"])) {
      return false;
    }
    return input.contains("owner_cursor") && cursor_json_valid(input["owner_cursor"]);
  } catch (...) {
    return false;
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
