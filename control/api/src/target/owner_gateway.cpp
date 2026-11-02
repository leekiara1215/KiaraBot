#include "kiarabot/control/target/owner_gateway.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>
#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/actions.pb.h"

namespace kiarabot::control::target {
namespace {
namespace wire = kiarabot::v1;
namespace codec = core::platform::target_codec;
using Json = nlohmann::json;

std::string random_identifier() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return "control-request";
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string value;
  value.reserve(32);
  for (const auto byte : bytes) {
    value.push_back(kHex[byte >> 4U]);
    value.push_back(kHex[byte & 0x0fU]);
  }
  return value;
}
std::optional<std::string> header(const OwnerHttpRequest& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (key.size() == name.size() &&
        std::equal(key.begin(), key.end(), name.begin(), [](char left, char right) {
          const auto lower = [](char value) {
            return static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value);
          };
          return lower(left) == lower(right);
        })) {
      return value;
    }
  }
  return std::nullopt;
}
std::optional<std::string> percent_decode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '%') {
      if (index + 2 >= value.size()) {
        return std::nullopt;
      }
      unsigned number = 0;
      const auto parsed =
          std::from_chars(value.data() + index + 1, value.data() + index + 3, number, 16);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + index + 3 || number < 32 ||
          number >= 127) {
        return std::nullopt;
      }
      result.push_back(static_cast<char>(number));
      index += 2;
    } else {
      result.push_back(value[index]);
    }
  }
  return result;
}
std::optional<std::map<std::string, std::string>> parse_query(std::string_view target) {
  std::map<std::string, std::string> query;
  const auto separator = target.find('?');
  if (separator == std::string_view::npos) {
    return query;
  }
  auto rest = target.substr(separator + 1);
  while (!rest.empty()) {
    const auto end = rest.find('&');
    const auto part = rest.substr(0, end);
    const auto equal = part.find('=');
    if (equal == std::string_view::npos) {
      return std::nullopt;
    }
    const auto name = percent_decode(part.substr(0, equal));
    const auto value = percent_decode(part.substr(equal + 1));
    if (!name || !value || !query.emplace(*name, *value).second) {
      return std::nullopt;
    }
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
  }
  return query;
}
std::optional<wire::ResourceScope> parse_scope(const std::map<std::string, std::string>& query) {
  const auto found = query.find("scope");
  if (found == query.end()) {
    return std::nullopt;
  }
  wire::ResourceScope scope;
  if (!google::protobuf::util::JsonStringToMessage(found->second, &scope).ok() ||
      scope.resources_size() != 1) {
    return std::nullopt;
  }
  return scope;
}
std::string json_time(const google::protobuf::Timestamp& value) {
  auto result = google::protobuf::util::TimeUtil::ToString(value);
  if (!result.empty() && result.back() != 'Z' && result.find('+') == std::string::npos) {
    result += 'Z';
  }
  return result;
}
Json error_json(wire::ErrorCode code, std::string_view request_id, std::string_view trace_id,
                bool retryable) {
  return {{"code", wire::ErrorCode_Name(code)},
          {"message", wire::ErrorCode_Name(code)},
          {"request_id", std::string{request_id}},
          {"trace_id", std::string{trace_id}},
          {"retryable", retryable},
          {"details", Json::object()}};
}
Json action_result_json(const wire::ActionResult& result) {
  Json value{{"action_id", result.action_id()}};
  value["status"] = wire::ActionStatus_Name(result.status());
  value["execution_outcome"] = wire::ExecutionOutcome_Name(result.execution_outcome());
  if (!result.platform_message_id().empty()) {
    value["platform_message_id"] = result.platform_message_id();
  }
  if (result.has_error()) {
    value["error"] = {{"code", wire::ErrorCode_Name(result.error().code())},
                      {"message", result.error().message()},
                      {"request_id", result.error().request_id()},
                      {"trace_id", result.error().trace_id()},
                      {"retryable", result.error().retryable()},
                      {"details", Json::object()}};
  }
  if (result.has_completed_at()) {
    value["completed_at"] = json_time(result.completed_at());
  }
  if (!result.dispatch_attempt_id().empty()) {
    value["dispatch_attempt_id"] = result.dispatch_attempt_id();
  }
  return value;
}
std::string request_identifier(const OwnerHttpRequest& request) {
  if (const auto value = header(request, "x-request-id");
      value && value->size() >= 8 && value->size() <= 128) {
    return *value;
  }
  return random_identifier();
}
std::string trace_identifier(const OwnerHttpRequest& request) {
  if (const auto value = header(request, "traceparent");
      value && value->size() >= 55 && value->substr(0, 3) == "00-") {
    return value->substr(3, 32);
  }
  return random_identifier();
}
} // namespace

