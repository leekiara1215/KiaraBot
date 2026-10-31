#include "kiarabot/control/http_mapper.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <regex>

#include <google/protobuf/util/json_util.h>

#include "kiarabot/core/application/control_service.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"

namespace kiarabot::control {
namespace {
using Json = core::ControlJson;
namespace wire = kiarabot::v1;

std::string lower(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string header(const HttpRequest& request, const std::string& key) {
  const auto found = request.headers.find(key);
  return found == request.headers.end() ? "" : found->second;
}

// Request/trace identifiers are short, printable and safe to echo in a response header.
bool correlation_identifier(std::string_view value) {
  if (value.size() < 8 || value.size() > 128) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '-' || ch == '_' || ch == ':';
  });
}

bool hex_digits(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || (ch >= '0' && ch <= '9');
  });
}

// W3C traceparent: 00-<32 hex trace-id>-<16 hex parent-id>-<2 hex flags>.
std::string inbound_trace_id(const HttpRequest& request) {
  const auto traceparent = lower(header(request, "traceparent"));
  if (traceparent.size() == 55 && traceparent.substr(0, 3) == "00-" && traceparent[35] == '-' &&
      traceparent[52] == '-' && hex_digits(traceparent.substr(3, 32)) &&
      hex_digits(traceparent.substr(36, 16)) && hex_digits(traceparent.substr(53, 2)) &&
      traceparent.substr(3, 32) != std::string(32, '0')) {
    return traceparent.substr(3, 32);
  }
  const auto fallback = header(request, "x-trace-id");
  return correlation_identifier(fallback) ? fallback : std::string();
}

core::ControlMetadata metadata(const HttpRequest& request) {
  core::ControlMetadata meta;
  const auto request_id = header(request, "x-request-id");
  meta.request_id = correlation_identifier(request_id) ? request_id : core::random_instance_id();
  const auto trace_id = inbound_trace_id(request);
  meta.trace_id = trace_id.empty() ? core::random_instance_id() : trace_id;
  meta.deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
  meta.idempotency_key = header(request, "idempotency-key");
  const auto auth = header(request, "authorization");
  if (auth.size() > 7 && lower(auth.substr(0, 7)) == "bearer ") {
    meta.access_token = auth.substr(7);
  }
  return meta;
}

HttpReply failure(wire::ErrorCode code, const core::ControlMetadata& meta, bool retryable = false) {
  std::map<std::string, std::string> headers;
  if (code == wire::RATE_LIMITED) {
    headers.emplace("Retry-After", "60");
  }
  if (code == wire::UNAUTHENTICATED) {
    headers.emplace("WWW-Authenticate", "Bearer");
  }
  return {core::control_http_status(code),
          Json{{"code", wire::ErrorCode_Name(code)},
               {"message", wire::ErrorCode_Name(code)},
               {"request_id", meta.request_id},
               {"trace_id", meta.trace_id},
               {"retryable", retryable},
               {"details", Json::object()}}
              .dump(),
          std::move(headers)};
}

std::string join(const std::vector<std::string>& values) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) {
      result += ", ";
    }
    result += value;
  }
  return result;
}

HttpReply method_not_allowed(const core::ControlMetadata& meta,
                             const std::vector<std::string>& methods) {
  auto allowed = methods;
  allowed.emplace_back("OPTIONS");
  auto reply = failure(wire::INVALID_ARGUMENT, meta);
  reply.status = 405;
  reply.headers.emplace("Allow", join(allowed));
  return reply;
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

bool json_content_type(const std::string& value) {
  const auto separator = value.find(';');
  auto type = lower(value.substr(0, separator));
  while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
    type.pop_back();
  }
  return type == "application/json";
}

