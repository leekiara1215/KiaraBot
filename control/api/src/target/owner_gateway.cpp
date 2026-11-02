#include "kiarabot/control/target/owner_gateway.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
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
std::string now_iso() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  google::protobuf::Timestamp stamp;
  stamp.set_seconds(seconds.count());
  stamp.set_nanos(static_cast<std::int32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch() - seconds)
          .count()));
  return json_time(stamp);
}
Json component(std::string_view name, bool ready) {
  return {{"component", std::string{name}},
          {"state", ready ? "HEALTHY" : "DEGRADED"},
          {"checked_at", now_iso()}};
}
std::string health_body(const HealthObservation& observation) {
  const bool degraded =
      !observation.runtime_ready || !observation.scheduler_ready || !observation.model_ready;
  Json value;
  value["state"] = observation.identity_ready ? (degraded ? "DEGRADED" : "HEALTHY") : "UNHEALTHY";
  value["components"] = Json::array({component("identity", observation.identity_ready),
                                     component("runtime", observation.runtime_ready),
                                     component("scheduler", observation.scheduler_ready),
                                     component("model", observation.model_ready)});
  return value.dump();
}
template <typename Message> bool json_request(const std::string& body, Message& value) {
  return !body.empty() && google::protobuf::util::JsonStringToMessage(body, &value).ok();
}
template <typename Message> std::string json_response(const Message& value) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string body;
  if (!google::protobuf::util::MessageToJsonString(value, &body, options).ok()) {
    return "{}";
  }
  return body;
}
Json json_request_value(const std::string& body) {
  auto parsed = Json::parse(body, nullptr, false);
  return parsed.is_object() ? parsed : Json::object();
}
wire::ResourceScope account_scope(std::string_view id) {
  wire::ResourceScope scope;
  auto* resource = scope.add_resources();
  resource->set_kind(wire::RESOURCE_ACCOUNT);
  resource->set_id(std::string{id});
  return scope;
}
wire::ResourceScope instance_scope(std::string_view id) {
  wire::ResourceScope scope;
  auto* resource = scope.add_resources();
  resource->set_kind(wire::RESOURCE_INSTANCE);
  resource->set_id(std::string{id});
  return scope;
}
bool fill_context(wire::RequestContext* context, std::string_view request_id,
                  std::string_view trace_id, const OwnerHttpRequest& request) {
  context->set_request_id(std::string{request_id});
  context->set_trace_id(std::string{trace_id});
  const auto deadline = codec::to_wire(request.deadline);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&deadline)) {
    *context->mutable_deadline() = *timestamp;
    return true;
  }
  return false;
}
std::optional<wire::ResourceScope> optional_scope(const std::map<std::string, std::string>& query) {
  const auto found = query.find("scope");
  if (found == query.end()) {
    return wire::ResourceScope{};
  }
  wire::ResourceScope scope;
  if (!google::protobuf::util::JsonStringToMessage(found->second, &scope).ok()) {
    return std::nullopt;
  }
  return scope;
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
      if (path == "/health/live") {
        return live();
      }
      const auto ready_state = health_ ? health_->observe().identity_ready : false;
      return ready(ready_state, request_id);
    }
    if (path == "/api/v1/health") {
      if (request.method != "GET" && request.method != "HEAD") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      if (request.access_token.empty()) {
        return OwnerHttpResult{
            401, owner_error_body(wire::UNAUTHENTICATED, request_id, trace_id, false)};
      }
      if (!health_) {
        return OwnerHttpResult{503, health_body(HealthObservation{})};
      }
      const auto observation = health_->observe();
      return OwnerHttpResult{static_cast<unsigned int>(observation.identity_ready ? 200 : 503),
                             health_body(observation)};
    }
    if (path == "/api/v1/principals") {
      if (request.method != "GET" && request.method != "HEAD") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      if (request.access_token.empty()) {
        return OwnerHttpResult{
            401, owner_error_body(wire::UNAUTHENTICATED, request_id, trace_id, false)};
      }
      if (!principals_) {
        return OwnerHttpResult{
            503, owner_error_body(wire::UPSTREAM_UNAVAILABLE, request_id, trace_id, true)};
      }
      auto query = parse_query(request.target);
      if (!query) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      wire::IdentityListPrincipalsRequest input;
      auto* context = input.mutable_context();
      context->set_request_id(request_id);
      context->set_trace_id(trace_id);
      const auto deadline = codec::to_wire(request.deadline);
      if (const auto* wire_deadline = std::get_if<google::protobuf::Timestamp>(&deadline)) {
        *context->mutable_deadline() = *wire_deadline;
      }
      if (const auto size = query->find("page_size"); size != query->end()) {
        std::uint32_t value = 0;
        const auto parsed =
            std::from_chars(size->second.data(), size->second.data() + size->second.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != size->second.data() + size->second.size() ||
            value == 0) {
          return OwnerHttpResult{
              400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
        }
        input.mutable_page()->set_page_size(value);
      }
      if (const auto token = query->find("page_token"); token != query->end()) {
        input.mutable_page()->set_page_token(token->second);
      }
      auto delegation =
          delegation_.issue("identity", wire::PERMISSION_IDENTITY_PRINCIPAL_READ,
                            wire::ResourceScope{}, request.access_token, request.deadline);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
      }
      auto result = principals_->list_principals(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
      }
      google::protobuf::util::JsonPrintOptions options;
      options.preserve_proto_field_names = true;
      std::string body;
      if (!google::protobuf::util::MessageToJsonString(
               std::get<wire::IdentityListPrincipalsResponse>(result), &body, options)
               .ok()) {
        return OwnerHttpResult{500, owner_error_body(wire::INTERNAL, request_id, trace_id, false)};
      }
      return OwnerHttpResult{200, body};
    }
    // P12c-3: owner commands under the same one-use-delegation gateway. Owner schemas mirror the
    // protobuf messages; Control forwards persisted operation references and never claims work
    // completed (202 semantics stay at the HTTP layer, operation progress is owner-owned).
    {
      constexpr std::string_view kBind = "/api/v1/adapter-bindings/";
      constexpr std::string_view kInstallations = "/api/v1/installations";
      constexpr std::string_view kInstances = "/api/v1/instances/";
      constexpr std::string_view kPolicy = "/api/v1/accounts/";
      constexpr std::string_view kTasks = "/api/v1/tasks";
      const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
      const auto missing = [&](wire::ErrorCode code) {
        return OwnerHttpResult{owner_http_status(code),
                               owner_error_body(code, request_id, trace_id, false)};
      };
      if (!resources_ && (path.starts_with(kBind) || path == kInstallations ||
                          path.starts_with(kInstances) || path.starts_with(kPolicy))) {
        return missing(wire::UPSTREAM_UNAVAILABLE);
      }
      if (!tasks_ && path.starts_with(kTasks)) {
        return missing(wire::UPSTREAM_UNAVAILABLE);
      }
      if (path.starts_with(kBind)) {
        const auto account = std::string{path.substr(kBind.size())};
        if (account.empty() || account.find('/') != std::string::npos) {
          return missing(wire::NOT_FOUND);
        }
        if (method == "PUT") {
          wire::RuntimeBindAdapterRequest input;
          if (!json_request(request.body, input) || !input.has_binding() ||
              input.binding().account_id() != account || !input.has_expected_version()) {
            return missing(wire::INVALID_ARGUMENT);
          }
          if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
            return missing(wire::INVALID_ARGUMENT);
          }
          auto delegation =
              delegation_.issue("runtime", wire::PERMISSION_ACCOUNT_MANAGE, account_scope(account),
                                request.access_token, request.deadline);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return missing(failure->code);
          }
          auto result = resources_->bind_adapter(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return missing(failure->code);
          }
          auto value =
              json_request_value(json_response(std::get<wire::RuntimeBindAdapterResponse>(result)));
          value.erase("bootstrap_credential");
          return OwnerHttpResult{200, value.dump()};
        }
        if (method == "DELETE") {
          wire::RuntimeUnbindAdapterRequest input;
          if (!json_request(request.body, input) || !input.has_binding() ||
              input.binding().account_id() != account || !input.has_expected_version()) {
            return missing(wire::INVALID_ARGUMENT);
          }
          if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
            return missing(wire::INVALID_ARGUMENT);
          }
          auto delegation =
              delegation_.issue("runtime", wire::PERMISSION_ACCOUNT_MANAGE, account_scope(account),
                                request.access_token, request.deadline);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return missing(failure->code);
          }
          auto result = resources_->unbind_adapter(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return missing(failure->code);
          }
          return OwnerHttpResult{
              200, json_response(std::get<wire::RuntimeUnbindAdapterResponse>(result))};
        }
        return missing(wire::INVALID_ARGUMENT);
      }
      if (path == kInstallations && method == "POST") {
        wire::RuntimeCreateInstallationRequest input;
        if (!json_request(request.body, input) ||
            !fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return missing(wire::INVALID_ARGUMENT);
        }
        auto delegation =
            delegation_.issue("runtime", wire::PERMISSION_PLUGIN_MANAGE, wire::ResourceScope{},
                              request.access_token, request.deadline);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return missing(failure->code);
        }
        auto result = resources_->create_installation(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return missing(failure->code);
        }
        return OwnerHttpResult{
            200, json_response(std::get<wire::RuntimeCreateInstallationResponse>(result))};
      }
      if (path.starts_with("/api/v1/installations/") && path.ends_with("/instances") &&
          method == "POST") {
        const auto prefix = std::string{"/api/v1/installations/"};
        const auto installation = std::string{path.substr(
            prefix.size(), path.size() - prefix.size() - std::string{"/instances"}.size())};
        wire::RuntimeCreateInstanceRequest input;
        if (installation.empty() || installation.find('/') != std::string::npos ||
            !json_request(request.body, input) || input.installation_id() != installation ||
            !fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return missing(wire::INVALID_ARGUMENT);
        }
        auto delegation =
            delegation_.issue("runtime", wire::PERMISSION_PLUGIN_MANAGE, wire::ResourceScope{},
                              request.access_token, request.deadline);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return missing(failure->code);
        }
        auto result = resources_->create_instance(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return missing(failure->code);
        }
        auto value = json_request_value(
            json_response(std::get<wire::RuntimeCreateInstanceResponse>(result)));
        value.erase("bootstrap_credential");
        return OwnerHttpResult{200, value.dump()};
      }
      if (path.starts_with(kInstances)) {
        const auto tail = path.substr(kInstances.size());
        const auto slash = tail.find('/');
        if (slash == std::string_view::npos) {
          return missing(wire::NOT_FOUND);
        }
        const auto instance = std::string{tail.substr(0, slash)};
        const auto action = tail.substr(slash);
        if (instance.empty() || action == "/desired-state" || action == "/config") {
          const auto scope = instance_scope(instance);
          if (action == "/desired-state" && method == "PUT") {
            wire::RuntimeSetDesiredStateRequest input;
            if (!json_request(request.body, input) || input.instance_id() != instance ||
                !input.has_expected_version() ||
                !fill_context(input.mutable_context(), request_id, trace_id, request)) {
              return missing(wire::INVALID_ARGUMENT);
            }
            auto delegation = delegation_.issue("runtime", wire::PERMISSION_PLUGIN_MANAGE, scope,
                                                request.access_token, request.deadline);
            if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
              return missing(failure->code);
            }
            auto result = resources_->set_desired_state(input, std::get<std::string>(delegation));
            if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
              return missing(failure->code);
            }
            return OwnerHttpResult{
                200, json_response(std::get<wire::RuntimeSetDesiredStateResponse>(result))};
          }
          if (action == "/config" && method == "PUT") {
            wire::RuntimeUpdateConfigRequest input;
            if (!json_request(request.body, input) || !input.has_resource() ||
                input.resource().id() != instance || !input.has_expected_version() ||
                !fill_context(input.mutable_context(), request_id, trace_id, request)) {
              return missing(wire::INVALID_ARGUMENT);
            }
            auto delegation = delegation_.issue("runtime", wire::PERMISSION_PLUGIN_MANAGE, scope,
                                                request.access_token, request.deadline);
            if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
              return missing(failure->code);
            }
            auto result = resources_->update_config(input, std::get<std::string>(delegation));
            if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
              return missing(failure->code);
            }
            return OwnerHttpResult{
                200, json_response(std::get<wire::RuntimeUpdateConfigResponse>(result))};
          }
          if (action == "/config" && method == "GET") {
            wire::RuntimeGetInstanceConfigRequest input;
            input.mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
            input.mutable_resource()->set_id(instance);
            if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
              return missing(wire::INVALID_ARGUMENT);
            }
            auto delegation = delegation_.issue("runtime", wire::PERMISSION_PLUGIN_CONFIG_READ,
                                                scope, request.access_token, request.deadline);
            if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
              return missing(failure->code);
            }
            auto result = resources_->get_instance_config(input, std::get<std::string>(delegation));
            if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
              return missing(failure->code);
            }
            return OwnerHttpResult{
                200, json_response(std::get<wire::RuntimeGetInstanceConfigResponse>(result))};
          }
        }
        return missing(wire::NOT_FOUND);
      }
      if (path.starts_with(kPolicy) && path.ends_with("/action-policy")) {
        const auto prefix = std::string{kPolicy};
        const auto account = std::string{path.substr(
            prefix.size(), path.size() - prefix.size() - std::string{"/action-policy"}.size())};
        if (account.empty() || account.find('/') != std::string::npos) {
          return missing(wire::NOT_FOUND);
        }
        const auto scope = account_scope(account);
        if (method == "GET") {
          wire::RuntimeGetActionPolicyRequest input;
          input.set_account_id(account);
          if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
            return missing(wire::INVALID_ARGUMENT);
          }
          auto delegation = delegation_.issue("runtime", wire::PERMISSION_ACCOUNT_POLICY_READ,
                                              scope, request.access_token, request.deadline);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return missing(failure->code);
          }
          auto result = resources_->get_action_policy(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return missing(failure->code);
          }
          return OwnerHttpResult{
              200, json_response(std::get<wire::RuntimeGetActionPolicyResponse>(result))};
        }
        if (method == "PUT") {
          wire::RuntimeUpdateActionPolicyRequest input;
          if (!json_request(request.body, input) || !input.has_policy() ||
              input.policy().account_id() != account || !input.has_expected_version() ||
              !fill_context(input.mutable_context(), request_id, trace_id, request)) {
            return missing(wire::INVALID_ARGUMENT);
          }
          auto delegation = delegation_.issue("runtime", wire::PERMISSION_ACCOUNT_POLICY_MANAGE,
                                              scope, request.access_token, request.deadline);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return missing(failure->code);
          }
          auto result = resources_->update_action_policy(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return missing(failure->code);
          }
          return OwnerHttpResult{
              200, json_response(std::get<wire::RuntimeUpdateActionPolicyResponse>(result))};
        }
        return missing(wire::INVALID_ARGUMENT);
      }
      if (path == kTasks && method == "GET") {
        wire::SchedulerListTasksRequest input;
        auto query = parse_query(request.target);
        if (!query || !fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return missing(wire::INVALID_ARGUMENT);
        }
        auto scope = optional_scope(*query);
        if (!scope) {
          return missing(wire::INVALID_ARGUMENT);
        }
        *input.mutable_scope() = std::move(*scope);
        if (const auto size = query->find("page_size"); size != query->end()) {
          std::uint32_t value = 0;
          const auto parsed = std::from_chars(size->second.data(),
                                              size->second.data() + size->second.size(), value);
          if (parsed.ec != std::errc{} || parsed.ptr != size->second.data() + size->second.size()) {
            return missing(wire::INVALID_ARGUMENT);
          }
          input.mutable_page()->set_page_size(value);
        }
        if (const auto token = query->find("page_token"); token != query->end()) {
          input.mutable_page()->set_page_token(token->second);
        }
        auto delegation = delegation_.issue("scheduler", wire::PERMISSION_TASK_READ, input.scope(),
                                            request.access_token, request.deadline);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return missing(failure->code);
        }
        auto result = tasks_->list_tasks(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return missing(failure->code);
        }
        return OwnerHttpResult{200,
                               json_response(std::get<wire::SchedulerListTasksResponse>(result))};
      }
      if (path.starts_with(kTasks) && path.ends_with("/cancel") && method == "POST") {
        const auto prefix = std::string{kTasks} + "/";
        const auto task = std::string{path.substr(
            prefix.size(), path.size() - prefix.size() - std::string{"/cancel"}.size())};
        auto query = parse_query(request.target);
        if (task.empty() || task.find('/') != std::string::npos || !query) {
          return missing(wire::INVALID_ARGUMENT);
        }
        auto scope = optional_scope(*query);
        if (!scope) {
          return missing(wire::INVALID_ARGUMENT);
        }
        wire::SchedulerCancelTaskRequest input;
        input.set_task_id(task);
        *input.mutable_scope() = std::move(*scope);
        if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return missing(wire::INVALID_ARGUMENT);
        }
        auto delegation = delegation_.issue("scheduler", wire::PERMISSION_TASK_SCHEDULE,
                                            input.scope(), request.access_token, request.deadline);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return missing(failure->code);
        }
        auto result = tasks_->cancel_task(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return missing(failure->code);
        }
        return OwnerHttpResult{200,
                               json_response(std::get<wire::SchedulerCancelTaskResponse>(result))};
      }
      if (path.starts_with(kTasks) && method == "GET") {
        const auto prefix = std::string{kTasks} + "/";
        const auto task = std::string{path.substr(prefix.size())};
        auto query = parse_query(request.target);
        if (task.empty() || task.find('/') != std::string::npos || !query) {
          return missing(wire::NOT_FOUND);
        }
        auto scope = optional_scope(*query);
        if (!scope) {
          return missing(wire::INVALID_ARGUMENT);
        }
        wire::SchedulerGetTaskRequest input;
        input.set_task_id(task);
        *input.mutable_scope() = std::move(*scope);
        if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return missing(wire::INVALID_ARGUMENT);
        }
        auto delegation = delegation_.issue("scheduler", wire::PERMISSION_TASK_READ, input.scope(),
                                            request.access_token, request.deadline);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return missing(failure->code);
        }
        auto result = tasks_->get_task(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return missing(failure->code);
        }
        return OwnerHttpResult{200,
                               json_response(std::get<wire::SchedulerGetTaskResponse>(result))};
      }
    }
    // P12c: authoritative log/audit sources are not registered yet. A successful empty page would
    // claim "no events" without evidence; but first enforce the separate sensitive grants.
    if (path == "/api/v1/logs" || path == "/api/v1/audit") {
      if (request.method != "GET" && request.method != "HEAD") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      if (request.access_token.empty()) {
        return OwnerHttpResult{
            401, owner_error_body(wire::UNAUTHENTICATED, request_id, trace_id, false)};
      }
      const auto permission =
          path == "/api/v1/logs" ? wire::PERMISSION_LOG_READ : wire::PERMISSION_AUDIT_READ;
      auto delegation = delegation_.issue("control", permission, wire::ResourceScope{},
                                          request.access_token, request.deadline);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
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