unsigned int owner_http_status(wire::ErrorCode code) {
  switch (code) {
  case wire::INVALID_ARGUMENT:
    return 400;
  case wire::UNAUTHENTICATED:
    return 401;
  case wire::PERMISSION_DENIED:
    return 403;
  case wire::NOT_FOUND:
    return 404;
  case wire::CONFLICT:
    return 409;
  case wire::RATE_LIMITED:
    return 429;
  case wire::TIMEOUT:
    return 504;
  case wire::UPSTREAM_UNAVAILABLE:
  case wire::PLUGIN_UNAVAILABLE:
  case wire::CAPABILITY_NOT_SUPPORTED:
    return 503;
  default:
    return 500;
  }
}

std::string owner_error_body(wire::ErrorCode code, std::string_view request_id,
                             std::string_view trace_id, bool retryable) {
  return error_json(code, request_id, trace_id, retryable).dump();
}

OwnerHttpResult OwnerGateway::live() {
  return {200, R"({"state":"HEALTHY"})"};
}

OwnerHttpResult OwnerGateway::ready(bool ready, std::string_view request_id) {
  if (ready) {
    return {200, R"({"state":"HEALTHY"})"};
  }
  return {503, owner_error_body(wire::UPSTREAM_UNAVAILABLE, request_id, request_id, true)};
}

std::optional<OwnerHttpResult> OwnerGateway::handle(const OwnerHttpRequest& request) {
  try {
    const auto separator = request.target.find('?');
    const auto path = std::string_view{request.target}.substr(0, separator);
    const auto request_id = request_identifier(request);
    const auto trace_id = trace_identifier(request);
    if (path == "/health/live" || path == "/health/ready") {
      if (request.method != "GET" && request.method != "HEAD") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      return path == "/health/live" ? live() : ready(true, request_id);
    }
    // P12c: authoritative log/audit sources are not registered yet. A successful empty page would
    // claim "no events" without evidence, so the boundary answers UNAVAILABLE instead.
    if (path == "/api/v1/logs" || path == "/api/v1/audit") {
      if (request.method != "GET" && request.method != "HEAD") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      return OwnerHttpResult{
          503, owner_error_body(wire::UPSTREAM_UNAVAILABLE, request_id, trace_id, true)};
    }
    constexpr std::string_view kActions = "/api/v1/actions/";
    if (!path.starts_with(kActions)) {
      return std::nullopt;
    }
    if (request.method != "GET" && request.method != "HEAD") {
      return OwnerHttpResult{405,
                             owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
    }
    auto query = parse_query(request.target);
    if (!query) {
      return OwnerHttpResult{400,
                             owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
    }
    auto scope = parse_scope(*query);
    if (!scope) {
      return OwnerHttpResult{400,
                             owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
    }
    const auto selector = path.substr(kActions.size());
    wire::RuntimeQueryActionResultRequest input;
    auto* context = input.mutable_context();
    context->set_request_id(request_id);
    context->set_trace_id(trace_id);
    const auto deadline = codec::to_wire(request.deadline);
    if (const auto* wire_deadline = std::get_if<google::protobuf::Timestamp>(&deadline)) {
      *context->mutable_deadline() = *wire_deadline;
    } else {
      return OwnerHttpResult{400,
                             owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
    }
    *input.mutable_account_scope() = std::move(*scope);
    if (selector == "by-idempotency-key") {
      const auto key = header(request, "idempotency-key");
      const auto owner = query->find("owner_instance");
      if (!key || owner == query->end() || owner->second.empty()) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      input.set_idempotency_key(*key);
      input.set_owner_instance_id(owner->second);
    } else {
      if (selector.empty() || selector.find('/') != std::string_view::npos) {
        return std::nullopt;
      }
      input.set_action_id(std::string{selector});
    }
    // Exactly one account scope + action.inspect is checked by Identity/Runtime; Control never
    // authorizes from its own cache.
    auto delegation =
        delegation_.issue("runtime", wire::PERMISSION_ACTION_INSPECT, input.account_scope(),
                          request.access_token, request.deadline);
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return OwnerHttpResult{
          owner_http_status(failure->code),
          owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
    }
    auto result = runtime_.query_action(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return OwnerHttpResult{
          owner_http_status(failure->code),
          owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
    }
    const auto& response = std::get<wire::RuntimeQueryActionResultResponse>(result);
    if (!response.has_result()) {
      // NOT_FOUND means absent/expired/invisible, never proof that the action failed.
      return OwnerHttpResult{404, owner_error_body(wire::NOT_FOUND, request_id, trace_id, false)};
    }
    return OwnerHttpResult{200, action_result_json(response.result()).dump()};
  } catch (...) {
    const auto request_id = random_identifier();
    return OwnerHttpResult{500, owner_error_body(wire::INTERNAL, request_id, request_id, false)};
  }
}

} // namespace kiarabot::control::target
