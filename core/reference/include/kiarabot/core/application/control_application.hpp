#pragma once

#include <array>

#include <nlohmann/json.hpp>

#include "kiarabot/core/application/plugin_config_delivery.hpp"
#include "kiarabot/core/ports/ports.hpp"

namespace kiarabot::core {
// Public JSON projection of control resources. The transport never sees domain types.
using ControlJson = nlohmann::json;
// In-process port v1. Never accept a caller-supplied role or user identity.
// The implementation validates credentials and current session for every invocation.
struct ControlMetadata {
  std::string access_token;
  std::string request_id;
  std::string trace_id;
  std::string idempotency_key;
  WallTime deadline{};
};
struct ControlLogin {
  std::string username;
  std::string password;
};
struct ControlRefresh {
  std::string refresh_token;
};
struct ControlPrincipalQuery {};
struct ControlRevokeSession {
  std::string session_id;
};
enum class ControlResource {
  Sessions,
  Adapters,
  Plugins,
  Audit,
  Bots,
  Models,
  Groups,
  Users,
  Tasks,
  Logs
};
struct ControlList {
  ControlResource resource{ControlResource::Plugins};
  std::uint32_t page_size{20};
  std::string page_token;
};
struct ControlValidatePlugin {
  kiarabot::v1::PluginManifest manifest;
};
struct ControlSetPluginEnabled {
  std::string plugin_id;
  bool enabled{false};
};
struct ControlReadPluginConfig {
  std::string plugin_id;
};
struct ControlWritePluginConfig {
  std::string plugin_id;
  std::string expected_version;
  google::protobuf::Struct values;
};
struct ControlHealth {
  std::optional<std::string> plugin_id;
};
struct ControlActionQuery {
  std::string selector;
  bool by_idempotency_key{false};
};
using ControlOperation = std::variant<ControlLogin, ControlRefresh, ControlPrincipalQuery,
                                      ControlRevokeSession, ControlList, ControlValidatePlugin,
                                      ControlSetPluginEnabled, ControlReadPluginConfig,
                                      ControlWritePluginConfig, ControlHealth, ControlActionQuery>;
struct ControlRequest {
  ControlMetadata metadata;
  ControlOperation operation;
};

struct ControlTokenPair {
  std::string access_token;
  std::string refresh_token;
  std::uint32_t expires_in{900};
};
struct ControlPrincipal {
  std::string user_id;
  std::string session_id;
  std::vector<std::string> roles;
  std::vector<std::string> permissions;
};
// Public resource documents only; implementations must omit secrets before returning.
// HTTP mapping validates each document against its canonical OpenAPI resource schema.
struct ControlPage {
  ControlResource resource{ControlResource::Plugins};
  std::vector<google::protobuf::Struct> items;
  std::uint32_t page_size{20};
  std::optional<std::string> next_page_token;
};
struct ControlPluginConfig {
  std::string version;
  google::protobuf::Struct values;
  std::optional<ConfigDeliveryStatus> delivery{};
};
struct ControlValidation {
  bool valid{false};
  std::vector<std::string> errors;
};
struct ControlHealthResult {
  kiarabot::v1::HealthState state{kiarabot::v1::UNHEALTHY};
  struct Component {
    std::string component;
    kiarabot::v1::HealthState state;
    std::string reason;
  };
  std::vector<Component> components;
};
struct ControlAccepted {};  // Durable command acceptance, not completion; HTTP 202.
struct ControlNoContent {}; // HTTP 204.
using ControlReply = std::variant<ControlTokenPair, ControlPrincipal, ControlPage,
                                  ControlPluginConfig, ControlValidation, ControlHealthResult,
                                  kiarabot::v1::ActionResult, ControlAccepted, ControlNoContent>;
class IControlApplication {
public:
  virtual ~IControlApplication() = default;
  virtual PortResult<ControlReply> invoke(const ControlRequest& request) = 0;
  // Readiness probe for /health/ready: only a writable store may report ready.
  // Implementations without readiness support keep the pre-existing 503 semantics.
  virtual PortStatus readiness() {
    kiarabot::v1::Error error;
    error.set_code(kiarabot::v1::UPSTREAM_UNAVAILABLE);
    error.set_retryable(true);
    return error;
  }
  // Authenticated snapshot used by the event stream. The default reports the capability
  // as unsupported instead of exposing a concrete implementation to the transport.
  virtual PortResult<ControlJson> stream_snapshot(const ControlMetadata& /*metadata*/) {
    kiarabot::v1::Error error;
    error.set_code(kiarabot::v1::CAPABILITY_NOT_SUPPORTED);
    return error;
  }
};

struct ControlOperationPolicy {
  std::string_view operation_id;
  std::string_view permission;
  bool mutating{false};
};
ControlOperationPolicy control_policy(const ControlOperation& operation);
// These helpers enforce the frozen shape/default roles, not credential verification.
bool control_role_allows(std::string_view role, std::string_view permission);
std::optional<kiarabot::v1::Error> validate_control_request(const ControlRequest& request,
                                                            WallTime now);
int control_http_status(kiarabot::v1::ErrorCode code);
} // namespace kiarabot::core
