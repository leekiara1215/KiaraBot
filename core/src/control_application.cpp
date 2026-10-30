#include "kiarabot/core/control_application.hpp"

namespace kiarabot::core {
namespace {
constexpr std::array<std::string_view, 10> kViewer{
    "session.read", "session.revoke", "health.read", "adapter.read", "plugin.read",
    "bot.read",     "model.read",     "group.read",  "task.read",    "action.read"};
constexpr std::array<std::string_view, 4> kOperator{"plugin.manage", "plugin.config.read",
                                                    "plugin.config.write", "log.read"};
bool contains(auto values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}
bool bounded(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum;
}
bool plugin_id(std::string_view value) {
  return value.size() >= 2 && value.size() <= 64 &&
         ((value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= '0' && value.front() <= '9')) &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
         });
}
} // namespace
ControlOperationPolicy control_policy(const ControlOperation& operation) {
  return std::visit(
      [](const auto& command) -> ControlOperationPolicy {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, ControlLogin>) {
          return {"login", "public", true};
        } else if constexpr (std::is_same_v<T, ControlRefresh>) {
          return {"refreshToken", "refresh.credential", true};
        } else if constexpr (std::is_same_v<T, ControlPrincipalQuery>) {
          return {"getCurrentPrincipal", "session.read"};
        } else if constexpr (std::is_same_v<T, ControlRevokeSession>) {
          return {"revokeSession", "session.revoke", true};
        } else if constexpr (std::is_same_v<T, ControlList>) {
          switch (command.resource) {
          case ControlResource::Sessions:
            return {"listSessions", "session.read"};
          case ControlResource::Adapters:
            return {"listAdapters", "adapter.read"};
          case ControlResource::Plugins:
            return {"listPlugins", "plugin.read"};
          case ControlResource::Audit:
            return {"listAuditEntries", "audit.read"};
          case ControlResource::Bots:
            return {"listBots", "bot.read"};
          case ControlResource::Models:
            return {"listModels", "model.read"};
          case ControlResource::Groups:
            return {"listGroups", "group.read"};
          case ControlResource::Users:
            return {"listUsers", "user.read"};
          case ControlResource::Tasks:
            return {"listTasks", "task.read"};
          case ControlResource::Logs:
            return {"listLogs", "log.read"};
          }
          return {};
        } else if constexpr (std::is_same_v<T, ControlValidatePlugin>) {
          return {"validatePlugin", "plugin.manage", true};
        } else if constexpr (std::is_same_v<T, ControlSetPluginEnabled>) {
          return {command.enabled ? "enablePlugin" : "disablePlugin", "plugin.manage", true};
        } else if constexpr (std::is_same_v<T, ControlReadPluginConfig>) {
          return {"getPluginConfig", "plugin.config.read"};
        } else if constexpr (std::is_same_v<T, ControlWritePluginConfig>) {
          return {"updatePluginConfig", "plugin.config.write", true};
        } else if constexpr (std::is_same_v<T, ControlHealth>) {
          return command.plugin_id ? ControlOperationPolicy{"getPluginHealth", "plugin.read"}
                                   : ControlOperationPolicy{"getHealth", "health.read"};
        } else {
          return {command.by_idempotency_key ? "getActionByIdempotencyKey" : "getActionResult",
                  "action.read"};
        }
      },
      operation);
}
bool control_role_allows(std::string_view role, std::string_view permission) {
  if (permission.empty()) {
    return false;
  }
  const bool viewer = contains(kViewer, permission);
  const bool elevated = contains(kOperator, permission);
  if (role == "admin") {
    return viewer || elevated || permission == "audit.read" || permission == "user.read";
  }
  if (role == "operator") {
    return viewer || elevated;
  }
  return role == "viewer" && viewer;
}
std::optional<kiarabot::v1::Error> validate_control_request(const ControlRequest& request,
                                                            WallTime now) {
  namespace wire = kiarabot::v1;
  auto reject = [&](wire::ErrorCode code) {
    wire::Error result;
    result.set_code(code);
    result.set_message(wire::ErrorCode_Name(code));
    result.set_request_id(request.metadata.request_id);
    result.set_trace_id(request.metadata.trace_id);
    result.set_retryable(code == wire::TIMEOUT);
    return std::optional<wire::Error>(result);
  };
  const auto& meta = request.metadata;
  const auto policy = control_policy(request.operation);
  if (!bounded(meta.request_id, 256) || !bounded(meta.trace_id, 256) ||
      policy.operation_id.empty() || meta.deadline > now + std::chrono::seconds(30) ||
      (policy.mutating &&
       (meta.idempotency_key.size() < 16 || meta.idempotency_key.size() > 128))) {
    return reject(wire::INVALID_ARGUMENT);
  }
  if (meta.deadline <= now) {
    return reject(wire::TIMEOUT);
  }
  if (!std::holds_alternative<ControlLogin>(request.operation) &&
      !std::holds_alternative<ControlRefresh>(request.operation) &&
      !bounded(meta.access_token, 4096)) {
    return reject(wire::UNAUTHENTICATED);
  }
  const bool valid = std::visit(
      [](const auto& command) {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, ControlLogin>) {
          return bounded(command.username, 128) && bounded(command.password, 1024) &&
                 std::all_of(command.username.begin(), command.username.end(), [](unsigned char c) {
                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                 });
        } else if constexpr (std::is_same_v<T, ControlRefresh>) {
          return command.refresh_token.size() >= 20 && command.refresh_token.size() <= 4096;
        } else if constexpr (std::is_same_v<T, ControlList>) {
          return command.page_size >= 1 && command.page_size <= 100 &&
                 command.page_token.size() <= 1024;
        } else if constexpr (std::is_same_v<T, ControlRevokeSession>) {
          return bounded(command.session_id, 256);
        } else if constexpr (std::is_same_v<T, ControlActionQuery>) {
          return bounded(command.selector, 256);
        } else if constexpr (std::is_same_v<T, ControlWritePluginConfig>) {
          return plugin_id(command.plugin_id) && bounded(command.expected_version, 128) &&
                 command.values.ByteSizeLong() <= 65536;
        } else if constexpr (std::is_same_v<T, ControlReadPluginConfig> ||
                             std::is_same_v<T, ControlSetPluginEnabled>) {
          return plugin_id(command.plugin_id);
        } else if constexpr (std::is_same_v<T, ControlHealth>) {
          return !command.plugin_id || plugin_id(*command.plugin_id);
        } else if constexpr (std::is_same_v<T, ControlValidatePlugin>) {
          return bounded(command.manifest.id(), 64) && command.manifest.ByteSizeLong() <= 65536;
        } else {
          return true;
        }
      },
      request.operation);
  return valid ? std::nullopt : reject(wire::INVALID_ARGUMENT);
}
int control_http_status(kiarabot::v1::ErrorCode code) {
  namespace wire = kiarabot::v1;
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
  case wire::UPSTREAM_UNAVAILABLE:
  case wire::PLUGIN_UNAVAILABLE:
  case wire::CAPABILITY_NOT_SUPPORTED:
    return 503;
  case wire::TIMEOUT:
    return 504;
  default:
    return 500;
  }
}
} // namespace kiarabot::core
