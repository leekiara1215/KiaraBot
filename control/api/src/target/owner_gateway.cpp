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
Json operation_json(const wire::Operation& operation) {
  Json value{{"ref",
              {{"owner_service", operation.ref().owner_service()},
               {"operation_id", operation.ref().operation_id()}}}};
  if (operation.ref().has_resource()) {
    value["ref"]["resource"] = {
        {"kind", wire::ResourceKind_Name(operation.ref().resource().kind())},
        {"id", operation.ref().resource().id()}};
  }
  value["version"] = std::to_string(operation.version());
  value["state"] = wire::OperationState_Name(operation.state());
  value["execution_outcome"] = wire::ExecutionOutcome_Name(operation.execution_outcome());
  value["observed_at"] = json_time(operation.observed_at());
  if (operation.has_reason()) {
    value["reason"] = {{"code", wire::ErrorCode_Name(operation.reason().code())},
                       {"message", operation.reason().message()},
                       {"request_id", operation.reason().request_id()},
                       {"trace_id", operation.reason().trace_id()},
                       {"retryable", operation.reason().retryable()},
                       {"details", Json::object()}};
  }
  Json participants = Json::array();
  for (const auto& participant : operation.participants()) {
    Json item{{"participant", participant.participant()},
              {"state", wire::OperationState_Name(participant.state())}};
    if (participant.has_next_attempt_at()) {
      item["next_attempt_at"] = json_time(participant.next_attempt_at());
    }
    if (participant.has_reason()) {
      item["reason"] = {{"code", wire::ErrorCode_Name(participant.reason().code())}};
    }
    participants.push_back(std::move(item));
  }
  value["participants"] = std::move(participants);
  if (operation.has_next_attempt_at()) {
    value["next_attempt_at"] = json_time(operation.next_attempt_at());
  }
  value["stalled"] = operation.stalled();
  return value;
}
Json task_page_json(const wire::SchedulerListTasksResponse& response) {
  // OpenAPI TaskPage(GenericResource items + PageInfo) instead of the raw proto JSON shape.
  Json items = Json::array();
  for (const auto& task : response.tasks()) {
    Json item{{"id", task.task_id()}};
    item["name"] = task.target().id().empty() ? task.task_id() : task.target().id();
    item["state"] = wire::ScheduledTaskState_Name(task.state());
    if (task.has_created_at()) {
      item["created_at"] = json_time(task.created_at());
    }
    items.push_back(std::move(item));
  }
  Json page{{"page_size", response.page().page_size()}};
  if (!response.page().next_page_token().empty()) {
    page["next_page_token"] = response.page().next_page_token();
  }
  return {{"items", std::move(items)}, {"page", std::move(page)}};
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

std::string uint64_text(std::uint64_t value) {
  return std::to_string(value);
}
Json error_json(const wire::Error& error) {
  Json value{{"code", wire::ErrorCode_Name(error.code())},
             {"message", error.message()},
             {"details", Json::object()},
             {"request_id", error.request_id()},
             {"trace_id", error.trace_id()},
             {"retryable", error.retryable()}};
  if (value["request_id"].get<std::string>().empty()) {
    value["request_id"] = "control-request";
  }
  if (value["trace_id"].get<std::string>().empty()) {
    value["trace_id"] = "control-trace";
  }
  if (value["message"].get<std::string>().empty()) {
    value["message"] = wire::ErrorCode_Name(error.code());
  }
  return value;
}
Json model_metadata_json(const wire::ModelMetadata& value) {
  return Json{{"model_id", value.model_id()},
              {"alias", value.alias()},
              {"config_version", uint64_text(value.config_version())},
              {"state", wire::LifecycleState_Name(value.state())},
              {"provider_kind", value.provider_kind()}};
}
Json model_configuration_json(const wire::ModelConfiguration& value) {
  return Json{{"model_id", value.model_id()},
              {"alias", value.alias()},
              {"provider_kind", value.provider_kind()},
              {"endpoint", value.endpoint()},
              {"provider_model", value.provider_model()},
              {"timeout_ms", value.timeout_ms()},
              {"version", uint64_text(value.version())}};
}
Json operation_ref_json(const wire::OperationRef& value) {
  Json ref{{"owner_service", value.owner_service()}, {"operation_id", value.operation_id()}};
  if (value.has_resource()) {
    ref["resource"] = Json{{"kind", wire::ResourceKind_Name(value.resource().kind())},
                           {"id", value.resource().id()}};
  }
  return ref;
}
Json call_record_json(const wire::ModelCallRecord& value) {
  Json record;
  record["logical_call_id"] = value.logical_call_id();
  record["owner_instance_id"] = value.owner_instance_id();
  record["model_id"] = value.model_id();
  record["config_version"] = uint64_text(value.config_version());
  record["state"] = wire::OperationState_Name(value.state());
  record["execution_outcome"] = wire::ExecutionOutcome_Name(value.execution_outcome());
  record["input_tokens"] = uint64_text(value.input_tokens());
  record["output_tokens"] = uint64_text(value.output_tokens());
  if (value.has_observed_at()) {
    record["observed_at"] = json_time(value.observed_at());
  }
  if (value.has_reason()) {
    record["reason"] = error_json(value.reason());
  }
  record["secret_version"] = uint64_text(value.secret_version());
  record["usage_known"] = value.usage_known();
  record["may_have_billed"] = value.may_have_billed();
  record["provider_request_id"] = value.provider_request_id();
  record["latency_ms"] = value.latency_ms();
  record["disposition"] = wire::ModelCallDisposition_Name(value.disposition());
  return record;
}
// P11d model REST row: returns the exact single model resource scope used for the one-shot
// delegation. The model id comes from the path; a query scope must name the same model and nothing
// else, so Control never widens to a role or another model.
std::optional<wire::ResourceScope>
single_model_scope(const std::map<std::string, std::string>& query, std::string_view model_id,
                   bool scope_required) {
  const auto found = query.find("scope");
  if (found == query.end()) {
    if (scope_required) {
      return std::nullopt;
    }
    wire::ResourceScope scope;
    auto* resource = scope.add_resources();
    resource->set_kind(wire::RESOURCE_MODEL);
    resource->set_id(std::string{model_id});
    return scope;
  }
  wire::ResourceScope scope;
  if (!google::protobuf::util::JsonStringToMessage(found->second, &scope).ok() ||
      scope.resources_size() != 1 || scope.resources(0).kind() != wire::RESOURCE_MODEL ||
      scope.resources(0).id() != model_id) {
    return std::nullopt;
  }
  return scope;
}
void fill_context(wire::RequestContext* context, const OwnerHttpRequest& request,
                  const std::string& request_id, const std::string& trace_id) {
  context->set_request_id(request_id);
  context->set_trace_id(trace_id);
  const auto deadline = codec::to_wire(request.deadline);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&deadline)) {
    *context->mutable_deadline() = *timestamp;
  }
}
std::optional<std::uint64_t> parse_uint64_json(const nlohmann::json& value) {
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 20) {
      return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return parsed;
  }
  return std::nullopt;
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
    constexpr std::string_view kOperations = "/api/v1/operations/";
    if (path.starts_with(kOperations)) {
      if (!operations_) {
        return OwnerHttpResult{
            503, owner_error_body(wire::UPSTREAM_UNAVAILABLE, request_id, trace_id, true)};
      }
      const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
      if (method != "GET") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      const auto tail = path.substr(kOperations.size());
      const auto slash = tail.find('/');
      if (slash == std::string_view::npos) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      const auto owner = std::string{tail.substr(0, slash)};
      const auto operation_id = std::string{tail.substr(slash + 1)};
      auto query = parse_query(request.target);
      auto scope = query ? parse_scope(*query) : std::nullopt;
      if (operation_id.empty() || operation_id.find('/') != std::string::npos || !scope) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      wire::Permission permission = wire::PERMISSION_UNSPECIFIED;
      if (owner == "runtime" || owner == "model") {
        permission = wire::PERMISSION_RESOURCE_READ;
      } else if (owner == "scheduler") {
        permission = wire::PERMISSION_TASK_READ;
      } else {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      wire::OperationRef reference;
      reference.set_owner_service(owner);
      reference.set_operation_id(operation_id);
      auto delegation =
          delegation_.issue(owner, permission, *scope, request.access_token, request.deadline);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
      }
      auto result = operations_->get(owner, reference, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
      }
      const auto& operation = std::get<wire::Operation>(result);
      if (operation.ref().operation_id().empty()) {
        // Missing/invisible operations are NOT_FOUND; never claim completion.
        return OwnerHttpResult{404, owner_error_body(wire::NOT_FOUND, request_id, trace_id, false)};
      }
      return OwnerHttpResult{200, Json{{"operation", operation_json(operation)}}.dump()};
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
        return OwnerHttpResult{
            200, task_page_json(std::get<wire::SchedulerListTasksResponse>(result)).dump()};
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
    if (path == "/api/v1/models") {
      if (!model_) {
        return OwnerHttpResult{
            503, owner_error_body(wire::CAPABILITY_NOT_SUPPORTED, request_id, trace_id, false)};
      }
      if (request.method != "GET" && request.method != "HEAD") {
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      if (request.access_token.empty()) {
        return OwnerHttpResult{
            401, owner_error_body(wire::UNAUTHENTICATED, request_id, trace_id, false)};
      }
      auto query = parse_query(request.target);
      auto scope = query ? parse_scope(*query) : std::nullopt;
      if (!query || !scope || scope->resources_size() < 1) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      for (const auto& resource : scope->resources()) {
        if (resource.kind() != wire::RESOURCE_MODEL || resource.id().empty()) {
          return OwnerHttpResult{
              400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
        }
      }
      wire::ModelListModelMetadataRequest input;
      fill_context(input.mutable_context(), request, request_id, trace_id);
      *input.mutable_scope() = *scope;
      if (const auto size = query->find("page_size"); size != query->end()) {
        std::uint32_t value = 0;
        const auto parsed =
            std::from_chars(size->second.data(), size->second.data() + size->second.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != size->second.data() + size->second.size() ||
            value == 0 || value > 100) {
          return OwnerHttpResult{
              400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
        }
        input.mutable_page()->set_page_size(value);
      }
      if (const auto token = query->find("page_token"); token != query->end()) {
        input.mutable_page()->set_page_token(token->second);
      }
      auto delegation = delegation_.issue("model", wire::PERMISSION_MODEL_READ, *scope,
                                          request.access_token, request.deadline);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
      }
      auto result = model_->list_metadata(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return OwnerHttpResult{
            owner_http_status(failure->code),
            owner_error_body(failure->code, request_id, trace_id, failure->retryable)};
      }
      google::protobuf::util::JsonPrintOptions options;
      options.preserve_proto_field_names = true;
      std::string body;
      if (!google::protobuf::util::MessageToJsonString(
               std::get<wire::ModelListModelMetadataResponse>(result), &body, options)
               .ok()) {
        return OwnerHttpResult{500, owner_error_body(wire::INTERNAL, request_id, trace_id, false)};
      }
      return OwnerHttpResult{200, body};
    }
    constexpr std::string_view kModels = "/api/v1/models/";
    if (path.starts_with(kModels)) {
      if (!model_) {
        return OwnerHttpResult{
            503, owner_error_body(wire::CAPABILITY_NOT_SUPPORTED, request_id, trace_id, false)};
      }
      auto query = parse_query(request.target);
      if (!query) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      const auto rest = path.substr(kModels.size());
      const auto first = rest.find('/');
      const auto model_id = rest.substr(0, first);
      if (model_id.empty()) {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      const auto tail =
          first == std::string_view::npos ? std::string_view{} : rest.substr(first + 1);
      const auto model_scope = [&]() -> std::optional<wire::ResourceScope> {
        wire::ResourceScope scope;
        auto* resource = scope.add_resources();
        resource->set_kind(wire::RESOURCE_MODEL);
        resource->set_id(std::string{model_id});
        return scope;
      }();
      const auto require_idempotency = [&]() -> std::optional<OwnerHttpResult> {
        if (!header(request, "idempotency-key")) {
          return OwnerHttpResult{
              400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
        }
        return std::nullopt;
      };
      const auto dispatch = [&](wire::Permission permission,
                                const wire::ResourceScope& scope) -> OwnerOutcome<std::string> {
        return delegation_.issue("model", permission, scope, request.access_token,
                                 request.deadline);
      };
      const auto fail = [&](OwnerFailure failure) -> OwnerHttpResult {
        return OwnerHttpResult{
            owner_http_status(failure.code),
            owner_error_body(failure.code, request_id, trace_id, failure.retryable)};
      };
      const auto invalid = [&]() -> OwnerHttpResult {
        return OwnerHttpResult{
            400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      };
      if (tail.empty() && (request.method == "GET" || request.method == "HEAD")) {
        auto scope = single_model_scope(*query, model_id, true);
        if (!scope) {
          return OwnerHttpResult{
              400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
        }
        auto delegation = dispatch(wire::PERMISSION_MODEL_READ, *scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return fail(*failure);
        }
        wire::ModelGetModelMetadataRequest input;
        fill_context(input.mutable_context(), request, request_id, trace_id);
        input.set_model_id(std::string{model_id});
        *input.mutable_scope() = *scope;
        auto result = model_->metadata(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return fail(*failure);
        }
        const auto& response = std::get<wire::ModelGetModelMetadataResponse>(result);
        if (!response.has_model()) {
          return OwnerHttpResult{404,
                                 owner_error_body(wire::NOT_FOUND, request_id, trace_id, false)};
        }
        google::protobuf::util::JsonPrintOptions options;
        options.preserve_proto_field_names = true;
        wire::ModelGetModelMetadataResponse body;
        *body.mutable_model() = response.model();
        std::string encoded;
        if (!google::protobuf::util::MessageToJsonString(body, &encoded, options).ok()) {
          return OwnerHttpResult{500,
                                 owner_error_body(wire::INTERNAL, request_id, trace_id, false)};
        }
        return OwnerHttpResult{200, encoded};
      }
      if (tail.empty() && request.method == "PUT") {
        if (const auto missing = require_idempotency()) {
          return *missing;
        }
        wire::ModelUpdateModelRequest input;
        if (!google::protobuf::util::JsonStringToMessage(request.body, &input).ok() ||
            !input.has_config() || input.config().model_id() != model_id) {
          return invalid();
        }
        fill_context(input.mutable_context(), request, request_id, trace_id);
        auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *model_scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return fail(*failure);
        }
        auto result = model_->update(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return fail(*failure);
        }
        const auto& response = std::get<wire::ModelUpdateModelResponse>(result);
        Json body;
        if (response.has_model()) {
          body["model"] = model_metadata_json(response.model());
        }
        if (response.has_operation()) {
          body["operation"] = operation_ref_json(response.operation());
        }
        return OwnerHttpResult{200, body.dump()};
      }
      const auto lifecycle = [&](bool enable) -> std::optional<OwnerHttpResult> {
        if (request.method != "POST") {
          return std::nullopt;
        }
        if (const auto missing = require_idempotency()) {
          return *missing;
        }
        std::optional<std::uint64_t> expected;
        if (!request.body.empty()) {
          const auto parsed = Json::parse(request.body, nullptr, false);
          if (!parsed.is_object() ||
              (parsed.contains("expected_version") &&
               !(expected = parse_uint64_json(parsed["expected_version"])))) {
            return invalid();
          }
        }
        if (!expected) {
          return invalid();
        }
        auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *model_scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return fail(*failure);
        }
        const auto proof = std::get<std::string>(delegation);
        Json body;
        if (enable) {
          wire::ModelEnableModelRequest input;
          fill_context(input.mutable_context(), request, request_id, trace_id);
          input.set_model_id(std::string{model_id});
          input.set_expected_version(*expected);
          auto result = model_->enable(input, proof);
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return fail(*failure);
          }
          const auto& response = std::get<wire::ModelEnableModelResponse>(result);
          if (response.has_model()) {
            body["model"] = model_metadata_json(response.model());
          }
        } else {
          wire::ModelDisableModelRequest input;
          fill_context(input.mutable_context(), request, request_id, trace_id);
          input.set_model_id(std::string{model_id});
          input.set_expected_version(*expected);
          auto result = model_->disable(input, proof);
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return fail(*failure);
          }
          const auto& response = std::get<wire::ModelDisableModelResponse>(result);
          if (response.has_model()) {
            body["model"] = model_metadata_json(response.model());
          }
        }
        return OwnerHttpResult{200, body.dump()};
      };
      if (tail == "disable" || tail == "enable") {
        if (const auto handled = lifecycle(tail == "enable")) {
          return *handled;
        }
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      if (tail == "secret" && request.method == "PUT") {
        if (const auto missing = require_idempotency()) {
          return *missing;
        }
        wire::ModelRotateProviderSecretRequest input;
        if (!google::protobuf::util::JsonStringToMessage(request.body, &input).ok() ||
            input.model_id() != model_id) {
          return invalid();
        }
        fill_context(input.mutable_context(), request, request_id, trace_id);
        auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *model_scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return fail(*failure);
        }
        auto result = model_->rotate_secret(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return fail(*failure);
        }
        const auto& response = std::get<wire::ModelRotateProviderSecretResponse>(result);
        Json body{{"version", uint64_text(response.version())}};
        if (response.has_operation()) {
          body["operation"] = operation_ref_json(response.operation());
        }
        return OwnerHttpResult{200, body.dump()};
      }
      if (tail == "configuration" && request.method == "GET") {
        auto scope = single_model_scope(*query, model_id, true);
        if (!scope) {
          return invalid();
        }
        auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return fail(*failure);
        }
        wire::ModelGetModelConfigurationRequest input;
        fill_context(input.mutable_context(), request, request_id, trace_id);
        input.set_model_id(std::string{model_id});
        *input.mutable_scope() = *scope;
        auto result = model_->configuration(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return fail(*failure);
        }
        const auto& response = std::get<wire::ModelGetModelConfigurationResponse>(result);
        Json body;
        if (response.has_config()) {
          body["config"] = model_configuration_json(response.config());
        }
        body["secret_configured"] = response.secret_configured();
        body["secret_version"] = uint64_text(response.secret_version());
        if (response.has_observed_at()) {
          body["observed_at"] = json_time(response.observed_at());
        }
        return OwnerHttpResult{200, body.dump()};
      }
      if (tail.starts_with("calls/")) {
        const auto call_rest = tail.substr(6);
        const auto slash = call_rest.find('/');
        const auto call_id = call_rest.substr(0, slash);
        if (call_id.empty()) {
          return invalid();
        }
        const auto resolve =
            slash != std::string_view::npos && call_rest.substr(slash + 1) == "resolve";
        if (slash != std::string_view::npos && !resolve) {
          return OwnerHttpResult{
              400, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
        }
        if (!resolve && request.method == "GET") {
          auto scope = single_model_scope(*query, model_id, true);
          if (!scope) {
            return invalid();
          }
          auto delegation = dispatch(wire::PERMISSION_MODEL_CALL_INSPECT, *scope);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return fail(*failure);
          }
          wire::ModelGetCallStatusRequest input;
          fill_context(input.mutable_context(), request, request_id, trace_id);
          input.set_logical_call_id(std::string{call_id});
          *input.mutable_scope() = *scope;
          auto result = model_->call_status(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return fail(*failure);
          }
          const auto& response = std::get<wire::ModelGetCallStatusResponse>(result);
          if (!response.has_call()) {
            return OwnerHttpResult{404,
                                   owner_error_body(wire::NOT_FOUND, request_id, trace_id, false)};
          }
          return OwnerHttpResult{200, Json{{"call", call_record_json(response.call())}}.dump()};
        }
        if (resolve && request.method == "POST") {
          if (const auto missing = require_idempotency()) {
            return *missing;
          }
          const auto parsed = Json::parse(request.body, nullptr, false);
          if (!parsed.is_object()) {
            return invalid();
          }
          wire::ModelResolveModelCallRequest input;
          fill_context(input.mutable_context(), request, request_id, trace_id);
          input.set_logical_call_id(std::string{call_id});
          if (parsed.contains("expected_version")) {
            const auto expected = parse_uint64_json(parsed["expected_version"]);
            if (!expected) {
              return invalid();
            }
            input.set_expected_version(*expected);
          }
          if (!parsed.contains("disposition") || !parsed["disposition"].is_string()) {
            return invalid();
          }
          wire::ModelCallDisposition disposition = wire::MODEL_CALL_DISPOSITION_UNSPECIFIED;
          if (!wire::ModelCallDisposition_Parse(parsed["disposition"].get<std::string>(),
                                                &disposition) ||
              disposition == wire::MODEL_CALL_DISPOSITION_UNSPECIFIED) {
            return invalid();
          }
          input.set_disposition(disposition);
          // Resolve requires the exact model scope just like the read path; absent scope query
          // derives it from the path, never from role membership.
          auto scope = single_model_scope(*query, model_id, false);
          if (!scope) {
            return invalid();
          }
          auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *scope);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return fail(*failure);
          }
          auto result = model_->resolve(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return fail(*failure);
          }
          const auto& response = std::get<wire::ModelResolveModelCallResponse>(result);
          if (!response.has_call()) {
            return OwnerHttpResult{404,
                                   owner_error_body(wire::NOT_FOUND, request_id, trace_id, false)};
          }
          return OwnerHttpResult{200, Json{{"call", call_record_json(response.call())}}.dump()};
        }
        return OwnerHttpResult{
            405, owner_error_body(wire::INVALID_ARGUMENT, request_id, trace_id, false)};
      }
      // Metadata/List paths keep the P12c explicit-unavailable behavior until their Control route
      // is wired; never fall through to a fabricated empty page.
      return OwnerHttpResult{
          503, owner_error_body(wire::CAPABILITY_NOT_SUPPORTED, request_id, trace_id, false)};
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
