// P12c Control owner-gateway boundary tests: Action query forwarding rules, stable error mapping
// and UNAVAILABLE (never a fabricated empty page) for log/audit sources not yet registered.
#include <chrono>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "kiarabot/control/target/owner_gateway.hpp"
#include "kiarabot/v1/actions.pb.h"

namespace {
namespace target = kiarabot::control::target;
namespace core = kiarabot::core;
namespace wire = kiarabot::v1;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error("P12c owner gateway assertion: " + std::string{message});
  }
}
std::string scope_json() {
  return R"({"resources":[{"kind":"RESOURCE_ACCOUNT","id":"11111111-1111-4111-8111-111111111111"}]})";
}
target::OwnerHttpRequest request(std::string method, std::string target_url) {
  target::OwnerHttpRequest value;
  value.method = std::move(method);
  value.target = std::move(target_url);
  value.headers["x-request-id"] = "request-123";
  value.access_token = "access-token";
  value.deadline =
      core::target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::system_clock::now().time_since_epoch()) +
                              std::chrono::seconds{2}};
  return value;
}
struct FakeDelegation final : target::DelegationPort {
  std::optional<target::OwnerFailure> failure;
  int calls = 0;
  std::string audience;
  wire::Permission permission = wire::PERMISSION_UNSPECIFIED;
  int scope_resources = 0;
  target::OwnerOutcome<std::string> issue(std::string_view audience_value,
                                          wire::Permission permission_value,
                                          const wire::ResourceScope& scope, std::string_view,
                                          core::target::TimePoint) override {
    ++calls;
    audience = std::string{audience_value};
    permission = permission_value;
    scope_resources = scope.resources_size();
    if (failure) {
      return *failure;
    }
    return std::string{"delegation-proof"};
  }
};
struct FakeRuntime final : target::RuntimeActionsPort {
  std::optional<target::OwnerFailure> failure;
  bool with_result = true;
  int calls = 0;
  wire::RuntimeQueryActionResultRequest last_request;
  target::OwnerOutcome<wire::RuntimeQueryActionResultResponse>
  query_action(const wire::RuntimeQueryActionResultRequest& value,
               std::string_view delegation) override {
    ++calls;
    last_request = value;
    require(delegation == "delegation-proof", "runtime receives issued delegation");
    if (failure) {
      return *failure;
    }
    wire::RuntimeQueryActionResultResponse response;
    if (with_result) {
      auto* result = response.mutable_result();
      result->set_action_id("action-1");
      result->set_status(wire::SUCCEEDED);
      result->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
      result->set_platform_message_id("platform-message-1");
    }
    return response;
  }
};
struct Gateway {
  FakeDelegation delegation;
  FakeRuntime runtime;
  target::OwnerGateway gateway{delegation, runtime};
};

std::string action_scope_path(std::string_view action = "action-1") {
  return "/api/v1/actions/" + std::string{action} + "?scope=" + scope_json();
}

void action_lookup_by_id() {
  Gateway fixture;
  auto result = fixture.gateway.handle(request("GET", action_scope_path()));
  if (!result || result->status != 200) {
    std::cerr << "action status=" << (result ? result->status : 999)
              << " body=" << (result ? result->body : "nullopt") << '\n';
  }
  require(result && result->status == 200, "action by id 200");
  require(result->body.find("\"action_id\":\"action-1\"") != std::string::npos, "action id mapped");
  require(result->body.find("\"status\":\"SUCCEEDED\"") != std::string::npos, "status mapped");
  require(result->body.find("\"execution_outcome\":\"EXECUTION_KNOWN_SUCCEEDED\"") !=
              std::string::npos,
          "execution outcome mapped");
  require(fixture.delegation.calls == 1 && fixture.delegation.audience == "runtime" &&
              fixture.delegation.permission == wire::PERMISSION_ACTION_INSPECT &&
              fixture.delegation.scope_resources == 1,
          "delegation requested for action.inspect + one scope");
  require(fixture.runtime.calls == 1 && fixture.runtime.last_request.action_id() == "action-1" &&
              fixture.runtime.last_request.has_account_scope(),
          "runtime queried by action id");
}