// Transport-independent route table used for 405/OPTIONS semantics. Empty means unknown route.
std::vector<std::string> allowed_methods(std::string_view path) {
  if (path == "/health/live" || path == "/health/ready") {
    return {"GET", "HEAD"};
  }
  if (!path.starts_with("/api/v1/")) {
    return {};
  }
  const auto tail = path.substr(8);
  static const std::vector<std::string> get_head{"GET", "HEAD"};
  static const std::vector<std::string> post{"POST"};
  static const std::vector<std::string> put{"PUT"};
  static const std::vector<std::string> get_put{"GET", "HEAD", "PUT"};
  if (tail == "auth/login" || tail == "auth/refresh" || tail == "plugins/validate") {
    return post;
  }
  if (tail == "auth/me" || tail == "auth/sessions" || tail == "health" || tail == "adapters" ||
      tail == "plugins" || tail == "audit" || tail == "bots" || tail == "models" ||
      tail == "groups" || tail == "users" || tail == "tasks" || tail == "logs" ||
      tail == "actions/by-idempotency-key") {
    return get_head;
  }
  std::smatch match;
  const std::string value(path);
  if (std::regex_match(value, match, std::regex("/api/v1/auth/sessions/([^/]+)/revoke"))) {
    return post;
  }
  if (std::regex_match(value, match,
                       std::regex("/api/v1/plugins/([^/]+)/(enable|disable|config|health)"))) {
    const auto kind = match[2].str();
    if (kind == "config") {
      return get_put;
    }
    return kind == "health" ? get_head : post;
  }
  if (std::regex_match(value, match, std::regex("/api/v1/actions/([^/]+)"))) {
    return get_head;
  }
  return {};
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

HttpReply dispatch(core::IControlApplication& application, const HttpRequest& request,
                   const core::ControlMetadata& meta) {
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
    const bool head = request.method == "HEAD";
    const auto methods = allowed_methods(path);
    if (request.method == "OPTIONS") {
      if (!query.empty() || !request.body.empty()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      if (methods.empty()) {
        return failure(wire::NOT_FOUND, meta);
      }
      auto allowed = methods;
      allowed.emplace_back("OPTIONS");
      return {204, "", {{"Allow", join(allowed)}}};
    }
    const std::string method = head ? "GET" : request.method;
    if (path == "/health/live" || path == "/health/ready") {
      if (method != "GET" || !query.empty() || !request.body.empty()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      if (path == "/health/ready") {
        const auto result = application.readiness();
        if (const auto* error = std::get_if<wire::Error>(&result)) {
          return failure(error->code(), meta, true);
        }
      }
      return {200, R"({"state":"HEALTHY"})", {}};
    }
    std::optional<core::ControlOperation> operation;
    Json body = Json::object();
    if (!request.body.empty()) {
      if (!json_content_type(header(request, "content-type"))) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      body = Json::parse(request.body);
      if (!body.is_object()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
    }
    if (method == "POST" && path == "/api/v1/auth/login") {
      operation = core::ControlLogin{body.at("username").get<std::string>(),
                                     body.at("password").get<std::string>()};
    } else if (method == "POST" && path == "/api/v1/auth/refresh") {
      operation = core::ControlRefresh{body.at("refresh_token").get<std::string>()};
    } else if (method == "GET" && path == "/api/v1/auth/me") {
      operation = core::ControlPrincipalQuery{};
    } else if (method == "GET" && path == "/api/v1/health") {
      operation = core::ControlHealth{};
    } else if (method == "POST" && path == "/api/v1/plugins/validate") {
      wire::PluginManifest manifest;
      if (!google::protobuf::util::JsonStringToMessage(body.dump(), &manifest).ok()) {
        return failure(wire::INVALID_ARGUMENT, meta);
      }
      operation = core::ControlValidatePlugin{manifest};
    } else if (method == "GET" && path == "/api/v1/actions/by-idempotency-key") {
      operation = core::ControlActionQuery{meta.idempotency_key, true};
    } else {
      std::smatch match;
      if (method == "POST" &&
          std::regex_match(path, match, std::regex("/api/v1/auth/sessions/([^/]+)/revoke"))) {
        operation = core::ControlRevokeSession{decode(match[1].str())};
      } else if (std::regex_match(
                     path, match,
                     std::regex("/api/v1/plugins/([^/]+)/(enable|disable|config|health)"))) {
        const auto id = decode(match[1].str());
        const auto kind = match[2].str();
        if (method == "POST" && (kind == "enable" || kind == "disable")) {
          operation = core::ControlSetPluginEnabled{id, kind == "enable"};
        }
        if (method == "GET" && kind == "config") {
          operation = core::ControlReadPluginConfig{id};
        }
        if (method == "GET" && kind == "health") {
          operation = core::ControlHealth{id};
        }
        if (method == "PUT" && kind == "config") {
          if (!body.at("values").is_object()) {
            return failure(wire::INVALID_ARGUMENT, meta);
          }
          operation =
              core::ControlWritePluginConfig{id, body.at("expected_version").get<std::string>(),
                                             core::control_struct(body.at("values"))};
        }
      } else if (method == "GET" &&
                 std::regex_match(path, match, std::regex("/api/v1/actions/([^/]+)"))) {
        operation = core::ControlActionQuery{decode(match[1].str()), false};
      } else if (method == "GET") {
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
      return methods.empty() ? failure(wire::NOT_FOUND, meta) : method_not_allowed(meta, methods);
    }
    if ((!query.empty() && !std::holds_alternative<core::ControlList>(*operation)) ||
        (method == "GET" && !request.body.empty())) {
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
} // namespace

HttpReply handle_http(core::IControlApplication& application, const HttpRequest& request) {
  const auto meta = metadata(request);
  auto reply = dispatch(application, request, meta);
  reply.headers["X-Request-ID"] = meta.request_id;
  reply.headers["X-Trace-ID"] = meta.trace_id;
  return reply;
}
} // namespace kiarabot::control
