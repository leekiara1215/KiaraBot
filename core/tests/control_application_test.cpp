#include <iostream>
#include <stdexcept>

#include "kiarabot/core/application/control_application.hpp"

namespace core = kiarabot::core;
namespace wire = kiarabot::v1;
namespace {
void check(bool ok) {
  if (!ok) {
    throw std::runtime_error("Control application contract assertion");
  }
}
std::vector<core::ControlOperation> operations() {
  std::vector<core::ControlOperation> result{core::ControlLogin{},
                                             core::ControlRefresh{},
                                             core::ControlPrincipalQuery{},
                                             core::ControlRevokeSession{},
                                             core::ControlValidatePlugin{},
                                             core::ControlSetPluginEnabled{"plugin", true},
                                             core::ControlSetPluginEnabled{"plugin", false},
                                             core::ControlReadPluginConfig{},
                                             core::ControlWritePluginConfig{},
                                             core::ControlHealth{},
                                             core::ControlHealth{std::string("plugin")},
                                             core::ControlActionQuery{"id", false},
                                             core::ControlActionQuery{"key", true}};
  for (int i = 0; i <= static_cast<int>(core::ControlResource::Logs); ++i) {
    result.push_back(core::ControlList{static_cast<core::ControlResource>(i), 20, {}});
  }
  return result;
}
} // namespace
int main(int argc, char* argv[]) try {
  if (argc == 2 && std::string_view(argv[1]) == "--dump-policy") {
    for (const auto& operation : operations()) {
      const auto policy = core::control_policy(operation);
      std::cout << policy.operation_id << ' ' << policy.permission << ' ' << policy.mutating;
      for (const auto role : {"admin", "operator", "viewer"}) {
        std::cout << ' ' << core::control_role_allows(role, policy.permission);
      }
      std::cout << '\n';
    }
    return 0;
  }
  const auto now = core::WallTime(std::chrono::seconds(1000));
  core::ControlRequest request{
      {"token", "request", "trace", "idempotency-key-1", now + std::chrono::seconds(5)},
      core::ControlList{}};
  check(!core::validate_control_request(request, now));
  request.metadata.access_token.clear();
  check(core::validate_control_request(request, now)->code() == wire::UNAUTHENTICATED);
  request.operation = core::ControlLogin{"Admin", "password"};
  check(!core::validate_control_request(request, now));
  request.metadata.idempotency_key = "short";
  check(core::validate_control_request(request, now)->code() == wire::INVALID_ARGUMENT);
  request.metadata.idempotency_key = "idempotency-key-1";
  request.metadata.deadline = now;
  check(core::validate_control_request(request, now)->code() == wire::TIMEOUT);
  request.metadata.deadline = now + std::chrono::seconds(31);
  check(core::validate_control_request(request, now)->code() == wire::INVALID_ARGUMENT);
  request.metadata.deadline = now + std::chrono::seconds(5);
  request.metadata.access_token = "token";
  request.operation = core::ControlList{core::ControlResource::Plugins, 101, {}};
  check(core::validate_control_request(request, now)->code() == wire::INVALID_ARGUMENT);
  // Exercise rejection of an unknown resource value.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  request.operation = core::ControlList{static_cast<core::ControlResource>(999), 20, {}};
  check(core::validate_control_request(request, now)->code() == wire::INVALID_ARGUMENT);
  request.operation = core::ControlSetPluginEnabled{"../private", true};
  check(core::validate_control_request(request, now)->code() == wire::INVALID_ARGUMENT);
  check(core::control_role_allows("operator", "plugin.manage"));
  check(!core::control_role_allows("viewer", "plugin.manage"));
  check(!core::control_role_allows("unknown", "plugin.read"));
  check(!core::control_role_allows("admin", "unknown.permission"));
  check(!core::control_role_allows("admin", "message.read"));
  check(core::control_http_status(wire::TIMEOUT) == 504);
  check(core::control_http_status(wire::UPSTREAM_UNAVAILABLE) == 503);
  check(core::control_http_status(wire::CONFLICT) == 409);
  check(core::control_http_status(wire::PERMISSION_DENIED) == 403);
  // Exercise the fallback for an unknown wire error value.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  check(core::control_http_status(static_cast<wire::ErrorCode>(999)) == 500);
  std::cout
      << "Control operation coverage, default deny, request validation and HTTP errors passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