void idempotency_rules() {
  Gateway fixture;
  auto by_key = request("GET", "/api/v1/actions/by-idempotency-key?scope=" + scope_json());
  by_key.headers["idempotency-key"] = "key-1";
  auto missing_owner = fixture.gateway.handle(by_key);
  require(missing_owner && missing_owner->status == 400 && fixture.runtime.calls == 0 &&
              fixture.delegation.calls == 0,
          "idempotency query requires owner_instance before any owner call");
  by_key.target += "&owner_instance=instance-1";
  auto found = fixture.gateway.handle(by_key);
  require(found && found->status == 200, "idempotency lookup 200");
  require(fixture.runtime.last_request.idempotency_key() == "key-1" &&
              fixture.runtime.last_request.owner_instance_id() == "instance-1",
          "idempotency key + owner forwarded");
  Gateway without_key;
  auto no_key = without_key.gateway.handle(
      request("GET", "/api/v1/actions/by-idempotency-key?scope=" + scope_json() +
                         "&owner_instance=instance-1"));
  require(no_key && no_key->status == 400, "missing idempotency header rejected");
}

void error_mapping() {
  Gateway not_found;
  not_found.runtime.failure = target::OwnerFailure{wire::NOT_FOUND, false};
  auto missing = not_found.gateway.handle(request("GET", action_scope_path()));
  require(missing && missing->status == 404 &&
              missing->body.find("\"code\":\"NOT_FOUND\"") != std::string::npos,
          "owner NOT_FOUND maps to 404, not an execution failure");
  Gateway denied;
  denied.runtime.failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  auto forbidden = denied.gateway.handle(request("GET", action_scope_path()));
  require(forbidden && forbidden->status == 403, "permission denied maps to 403");
  Gateway no_delegation;
  no_delegation.delegation.failure = target::OwnerFailure{wire::UNAUTHENTICATED, false};
  auto unauthorized = no_delegation.gateway.handle(request("GET", action_scope_path()));
  require(unauthorized && unauthorized->status == 401 && no_delegation.runtime.calls == 0,
          "delegation failure stops before owner call");
  Gateway timeout;
  timeout.runtime.failure = target::OwnerFailure{wire::TIMEOUT, true};
  auto timed_out = timeout.gateway.handle(request("GET", action_scope_path()));
  require(timed_out && timed_out->status == 504, "owner timeout maps to 504");
}

void unavailable_sources_and_health() {
  Gateway fixture;
  const auto logs = fixture.gateway.handle(request("GET", "/api/v1/logs"));
  require(logs && logs->status == 503 &&
              logs->body.find("\"code\":\"UPSTREAM_UNAVAILABLE\"") != std::string::npos,
          "logs return UNAVAILABLE instead of an empty page");
  const auto audit = fixture.gateway.handle(request("GET", "/api/v1/audit?page_size=20"));
  require(audit && audit->status == 503, "audit returns UNAVAILABLE");
  require(fixture.delegation.calls == 0 && fixture.runtime.calls == 0,
          "unavailable sources never call owners");
  const auto live = fixture.gateway.handle(request("GET", "/health/live"));
  require(live && live->status == 200, "health live");
  const auto unknown = fixture.gateway.handle(request("GET", "/api/v1/tasks"));
  require(!unknown, "non-owner paths are left to other handlers");
}

void validation_rules() {
  Gateway fixture;
  auto no_scope = fixture.gateway.handle(request("GET", "/api/v1/actions/action-1"));
  require(no_scope && no_scope->status == 400, "missing scope rejected");
  auto bad_scope =
      fixture.gateway.handle(request("GET", "/api/v1/actions/action-1?scope=%7Bbad-json%7D"));
  require(bad_scope && bad_scope->status == 400, "malformed scope rejected");
  auto multi_scope = fixture.gateway.handle(request(
      "GET",
      "/api/v1/actions/"
      "action-1?scope=%7B%22resources%22%3A%5B%7B%22kind%22%3A%22RESOURCE_ACCOUNT%22%2C%22id%22%3A%"
      "2211111111-1111-4111-8111-111111111111%22%7D%2C%7B%22kind%22%3A%22RESOURCE_ACCOUNT%22%2C%"
      "22id%22%3A%2222222222-2222-4222-8222-222222222222%22%7D%5D%7D"));
  require(multi_scope && multi_scope->status == 400, "exactly one account scope required");
  auto wrong_method = fixture.gateway.handle(request("POST", action_scope_path()));
  require(wrong_method && wrong_method->status == 405, "non-GET rejected");
}

} // namespace

int main() {
  try {
    action_lookup_by_id();
    idempotency_rules();
    error_mapping();
    unavailable_sources_and_health();
    validation_rules();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "control owner gateway: ok\n";
  return 0;
}
