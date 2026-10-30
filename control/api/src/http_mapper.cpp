#include "kiarabot/control/http_mapper.hpp"

#include <charconv>
#include <regex>

#include <google/protobuf/util/json_util.h>

#include "kiarabot/core/application/control_service.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"

namespace kiarabot::control {
namespace {
using Json = core::ControlJson;
namespace wire = kiarabot::v1;
std::string header(const HttpRequest& request, const std::string& key) {
  const auto found = request.headers.find(key);
  return found == request.headers.end() ? "" : found->second;
}
HttpReply failure(wire::ErrorCode code, const core::ControlMetadata& meta, bool retryable = false) {
  return {core::control_http_status(code),
          Json{{"code", wire::ErrorCode_Name(code)},
               {"message", wire::ErrorCode_Name(code)},
               {"request_id", meta.request_id},
               {"trace_id", meta.trace_id},
               {"retryable", retryable},
               {"details", Json::object()}}
              .dump(),
          code == wire::RATE_LIMITED ? std::map<std::string, std::string>{{"Retry-After", "60"}}
                                     : std::map<std::string, std::string>{}};
}
std::string decode(std::string_view input) {
  std::string result;
  for (std::size_t index = 0; index < input.size(); ++index) {
    unsigned value = static_cast<unsigned char>(input[index]);
    if (input[index] == '%') {
      if (index + 2 >= input.size()) {
        throw std::invalid_argument("encoding");
      }
      const auto code =
          std::from_chars(input.data() + index + 1, input.data() + index + 3, value, 16);
      if (code.ec != std::errc{} || code.ptr != input.data() + index + 3) {
        throw std::invalid_argument("encoding");
      }
      index += 2;
    }
    if (value < 32 || value >= 127 || value == '/' || value == '\\') {
      throw std::invalid_argument("encoding");
    }
    result += static_cast<char>(value);
  }
  return result;
}
Json document(const core::ControlReply& reply) {
  return std::visit(
      [](const auto& value) -> Json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::ControlTokenPair>) {
          return {{"access_token", value.access_token},
                  {"refresh_token", value.refresh_token},
                  {"expires_in", value.expires_in}};
        } else if constexpr (std::is_same_v<T, core::ControlPrincipal>) {
          return {{"user_id", value.user_id},
                  {"session_id", value.session_id},
                  {"roles", value.roles},
                  {"permissions", value.permissions}};
        } else if constexpr (std::is_same_v<T, core::ControlPage>) {
          Json items = Json::array();
          for (const auto& item : value.items) {
            items.push_back(core::control_json(item));
          }
          Json result{{"items", items}, {"page", {{"page_size", value.page_size}}}};
          if (value.next_page_token) {
            result["page"]["next_page_token"] = *value.next_page_token;
          }
          return result;
        } else if constexpr (std::is_same_v<T, core::ControlPluginConfig>) {
          Json result{{"version", value.version}, {"values", core::control_json(value.values)}};
          if (value.delivery) {
            auto& delivery = result["delivery"];
            delivery = {{"state", value.delivery->state}};
            if (!value.delivery->applied_version.empty()) {
              delivery["applied_version"] = value.delivery->applied_version;
            }
            if (value.delivery->error) {
              delivery["error"] = core::control_json(*value.delivery->error);
            }
          }
          return result;
        } else if constexpr (std::is_same_v<T, core::ControlValidation>) {
          return {{"valid", value.valid}, {"errors", value.errors}};
        } else if constexpr (std::is_same_v<T, core::ControlHealthResult>) {
          Json items = Json::array();
          for (const auto& item : value.components) {
            items.push_back({{"component", item.component},
                             {"state", wire::HealthState_Name(item.state)},
                             {"reason", item.reason}});
          }
          return {{"state", wire::HealthState_Name(value.state)}, {"components", items}};
        } else if constexpr (std::is_same_v<T, wire::ActionResult>) {
          return core::control_json(value);
        } else {
          return Json::object();
        }
      },
      reply);
}
} // namespace
HttpReply handle_http(core::IControlApplication& application, const HttpRequest& request) {
  core::ControlMetadata meta;
  meta.request_id = core::random_instance_id();
  meta.trace_id = core::random_instance_id();
  meta.deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
  meta.idempotency_key = header(request, "idempotency-key");
  const auto auth = header(request, "authorization");
  if (auth.starts_with("Bearer ")) {
    meta.access_token = auth.substr(7);
  }
  try {
    if (request.body.size() > 65536 || request.target.size() > 4096) {
      return failure(wire::INVALID_ARGUMENT, meta);
    }
    const auto separator = request.target.find('?');
    const auto path = request.target.substr(0, separator);
    std::map<std::string, std::string> query;
    if (separator != std::string::npos) {
      auto rest = request.target.substr(separator + 1);
      while (!rest.empty()) {
        const auto end = rest.find('&');
        const auto part = rest.substr(0, end);
        const auto equal = part.find('=');
        if (equal == std::string::npos ||
            !query.emplace(decode(part.substr(0, equal)), decode(part.substr(equal + 1))).second) {
          return failure(wire::INVALID_ARGUMENT, meta);
        }
        rest = end == std::string::npos ? "" : rest.substr(end + 1);
      }
    }
    for (const auto& [name, value] : query) {
      (void)value;
      if (name != "page_size" && name != "page_token") {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
    }
    if (path == "/health/live" || path == "/health/ready") {
      if (request.method != "GET" || !query.empty() || !request.body.empty()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      if (path == "/health/ready") {
        auto* service = dynamic_cast<core::ControlService*>(&application);
        if (!service) {
          return failure(wire::UPSTREAM_UNAVAILABLE, meta);
        }
        const auto result = service->readiness();
        if (const auto* error = std::get_if<wire::Error>(&result)) {
          return failure(error->code(), meta, true);
        }
      }
      return {200, R"({"state":"HEALTHY"})", {}};
    }
    std::optional<core::ControlOperation> operation;
    Json body = Json::object();
    if (!request.body.empty()) {
      if (header(request, "content-type") != "application/json") {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      body = Json::parse(request.body);
      if (!body.is_object()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
    }
    if (request.method == "POST" && path == "/api/v1/auth/login") {
      operation = core::ControlLogin{body.at("username").get<std::string>(),
                                     body.at("password").get<std::string>()};
    } else if (request.method == "POST" && path == "/api/v1/auth/refresh") {
      operation = core::ControlRefresh{body.at("refresh_token").get<std::string>()};
    } else if (request.method == "GET" && path == "/api/v1/auth/me") {
      operation = core::ControlPrincipalQuery{};
    } else if (request.method == "GET" && path == "/api/v1/health") {
      operation = core::ControlHealth{};
    } else if (request.method == "POST" && path == "/api/v1/plugins/validate") {
      wire::PluginManifest manifest;
      if (!google::protobuf::util::JsonStringToMessage(body.dump(), &manifest).ok()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      operation = core::ControlValidatePlugin{manifest};
    } else if (request.method == "GET" && path == "/api/v1/actions/by-idempotency-key") {
      operation = core::ControlActionQuery{meta.idempotency_key, true};
    } else {
      std::smatch match;
      if (request.method == "POST" &&
          std::regex_match(path, match, std::regex("/api/v1/auth/sessions/([^/]+)/revoke"))) {
        operation = core::ControlRevokeSession{decode(match[1].str())};
      } else if (std::regex_match(
                     path, match,
                     std::regex("/api/v1/plugins/([^/]+)/(enable|disable|config|health)"))) {
        const auto id = decode(match[1].str());
        const auto kind = match[2].str();
        if (request.method == "POST" && (kind == "enable" || kind == "disable")) {
          operation = core::ControlSetPluginEnabled{id, kind == "enable"};
        }
        if (request.method == "GET" && kind == "config") {
          operation = core::ControlReadPluginConfig{id};
        }
        if (request.method == "GET" && kind == "health") {
          operation = core::ControlHealth{id};
        }
        if (request.method == "PUT" && kind == "config") {
          if (!body.at("values").is_object()) {
            return failure(wire::INVALID_ARGUMENT, meta);
          }
          operation =
              core::ControlWritePluginConfig{id, body.at("expected_version").get<std::string>(),
                                             core::control_struct(body.at("values"))};
        }
      } else if (request.method == "GET" &&
                 std::regex_match(path, match, std::regex("/api/v1/actions/([^/]+)"))) {
        operation = core::ControlActionQuery{decode(match[1].str()), false};
      } else if (request.method == "GET") {
        static const std::map<std::string, core::ControlResource> lists{
            {"auth/sessions", core::ControlResource::Sessions},
            {"adapters", core::ControlResource::Adapters},
            {"plugins", core::ControlResource::Plugins},
            {"audit", core::ControlResource::Audit},
            {"bots", core::ControlResource::Bots},
            {"models", core::ControlResource::Models},
            {"groups", core::ControlResource::Groups},
            {"users", core::ControlResource::Users},
            {"tasks", core::ControlResource::Tasks},
            {"logs", core::ControlResource::Logs}};
        if (path.starts_with("/api/v1/") && lists.contains(path.substr(8))) {
          std::uint32_t size = 20;
          if (query.contains("page_size")) {
            const auto& value = query.at("page_size");
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
              return failure(wire::INVALID_ARGUMENT, meta);
            }
          }
          operation = core::ControlList{lists.at(path.substr(8)), size,
                                        query.contains("page_token") ? query.at("page_token") : ""};
        }
      }
    }
    if (!operation) {
      return failure(wire::NOT_FOUND, meta);
    }
    if ((!query.empty() && !std::holds_alternative<core::ControlList>(*operation)) ||
        (request.method == "GET" && !request.body.empty())) {
      return failure(wire::INVALID_ARGUMENT, meta);
    }
    const auto outcome = application.invoke({meta, *operation});
    if (const auto* error = std::get_if<wire::Error>(&outcome)) {
      return failure(error->code(), meta, error->retryable());
    }
    const auto& reply = std::get<core::ControlReply>(outcome);
    if (std::holds_alternative<core::ControlAccepted>(reply)) {
      return {202, "", {}};
    }
    if (std::holds_alternative<core::ControlNoContent>(reply)) {
      return {204, "", {}};
    }
    return {200, document(reply).dump(), {}};
  } catch (const Json::exception&) {
    return failure(wire::INVALID_ARGUMENT, meta);
  } catch (const std::invalid_argument&) {
    return failure(wire::INVALID_ARGUMENT, meta);
  } catch (...) {
    return failure(wire::INTERNAL, meta);
  }
}
} // namespace kiarabot::control
