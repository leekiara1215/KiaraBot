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
struct FakePrincipals final : target::IdentityPrincipalsPort {
  std::optional<target::OwnerFailure> failure;
  int calls = 0;
  wire::IdentityListPrincipalsRequest last_request;
  target::OwnerOutcome<wire::IdentityListPrincipalsResponse>
  list_principals(const wire::IdentityListPrincipalsRequest& value,
                  std::string_view delegation) override {
    ++calls;
    last_request = value;
    require(delegation == "delegation-proof", "principals receives issued delegation");
    if (failure) {
      return *failure;
    }
    wire::IdentityListPrincipalsResponse response;
    auto* principal = response.add_principals();
    principal->set_principal_id("11111111-1111-4111-8111-111111111111");
    principal->set_principal_kind("human");
    response.mutable_page()->set_page_size(value.page().page_size());
    return response;
  }
};
struct FakeHealth final : target::HealthPort {
  bool identity_ready = true;
  bool runtime_ready = true;
  bool scheduler_ready = true;
  bool model_ready = true;
  target::HealthObservation observe() override {
    target::HealthObservation value;
    value.identity_ready = identity_ready;
    value.runtime_ready = runtime_ready;
    value.scheduler_ready = scheduler_ready;
    value.model_ready = model_ready;
    value.runtime_configured = true;
    value.scheduler_configured = true;
    value.model_configured = true;
    value.checked_at =
        core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
            std::chrono::system_clock::now().time_since_epoch())};
    return value;
  }
};
struct Gateway {
  FakeDelegation delegation;
  FakeRuntime runtime;
  target::OwnerGateway gateway{delegation, runtime};
};
struct FakeResources final : target::RuntimeResourcesPort {
  std::optional<target::OwnerFailure> failure;
  int calls = 0;
  wire::RuntimeBindAdapterRequest last_bind;
  wire::RuntimeSetDesiredStateRequest last_state;
  wire::RuntimeUpdateActionPolicyRequest last_policy;
  template <typename Response> target::OwnerOutcome<Response> ok(Response response) const {
    if (failure) {
      return *failure;
    }
    return response;
  }
  target::OwnerOutcome<wire::RuntimeBindAdapterResponse>
  bind_adapter(const wire::RuntimeBindAdapterRequest& input, std::string_view) override {
    ++calls;
    last_bind = input;
    wire::RuntimeBindAdapterResponse response;
    *response.mutable_binding() = input.binding();
    response.mutable_operation()->set_operation_id("operation-1");
    response.set_bootstrap_credential("must-not-leak");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeUnbindAdapterResponse>
  unbind_adapter(const wire::RuntimeUnbindAdapterRequest& input, std::string_view) override {
    ++calls;
    wire::RuntimeUnbindAdapterResponse response;
    response.mutable_operation()->set_operation_id("operation-2");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeCreateInstallationResponse>
  create_installation(const wire::RuntimeCreateInstallationRequest& input,
                      std::string_view) override {
    ++calls;
    wire::RuntimeCreateInstallationResponse response;
    response.mutable_installation()->set_installation_id("installation-1");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeCreateInstanceResponse>
  create_instance(const wire::RuntimeCreateInstanceRequest& input, std::string_view) override {
    ++calls;
    wire::RuntimeCreateInstanceResponse response;
    response.mutable_instance()->set_instance_id("instance-1");
    response.set_bootstrap_credential("must-not-leak");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeSetDesiredStateResponse>
  set_desired_state(const wire::RuntimeSetDesiredStateRequest& input, std::string_view) override {
    ++calls;
    last_state = input;
    wire::RuntimeSetDesiredStateResponse response;
    response.mutable_instance()->set_instance_id(input.instance_id());
    response.mutable_operation()->set_operation_id("operation-3");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeUpdateConfigResponse>
  update_config(const wire::RuntimeUpdateConfigRequest& input, std::string_view) override {
    ++calls;
    return ok(wire::RuntimeUpdateConfigResponse{});
  }
  target::OwnerOutcome<wire::RuntimeGetInstanceConfigResponse>
  get_instance_config(const wire::RuntimeGetInstanceConfigRequest& input,
                      std::string_view) override {
    ++calls;
    return ok(wire::RuntimeGetInstanceConfigResponse{});
  }
  target::OwnerOutcome<wire::RuntimeGetActionPolicyResponse>
  get_action_policy(const wire::RuntimeGetActionPolicyRequest& input, std::string_view) override {
    ++calls;
    wire::RuntimeGetActionPolicyResponse response;
    response.mutable_policy()->set_account_id(input.account_id());
    response.mutable_policy()->set_version(4);
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeUpdateActionPolicyResponse>
  update_action_policy(const wire::RuntimeUpdateActionPolicyRequest& input,
                       std::string_view) override {
    ++calls;
    last_policy = input;
    wire::RuntimeUpdateActionPolicyResponse response;
    *response.mutable_policy() = input.policy();
    return ok(response);
  }
};
struct FakeOperations final : target::OperationPort {
  std::optional<target::OwnerFailure> failure;
  bool empty = false;
  int calls = 0;
  std::string owner;
  target::OwnerOutcome<wire::Operation> get(std::string_view owner_value,
                                            const wire::OperationRef& input,
                                            std::string_view delegation) override {
    ++calls;
    owner = std::string{owner_value};
    require(delegation == "delegation-proof", "operation receives delegation");
    if (failure) {
      return *failure;
    }
    wire::Operation operation;
    if (!empty) {
      operation.mutable_ref()->set_owner_service(std::string{owner_value});
      operation.mutable_ref()->set_operation_id(input.operation_id());
      operation.set_version(7);
      operation.set_state(wire::OPERATION_RUNNING);
      operation.set_execution_outcome(wire::EXECUTION_UNKNOWN);
      auto* participant = operation.add_participants();
      participant->set_participant("runtime");
      participant->set_state(wire::OPERATION_RUNNING);
      operation.mutable_observed_at()->set_seconds(1'700'000'000);
      operation.mutable_next_attempt_at()->set_seconds(1'700'000'030);
      operation.set_stalled(true);
    }
    return operation;
  }
};
struct FakeTasks final : target::SchedulerTasksPort {
  std::optional<target::OwnerFailure> failure;
  int calls = 0;
  target::OwnerOutcome<wire::SchedulerListTasksResponse>
  list_tasks(const wire::SchedulerListTasksRequest& input, std::string_view) override {
    ++calls;
    if (failure)
      return *failure;
    wire::SchedulerListTasksResponse response;
    response.add_tasks()->set_task_id("task-1");
    response.mutable_page()->set_page_size(input.page().page_size());
    return response;
  }
  target::OwnerOutcome<wire::SchedulerCancelTaskResponse>
  cancel_task(const wire::SchedulerCancelTaskRequest& input, std::string_view) override {
    ++calls;
    if (failure)
      return *failure;
    wire::SchedulerCancelTaskResponse response;
    response.mutable_task()->set_task_id(input.task_id());
    return response;
  }
  target::OwnerOutcome<wire::SchedulerGetTaskResponse>
  get_task(const wire::SchedulerGetTaskRequest& input, std::string_view) override {
    ++calls;
    if (failure)
      return *failure;
    wire::SchedulerGetTaskResponse response;
    response.mutable_task()->set_task_id(input.task_id());
    return response;
  }
};
struct OwnerCommands {
  FakeDelegation delegation;
  FakeRuntime runtime;
  FakePrincipals principals;
  FakeHealth health;
  FakeResources resources;
  FakeTasks tasks;
  FakeOperations operations;
  target::OwnerGateway gateway{delegation, runtime, &principals, &health,
                               &resources, &tasks,  &operations};
};
struct ExtendedGateway {
  FakeDelegation delegation;
  FakeRuntime runtime;
  FakePrincipals principals;
  FakeHealth health;
  target::OwnerGateway gateway{delegation, runtime, &principals, &health};
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
  require(fixture.delegation.calls >= 1 && fixture.runtime.calls == 0,
          "sensitive reads obtain a delegation but never call the action owner");
  const auto live = fixture.gateway.handle(request("GET", "/health/live"));
  require(live && live->status == 200, "health live");
  const auto unknown = fixture.gateway.handle(request("GET", "/api/v1/unknown"));
  require(!unknown, "non-owner paths are left to other handlers");
}

void health_aggregation() {
  ExtendedGateway fixture;
  auto ready = fixture.gateway.handle(request("GET", "/health/ready"));
  require(ready && ready->status == 200, "anonymous ready when Identity is reachable");
  auto healthy = fixture.gateway.handle(request("GET", "/api/v1/health"));
  require(healthy && healthy->status == 200 &&
              healthy->body.find("\"state\":\"HEALTHY\"") != std::string::npos,
          "authorized health healthy");
  fixture.health.runtime_ready = false;
  auto degraded = fixture.gateway.handle(request("GET", "/api/v1/health"));
  require(degraded && degraded->status == 200 &&
              degraded->body.find("\"state\":\"DEGRADED\"") != std::string::npos,
          "single owner outage degrades but keeps serving");
  fixture.health.identity_ready = false;
  auto unready = fixture.gateway.handle(request("GET", "/api/v1/health"));
  require(unready && unready->status == 503 &&
              unready->body.find("\"state\":\"UNHEALTHY\"") != std::string::npos,
          "Identity failure makes Control unready");
  auto anonymous = request("GET", "/api/v1/health");
  anonymous.access_token.clear();
  auto unauthorized = fixture.gateway.handle(anonymous);
  require(unauthorized && unauthorized->status == 401, "authorized health requires a caller");
}

void principals_catalog() {
  ExtendedGateway fixture;
  auto page =
      fixture.gateway.handle(request("GET", "/api/v1/principals?page_size=5&page_token=cursor-1"));
  require(page && page->status == 200, "principals page");
  require(page->body.find("\"principal_id\"") != std::string::npos &&
              page->body.find("human") != std::string::npos,
          "principal projection");
  require(fixture.principals.calls == 1 &&
              fixture.principals.last_request.page().page_size() == 5 &&
              fixture.principals.last_request.page().page_token() == "cursor-1" &&
              fixture.delegation.permission == wire::PERMISSION_IDENTITY_PRINCIPAL_READ,
          "page + identity.principal.read delegation");
  fixture.principals.failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  auto unavailable = fixture.gateway.handle(request("GET", "/api/v1/principals"));
  require(unavailable && unavailable->status == 503,
          "principal source unavailable is not an empty page");
  fixture.principals.failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  auto denied = fixture.gateway.handle(request("GET", "/api/v1/principals"));
  require(denied && denied->status == 403, "principal catalog denied");
}

void owner_commands() {
  OwnerCommands fixture;
  auto bind = request("PUT", "/api/v1/adapter-bindings/321d4c70-5c3d-4ac5-b7f6-2b871e21b1a1");
  bind.body =
      R"({"binding":{"account_id":"321d4c70-5c3d-4ac5-b7f6-2b871e21b1a1","adapter_instance_id":"adapter-1"},"expected_version":"2"})";
  auto bound = fixture.gateway.handle(bind);
  require(bound && bound->status == 200, "bind adapter response");
  require(bound->body.find("bootstrap_credential") == std::string::npos,
          "one-time bootstrap credential is never returned by REST");
  require(fixture.delegation.permission == wire::PERMISSION_ACCOUNT_MANAGE &&
              fixture.resources.last_bind.expected_version() == 2,
          "bind delegation/version");
  auto mismatch = bind;
  mismatch.body = R"({"binding":{"account_id":"another-account"},"expected_version":"2"})";
  require(fixture.gateway.handle(mismatch)->status == 400, "path/body account mismatch rejected");
  auto conflict_bind = bind;
  fixture.resources.failure = target::OwnerFailure{wire::CONFLICT, false};
  require(fixture.gateway.handle(conflict_bind)->status == 409, "version conflict maps 409");
  fixture.resources.failure.reset();

  auto state = request("PUT", "/api/v1/instances/inst-1/desired-state");
  state.body =
      R"({"instance_id":"inst-1","desired_state":"DESIRED_RUNNING","expected_version":"3"})";
  auto started = fixture.gateway.handle(state);
  require(started && started->status == 200 &&
              started->body.find("operation-3") != std::string::npos,
          "desired state returns persisted operation reference");
  require(fixture.delegation.permission == wire::PERMISSION_PLUGIN_MANAGE,
          "desired state plugin.manage delegation");
  auto policy = request("PUT", "/api/v1/accounts/account-1/action-policy");
  policy.body =
      R"({"policy":{"account_id":"account-1","mute_enabled":true,"kick_enabled":false,"allowed_group_ids":["group-1"],"mute_daily_budget":2,"kick_daily_budget":1,"version":"0"},"expected_version":"4"})";
  auto updated = fixture.gateway.handle(policy);
  require(updated && updated->status == 200 && updated->body.find("group-1") != std::string::npos,
          "action policy update forwards expected_version");
  require(fixture.resources.last_policy.policy().mute_enabled() &&
              fixture.resources.last_policy.expected_version() == 4 &&
              fixture.delegation.permission == wire::PERMISSION_ACCOUNT_POLICY_MANAGE,
          "action policy request/delegation");
  auto tasks = request("GET", "/api/v1/tasks?scope=" + scope_json() + "&page_size=5");
  auto listed = fixture.gateway.handle(tasks);
  require(listed && listed->status == 200 && listed->body.find("task-1") != std::string::npos,
          "task list forwards scope/page");
  require(listed->body.find("\"items\"") != std::string::npos &&
              listed->body.find("\"page\"") != std::string::npos,
          "task list uses OpenAPI TaskPage shape");
  auto cancel = request("POST", "/api/v1/tasks/task-1/cancel?scope=" + scope_json());
  auto cancelled = fixture.gateway.handle(cancel);
  require(cancelled && cancelled->status == 200 &&
              cancelled->body.find("task-1") != std::string::npos,
          "task cancel forwards owner");
  require(fixture.delegation.permission == wire::PERMISSION_TASK_SCHEDULE,
          "task cancel delegation");
  auto unavailable =
      request("GET", "/api/v1/tasks/" + std::string{"task-1"} + "?scope=" + scope_json());
  fixture.tasks.failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  require(fixture.gateway.handle(unavailable)->status == 503, "task owner unavailable maps 503");
}

void operation_snapshots() {
  OwnerCommands fixture;
  auto runtime = request("GET", "/api/v1/operations/runtime/operation-1?scope=" + scope_json());
  auto snapshot = fixture.gateway.handle(runtime);
  require(snapshot && snapshot->status == 200, "runtime operation snapshot");
  require(snapshot->body.find("\"state\":\"OPERATION_RUNNING\"") != std::string::npos &&
              snapshot->body.find("\"execution_outcome\":\"EXECUTION_UNKNOWN\"") !=
                  std::string::npos &&
              snapshot->body.find("\"participants\"") != std::string::npos &&
              snapshot->body.find("\"stalled\":true") != std::string::npos,
          "operation progress/unknown/stalled fields");
  require(snapshot->body.find("\"version\":\"7\"") != std::string::npos,
          "version is a JSON string");
  require(fixture.operations.owner == "runtime" &&
              fixture.delegation.permission == wire::PERMISSION_RESOURCE_READ,
          "runtime operation delegation");
  auto scheduler = fixture.gateway.handle(
      request("GET", "/api/v1/operations/scheduler/op-2?scope=" + scope_json()));
  require(scheduler && scheduler->status == 200 &&
              fixture.delegation.permission == wire::PERMISSION_TASK_READ,
          "scheduler operation uses task.read");
  auto model =
      fixture.gateway.handle(request("GET", "/api/v1/operations/model/op-3?scope=" + scope_json()));
  require(model && model->status == 200, "model operation snapshot");
  auto bad_owner = fixture.gateway.handle(
      request("GET", "/api/v1/operations/identity/op-4?scope=" + scope_json()));
  require(bad_owner && bad_owner->status == 400, "unknown owner rejected");
  auto no_scope = fixture.gateway.handle(request("GET", "/api/v1/operations/runtime/op-1"));
  require(no_scope && no_scope->status == 400, "operation query requires scope");
  fixture.operations.empty = true;
  auto missing = fixture.gateway.handle(
      request("GET", "/api/v1/operations/runtime/missing?scope=" + scope_json()));
  require(missing && missing->status == 404, "missing operation is NOT_FOUND, not completion");
  fixture.operations.empty = false;
  fixture.operations.failure = target::OwnerFailure{wire::CONFLICT, false};
  auto conflict = fixture.gateway.handle(
      request("GET", "/api/v1/operations/runtime/op-1?scope=" + scope_json()));
  require(conflict && conflict->status == 409, "owner conflict maps 409");
}

void sensitive_read_permissions() {
  Gateway fixture;
  fixture.delegation.failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  auto denied = fixture.gateway.handle(request("GET", "/api/v1/logs"));
  require(denied && denied->status == 403, "missing log.read denied");
  fixture.delegation.failure.reset();
  require(fixture.gateway.handle(request("GET", "/api/v1/logs"))->status == 503,
          "granted log.read still UNAVAILABLE without source");
  auto missing_token = request("GET", "/api/v1/audit");
  missing_token.access_token.clear();
  auto unauthorized = fixture.gateway.handle(missing_token);
  require(unauthorized && unauthorized->status == 401, "audit requires authentication");
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

struct FakeModel final : target::ModelManagementPort {
  std::optional<target::OwnerFailure> failure;
  std::string permission;
  int calls = 0;
  wire::ModelGetModelMetadataRequest metadata_request;
  wire::ModelListModelMetadataRequest list_request;
  wire::ModelUpdateModelRequest update_request;
  wire::ModelDisableModelRequest disable_request;
  wire::ModelEnableModelRequest enable_request;
  wire::ModelRotateProviderSecretRequest rotate_request;
  wire::ModelGetModelConfigurationRequest configuration_request;
  wire::ModelGetCallStatusRequest call_request;
  wire::ModelResolveModelCallRequest resolve_request;
  wire::ModelGetOperationRequest operation_request;
  wire::ModelMetadata metadata() {
    wire::ModelMetadata value;
    value.set_model_id("model-1");
    value.set_alias("default");
    value.set_config_version(2);
    value.set_state(wire::LIFECYCLE_ACTIVE);
    value.set_provider_kind("openai_chat_completions");
    return value;
  }
  target::OwnerOutcome<wire::ModelGetModelMetadataResponse>
  metadata(const wire::ModelGetModelMetadataRequest& request, std::string_view proof) override {
    ++calls;
    metadata_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelGetModelMetadataResponse response;
    *response.mutable_model() = metadata();
    return response;
  }
  target::OwnerOutcome<wire::ModelListModelMetadataResponse>
  list_metadata(const wire::ModelListModelMetadataRequest& request,
                std::string_view proof) override {
    ++calls;
    list_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelListModelMetadataResponse response;
    *response.add_models() = metadata();
    return response;
  }
  target::OwnerOutcome<wire::ModelUpdateModelResponse>
  update(const wire::ModelUpdateModelRequest& request, std::string_view proof) override {
    ++calls;
    update_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelUpdateModelResponse response;
    *response.mutable_model() = metadata();
    return response;
  }
  target::OwnerOutcome<wire::ModelDisableModelResponse>
  disable(const wire::ModelDisableModelRequest& request, std::string_view proof) override {
    ++calls;
    disable_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelDisableModelResponse response;
    *response.mutable_model() = metadata();
    return response;
  }
  target::OwnerOutcome<wire::ModelEnableModelResponse>
  enable(const wire::ModelEnableModelRequest& request, std::string_view proof) override {
    ++calls;
    enable_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelEnableModelResponse response;
    *response.mutable_model() = metadata();
    return response;
  }
  target::OwnerOutcome<wire::ModelRotateProviderSecretResponse>
  rotate_secret(const wire::ModelRotateProviderSecretRequest& request,
                std::string_view proof) override {
    ++calls;
    rotate_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelRotateProviderSecretResponse response;
    response.set_version(3);
    return response;
  }
  target::OwnerOutcome<wire::ModelGetModelConfigurationResponse>
  configuration(const wire::ModelGetModelConfigurationRequest& request,
                std::string_view proof) override {
    ++calls;
    configuration_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelGetModelConfigurationResponse response;
    auto* config = response.mutable_config();
    config->set_model_id("model-1");
    config->set_alias("default");
    config->set_provider_kind("openai_chat_completions");
    config->set_endpoint("https://api.example.com/v1");
    config->set_provider_model("gpt-test");
    config->set_timeout_ms(30000);
    config->set_version(2);
    response.set_secret_configured(true);
    response.set_secret_version(7);
    google::protobuf::Timestamp observed;
    observed.set_seconds(1'800'000'000);
    *response.mutable_observed_at() = observed;
    return response;
  }
  target::OwnerOutcome<wire::ModelGetCallStatusResponse>
  call_status(const wire::ModelGetCallStatusRequest& request, std::string_view proof) override {
    ++calls;
    call_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelGetCallStatusResponse response;
    auto* call = response.mutable_call();
    call->set_logical_call_id("call-1");
    call->set_model_id("model-1");
    call->set_config_version(2);
    call->set_state(wire::OPERATION_BLOCKED);
    call->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    call->set_usage_known(false);
    call->set_may_have_billed(true);
    call->set_disposition(wire::MODEL_CALL_DISPOSITION_UNSPECIFIED);
    google::protobuf::Timestamp observed;
    observed.set_seconds(1'800'000'000);
    *call->mutable_observed_at() = observed;
    return response;
  }
  target::OwnerOutcome<wire::ModelResolveModelCallResponse>
  resolve(const wire::ModelResolveModelCallRequest& request, std::string_view proof) override {
    ++calls;
    resolve_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelResolveModelCallResponse response;
    auto* call = response.mutable_call();
    call->set_logical_call_id("call-1");
    call->set_model_id("model-1");
    call->set_state(wire::OPERATION_SUCCEEDED);
    call->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    call->set_disposition(wire::EXECUTED_BILLED);
    return response;
  }
  target::OwnerOutcome<wire::ModelGetOperationResponse>
  operation(const wire::ModelGetOperationRequest& request, std::string_view proof) override {
    ++calls;
    operation_request = request;
    permission = proof;
    if (failure) {
      return *failure;
    }
    wire::ModelGetOperationResponse response;
    auto* operation = response.mutable_operation();
    auto* reference = operation->mutable_ref();
    reference->set_owner_service("model");
    reference->set_operation_id("operation-1");
    operation->set_version(1);
    operation->set_state(wire::OPERATION_SUCCEEDED);
    operation->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    google::protobuf::Timestamp observed;
    observed.set_seconds(1'800'000'000);
    *operation->mutable_observed_at() = observed;
    return response;
  }
};
std::string model_scope_json() {
  return R"({"resources":[{"kind":"RESOURCE_MODEL","id":"model-1"}]})";
}
std::string model_path(std::string_view suffix = {}) {
  return "/api/v1/models/model-1" + std::string{suffix} + "?scope=" + model_scope_json();
}
std::string model_scope_query() {
  return "?scope=" + model_scope_json();
}
void model_management_surface() {
  FakeDelegation delegation;
  FakeRuntime runtime;
  FakeModel model;
  FakeOperations operations;
  target::OwnerGateway gateway{delegation, runtime, nullptr,     nullptr,
                               nullptr,    nullptr, &operations, &model};
  auto update = request("PUT", "/api/v1/models/model-1");
  update.headers["idempotency-key"] = "model-idem-1";
  update.body =
      R"({"config":{"model_id":"model-1","alias":"default","provider_kind":"openai_chat_completions","endpoint":"https://api.example.com/v1","provider_model":"gpt-test","timeout_ms":30000,"version":"2"},"expected_version":"2"})";
  auto updated = gateway.handle(update);
  require(updated && updated->status == 200, "model update 200");
  require(updated->body.find("\"model_id\":\"model-1\"") != std::string::npos &&
              updated->body.find("\"state\":\"LIFECYCLE_ACTIVE\"") != std::string::npos,
          "model metadata projected");
  require(delegation.calls == 1 && delegation.audience == "model" &&
              delegation.permission == wire::PERMISSION_MODEL_MANAGE &&
              delegation.scope_resources == 1,
          "update delegation is model.manage + single scope");
  require(model.update_request.config().model_id() == "model-1",
          "model id forwarded from path/body");

  auto metadata = request("GET", model_path());
  auto metadata_result = gateway.handle(metadata);
  require(metadata_result && metadata_result->status == 200 &&
              metadata_result->body.find("\"model_id\":\"model-1\"") != std::string::npos &&
              metadata_result->body.find("\"provider_kind\":\"openai_chat_completions\"") !=
                  std::string::npos,
          "model metadata forwarded");
  require(delegation.permission == wire::PERMISSION_MODEL_READ,
          "metadata delegation is model.read");
  auto listed =
      gateway.handle(request("GET", "/api/v1/models" + model_scope_query() + "&page_size=1"));
  require(listed && listed->status == 200 && listed->body.find("\"models\"") != std::string::npos &&
              model.list_request.scope().resources_size() == 1,
          "model metadata list forwarded");
  auto runtime_operation = gateway.handle(
      request("GET", "/api/v1/operations/runtime/operation-1?scope=" + model_scope_json()));
  require(runtime_operation && runtime_operation->status == 200 &&
              runtime_operation->body.find("\"state\":\"OPERATION_RUNNING\"") !=
                  std::string::npos &&
              operations.owner == "runtime",
          "P12c-4 unified operation branch routes runtime through OperationPort");

  auto disable = request("POST", "/api/v1/models/model-1/disable");
  disable.headers["idempotency-key"] = "model-idem-2";
  disable.body = R"({"expected_version":"2"})";
  auto disabled = gateway.handle(disable);
  require(disabled && disabled->status == 200 && model.disable_request.expected_version() == 2 &&
              disabled->body.find("\"model\"") != std::string::npos,
          "disable forwards expected_version");
  auto enable = request("POST", "/api/v1/models/model-1/enable");
  enable.headers["idempotency-key"] = "model-idem-3";
  enable.body = R"({"expected_version":"3"})";
  auto enabled = gateway.handle(enable);
  require(enabled && enabled->status == 200 && model.enable_request.expected_version() == 3,
          "enable forwards expected_version");

  auto secret = request("PUT", "/api/v1/models/model-1/secret");
  secret.headers["idempotency-key"] = "model-idem-4";
  secret.body = R"({"model_id":"model-1","new_secret":"c2VjcmV0","expected_version":"2"})";
  auto rotated = gateway.handle(secret);
  require(rotated && rotated->status == 200 &&
              rotated->body.find("\"version\":\"3\"") != std::string::npos,
          "secret rotation version projected");
  require(rotated->body.find("new_secret") == std::string::npos &&
              rotated->body.find("c2VjcmV0") == std::string::npos,
          "secret is never echoed");
  require(model.rotate_request.new_secret() == "secret", "secret forwarded to owner only");

  auto configuration = request("GET", model_path("/configuration"));
  auto configured = gateway.handle(configuration);
  require(configured && configured->status == 200 &&
              configured->body.find("\"secret_configured\":true") != std::string::npos &&
              configured->body.find("\"secret_version\":\"7\"") != std::string::npos &&
              configured->body.find("\"observed_at\"") != std::string::npos,
          "configuration reports presence only");
  require(configured->body.find("secret") == configured->body.find("secret_configured"),
          "configuration never contains the secret value");

  auto status = request("GET", model_path("/calls/call-1"));
  auto call = gateway.handle(status);
  require(call && call->status == 200 &&
              call->body.find("\"execution_outcome\":\"EXECUTION_UNKNOWN\"") != std::string::npos &&
              call->body.find("\"may_have_billed\":true") != std::string::npos,
          "UNKNOWN call status is queryable, not inferred from transport");
  require(delegation.permission == wire::PERMISSION_MODEL_CALL_INSPECT,
          "call status delegation is model.call.inspect");

  auto resolve = request("POST", model_path("/calls/call-1/resolve"));
  resolve.headers["idempotency-key"] = "model-idem-5";
  resolve.body = R"({"disposition":"EXECUTED_BILLED","expected_version":"2"})";
  auto resolved = gateway.handle(resolve);
  require(resolved && resolved->status == 200 &&
              resolved->body.find("\"disposition\":\"EXECUTED_BILLED\"") != std::string::npos,
          "resolve disposition projected");

  auto operation_request =
      request("GET", "/api/v1/operations/model/operation-1" + model_scope_query());
  auto operation = gateway.handle(operation_request);
  require(operation && operation->status == 200 &&
              operation->body.find("\"state\":\"OPERATION_RUNNING\"") != std::string::npos,
          "model get operation forwarded");
  require(delegation.permission == wire::PERMISSION_RESOURCE_READ,
          "operation delegation is resource.read");

  FakeModel failing;
  failing.failure = target::OwnerFailure{wire::CONFLICT, false};
  target::OwnerGateway conflicted{delegation, runtime, nullptr, nullptr,
                                  nullptr,    nullptr, nullptr, &failing};
  auto conflict = request("PUT", "/api/v1/models/model-1");
  conflict.headers["idempotency-key"] = "model-idem-6";
  conflict.body =
      R"({"config":{"model_id":"model-1","alias":"default","provider_kind":"openai_chat_completions","endpoint":"https://api.example.com/v1","provider_model":"gpt-test"},"expected_version":"1"})";
  auto version_conflict = conflicted.handle(conflict);
  require(version_conflict && version_conflict->status == 409 &&
              version_conflict->body.find("\"code\":\"CONFLICT\"") != std::string::npos,
          "model version conflict maps to 409");

  target::OwnerGateway unavailable{delegation, runtime};
  auto missing = unavailable.handle(request("GET", model_path("/configuration")));
  require(missing && missing->status == 503 &&
              missing->body.find("\"code\":\"CAPABILITY_NOT_SUPPORTED\"") != std::string::npos,
          "model routes without an injected owner stay explicitly unavailable");
}

} // namespace

int main() {
  try {
    action_lookup_by_id();
    idempotency_rules();
    error_mapping();
    unavailable_sources_and_health();
    health_aggregation();
    principals_catalog();
    sensitive_read_permissions();
    owner_commands();
    operation_snapshots();
    validation_rules();
    model_management_surface();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "control owner gateway: ok\n";
  return 0;
}
