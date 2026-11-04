// P12c Control owner-gateway boundary tests: Action query forwarding rules, stable error mapping
// and UNAVAILABLE (never a fabricated empty page) for log/audit sources not yet registered.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
core::target::TimePoint timepoint(std::int64_t nanos) {
  return core::target::TimePoint{std::chrono::nanoseconds{nanos}};
}
std::string scope_json() {
  return R"({"resources":[{"kind":"RESOURCE_ACCOUNT","id":"11111111-1111-4111-8111-111111111111"}]})";
}
target::OwnerHttpRequest request(std::string method, std::string target_url) {
  target::OwnerHttpRequest value;
  value.method = std::move(method);
  if (value.method != "GET" && value.method != "HEAD") {
    value.headers["idempotency-key"] = "fixture-write-key-0001";
  }
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
  wire::ResourceScope last_scope;
  std::optional<target::OwnerFailure> catalog_failure;
  std::optional<wire::ResourceScope> selected;
  wire::ResourceRef last_catalog;
  wire::ResourceKind child_kind = wire::RESOURCE_KIND_UNSPECIFIED;
  target::OwnerOutcome<wire::ResourceScope>
  catalog_scope(wire::Permission, const wire::ResourceRef& catalog, wire::ResourceKind kind,
                const std::optional<wire::ResourceScope>& selection, std::string_view,
                core::target::TimePoint) override {
    last_catalog = catalog;
    child_kind = kind;
    selected = selection;
    if (catalog_failure) {
      return *catalog_failure;
    }
    wire::ResourceScope result;
    *result.add_resources() = catalog;
    if (selection) {
      for (const auto& resource : selection->resources()) {
        *result.add_resources() = resource;
      }
    }
    return result;
  }
  target::OwnerOutcome<std::string> issue(std::string_view audience_value,
                                          wire::Permission permission_value,
                                          const wire::ResourceScope& scope, std::string_view,
                                          core::target::TimePoint) override {
    ++calls;
    audience = std::string{audience_value};
    permission = permission_value;
    scope_resources = scope.resources_size();
    last_scope = scope;
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
  wire::RuntimeCreateAccountRequest last_account;
  wire::RuntimeCreateInstallationRequest last_installation;
  wire::RuntimeListAccountsRequest last_accounts_page;
  target::OwnerOutcome<wire::RuntimeCreateAccountResponse>
  create_account(const wire::RuntimeCreateAccountRequest& input, std::string_view) override {
    ++calls;
    last_account = input;
    wire::RuntimeCreateAccountResponse response;
    response.mutable_account()->set_account_id("11111111-1111-4111-8111-111111111111");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeListAccountsResponse>
  list_accounts(const wire::RuntimeListAccountsRequest& input, std::string_view) override {
    ++calls;
    last_accounts_page = input;
    wire::RuntimeListAccountsResponse response;
    response.mutable_snapshot()->set_snapshot_id("snapshot-1");
    return ok(response);
  }
  wire::RuntimeBindAdapterRequest last_bind;
  wire::RuntimeSetDesiredStateRequest last_state;
  wire::RuntimeUpdateActionPolicyRequest last_policy;
  wire::RuntimeUpdateConfigRequest last_config;
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
    last_installation = input;
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
    auto* instance = response.mutable_instance();
    instance->set_instance_id(input.instance_id());
    instance->set_installation_id("installation-1");
    instance->set_host_id("host-1");
    instance->set_externally_managed(false);
    auto* state = instance->mutable_state();
    state->mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    state->mutable_resource()->set_id(input.instance_id());
    state->set_version(3);
    state->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    state->set_desired(input.desired_state());
    state->set_registered(true);
    state->set_desired_config_version(2);
    state->set_applied_config_version(1);
    state->set_process_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    state->mutable_observed_at()->set_seconds(1'700'000'000);
    state->set_source_service("host");
    response.mutable_operation()->set_owner_service("runtime");
    response.mutable_operation()->set_operation_id("operation-3");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeUpdateConfigResponse>
  update_config(const wire::RuntimeUpdateConfigRequest& input, std::string_view) override {
    ++calls;
    last_config = input;
    wire::RuntimeUpdateConfigResponse response;
    response.mutable_state()->mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    response.mutable_state()->mutable_resource()->set_id(input.resource().id());
    response.mutable_state()->set_version(5);
    response.mutable_state()->mutable_observed_at()->set_seconds(1'700'000'000);
    response.mutable_operation()->set_owner_service("runtime");
    response.mutable_operation()->set_operation_id("operation-config");
    return ok(response);
  }
  target::OwnerOutcome<wire::RuntimeGetInstanceConfigResponse>
  get_instance_config(const wire::RuntimeGetInstanceConfigRequest& input,
                      std::string_view) override {
    ++calls;
    wire::RuntimeGetInstanceConfigResponse response;
    auto* config = response.mutable_config();
    config->mutable_instance()->set_kind(wire::RESOURCE_INSTANCE);
    config->mutable_instance()->set_id(input.resource().id());
    config->set_desired_version(4);
    config->set_applied_version(3);
    config->set_apply_state(wire::CONFIG_APPLY_PENDING);
    config->mutable_observed_at()->set_seconds(1'700'000'000);
    config->set_delivery_id("delivery-1");
    return ok(response);
  }
  wire::RuntimeBeginDeleteRequest last_delete;
  wire::RuntimeRestoreResourceRequest last_restore;
  target::OwnerOutcome<wire::RuntimeBeginDeleteResponse>
  begin_delete(const wire::RuntimeBeginDeleteRequest& input, std::string_view) override {
    ++calls;
    last_delete = input;
    if (failure) {
      return *failure;
    }
    wire::RuntimeBeginDeleteResponse response;
    response.mutable_operation()->set_owner_service("runtime");
    response.mutable_operation()->set_operation_id("delete-operation");
    *response.mutable_operation()->mutable_resource() = input.resource();
    return response;
  }
  target::OwnerOutcome<wire::RuntimeRestoreResourceResponse>
  restore_resource(const wire::RuntimeRestoreResourceRequest& input, std::string_view) override {
    ++calls;
    last_restore = input;
    if (failure) {
      return *failure;
    }
    wire::RuntimeRestoreResourceResponse response;
    response.mutable_operation()->set_owner_service("runtime");
    response.mutable_operation()->set_operation_id("restore-operation");
    *response.mutable_operation()->mutable_resource() = input.resource();
    return response;
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
  wire::OperationState state = wire::OPERATION_RUNNING;
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
      operation.set_state(state);
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
struct FakeAuditLogs final : target::AuditLogSourcePort {
  std::optional<target::OwnerFailure> identity_failure;
  std::optional<target::OwnerFailure> runtime_failure;
  bool logs = false;
  int identity_calls = 0;
  int runtime_calls = 0;
  target::AuditLogFilter last_identity_filter;
  target::AuditLogFilter last_runtime_filter;
  std::string last_runtime_delegation;
  bool paged = false;
  std::vector<target::AuditLogRow> paged_identity;
  std::vector<target::AuditLogRow> paged_runtime;
  static std::optional<std::pair<std::int64_t, std::string>> cursor(std::string_view token) {
    const auto colon = token.find(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    std::int64_t nanos = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + colon, nanos);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + colon) {
      return std::nullopt;
    }
    return std::pair{nanos, std::string{token.substr(colon + 1)}};
  }
  static std::string token(const target::AuditLogRow& row) {
    return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              row.occurred_at.time_since_epoch())
                              .count()) +
           ":" + row.entry_id;
  }
  static target::AuditLogSourcePage slice(const std::vector<target::AuditLogRow>& rows,
                                          const target::AuditLogFilter& filter) {
    target::AuditLogSourcePage page;
    auto before = [](const target::AuditLogRow& row, const target::AuditLogRow& other) {
      if (row.occurred_at != other.occurred_at) {
        return row.occurred_at > other.occurred_at;
      }
      return row.entry_id > other.entry_id;
    };
    std::vector<const target::AuditLogRow*> filtered;
    for (const auto& row : rows) {
      if (filter.actor.empty() == false && row.actor != filter.actor) {
        continue;
      }
      if (filter.resource && row.resource_id != filter.resource->id()) {
        continue;
      }
      if (filter.occurred_from && row.occurred_at < *filter.occurred_from) {
        continue;
      }
      if (filter.occurred_until && row.occurred_at >= *filter.occurred_until) {
        continue;
      }
      if (!filter.page_token.empty()) {
        auto cursor_value = cursor(filter.page_token);
        if (!cursor_value) {
          continue;
        }
        const auto& [nanos, id] = *cursor_value;
        const auto row_nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(row.occurred_at.time_since_epoch())
                .count();
        if (row_nanos > nanos || (row_nanos == nanos && row.entry_id >= id)) {
          continue;
        }
      }
      filtered.push_back(&row);
    }
    std::sort(filtered.begin(), filtered.end(),
              [&](const auto* left, const auto* right) { return before(*left, *right); });
    const size_t take = std::min<std::size_t>(filtered.size(), filter.page_size);
    for (size_t index = 0; index < take; ++index) {
      page.rows.push_back(*filtered[index]);
    }
    if (filtered.size() > take) {
      page.next_token = token(page.rows.back());
    }
    return page;
  }
  target::OwnerOutcome<target::AuditLogSourcePage>
  identity(target::AuditLogKind kind, const target::AuditLogFilter& filter) override {
    ++identity_calls;
    last_identity_filter = filter;
    logs = kind == target::AuditLogKind::Logs;
    if (identity_failure) {
      return *identity_failure;
    }
    if (paged) {
      return slice(paged_identity, filter);
    }
    target::AuditLogSourcePage page;
    target::AuditLogRow row;
    row.entry_id = "identity-entry-1";
    row.owner = "identity";
    row.actor = "alice";
    row.method = logs ? "Login" : "Login";
    row.occurred_at = timepoint(1'000'000'000);
    row.severity = "info";
    row.component = "identity";
    row.message = "login accepted";
    page.rows.push_back(row);
    page.next_token = filter.page_token.empty() ? "" : "";
    return page;
  }
  target::OwnerOutcome<target::AuditLogSourcePage> runtime(target::AuditLogKind kind,
                                                           const target::AuditLogFilter& filter,
                                                           std::string_view delegation) override {
    ++runtime_calls;
    last_runtime_filter = filter;
    last_runtime_delegation = std::string{delegation};
    logs = kind == target::AuditLogKind::Logs;
    if (runtime_failure) {
      return *runtime_failure;
    }
    if (paged) {
      return slice(paged_runtime, filter);
    }
    target::AuditLogSourcePage page;
    target::AuditLogRow row;
    row.entry_id = "runtime-entry-1";
    row.owner = "runtime";
    row.actor = "bob";
    row.method = logs ? "SendMessage" : "UpdateAccount";
    row.occurred_at = timepoint(2'000'000'000);
    row.severity = "info";
    row.component = "runtime";
    row.message = "runtime accepted";
    page.rows.push_back(row);
    return page;
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
  target::OwnerOutcome<wire::SchedulerGetOccurrenceResponse>
  get_occurrence(const wire::SchedulerGetOccurrenceRequest& input, std::string_view) override {
    ++calls;
    if (failure)
      return *failure;
    wire::SchedulerGetOccurrenceResponse response;
    auto* occurrence = response.mutable_occurrence();
    occurrence->set_occurrence_id(input.occurrence_id());
    occurrence->set_task_id("task-1");
    occurrence->set_state(wire::OPERATION_SUCCEEDED);
    occurrence->set_accepted_event_id("event-1");
    occurrence->mutable_updated_at()->set_seconds(1);
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
  FakeAuditLogs audit_logs;
  target::OwnerGateway gateway{delegation, runtime,     &principals, &health,    &resources,
                               &tasks,     &operations, nullptr,     &audit_logs};
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
          "unconfigured logs source returns UNAVAILABLE instead of an empty page");
  const auto audit = fixture.gateway.handle(request("GET", "/api/v1/audit?page_size=20"));
  require(audit && audit->status == 503, "unconfigured audit source returns UNAVAILABLE");
  require(fixture.delegation.calls == 0 && fixture.runtime.calls == 0,
          "unconfigured sources do not call owners");
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
  fixture.delegation.failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  auto denied = fixture.gateway.handle(request("GET", "/api/v1/health"));
  require(denied && denied->status == 403 &&
              fixture.delegation.permission == wire::PERMISSION_HEALTH_READ,
          "health.read denial maps 403");
  fixture.delegation.failure.reset();
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

void runtime_catalogs() {
  OwnerCommands fixture;
  const std::string catalog =
      R"({"kind":"RESOURCE_CATALOG","id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"})";
  auto create = request("POST", "/api/v1/accounts");
  create.body = R"({"platform":"qq","platform_account_id":"test","catalog":)" + catalog + "}";
  auto created = fixture.gateway.handle(create);
  require(created && created->status == 200 && fixture.resources.last_account.has_catalog() &&
              fixture.delegation.last_scope.resources(0).id() ==
                  "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa" &&
              fixture.delegation.permission == wire::PERMISSION_ACCOUNT_MANAGE,
          "create account forwards exact catalog");
  require(fixture.resources.last_account.context().idempotency_key() == "fixture-write-key-0001",
          "write key survives Control boundary");
  create.headers["x-request-id"] = "new-request-id";
  require(fixture.gateway.handle(create)->status == 200 &&
              fixture.resources.last_account.context().idempotency_key() ==
                  "fixture-write-key-0001",
          "new request id preserves caller key");
  create.body = R"({"platform":"qq","platform_account_id":"test"})";
  require(fixture.gateway.handle(create)->status == 400, "missing create catalog fails closed");
  create.body =
      R"({"catalog":{"kind":"RESOURCE_ACCOUNT","id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}})";
  require(fixture.gateway.handle(create)->status == 400, "catalog cannot be a child resource");
  auto listing =
      request("GET", "/api/v1/accounts?catalog=" + catalog + "&page_size=2&page_token=cursor");
  auto empty = fixture.gateway.handle(listing);
  require(empty && empty->status == 200 && nlohmann::json::parse(empty->body)["accounts"].empty() &&
              fixture.delegation.scope_resources == 1 &&
              fixture.resources.last_accounts_page.page().page_token() == "cursor",
          "empty child set still delegates catalog and emits a typed empty page");
  listing.target += "&scope=" + scope_json();
  require(fixture.gateway.handle(listing)->status == 200 &&
              fixture.delegation.scope_resources == 2 && fixture.delegation.selected &&
              fixture.delegation.selected->resources_size() == 1,
          "explicit selection contains only child references");
  auto wrong = request("GET", "/api/v1/accounts?catalog=" + catalog + "&scope={\"resources\":[" +
                                  catalog + "]}");
  require(fixture.gateway.handle(wrong)->status == 400,
          "selection rejects catalog instead of expanding its children");
  fixture.delegation.catalog_failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  require(fixture.gateway.handle(listing)->status == 403, "revoked child grant fails closed");
  fixture.delegation.catalog_failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  require(fixture.gateway.handle(listing)->status == 503,
          "assignment dependency outage fails closed");
  fixture.delegation.catalog_failure = target::OwnerFailure{wire::RATE_LIMITED, true};
  require(fixture.gateway.handle(listing)->status == 429, "oversized child grants require a batch");
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

std::string occurrence_scope() {
  return "?scope=" + scope_json();
}

void occurrence_snapshot() {
  OwnerCommands fixture;
  const auto scope = occurrence_scope();
  auto result = fixture.gateway.handle(request("GET", "/api/v1/occurrences/occurrence-1" + scope));
  require(result && result->status == 200, "occurrence snapshot 200");
  require(result->body.find("occurrence-1") != std::string::npos &&
              result->body.find("task-1") != std::string::npos,
          "occurrence projection returns owner occurrence");
  auto missing_scope = fixture.gateway.handle(request("GET", "/api/v1/occurrences/occurrence-1"));
  require(missing_scope && missing_scope->status == 400, "occurrence requires explicit scope");
  auto invalid =
      fixture.gateway.handle(request("GET", "/api/v1/occurrences/occurrence-1?scope=not-json"));
  require(invalid && invalid->status == 400, "occurrence invalid scope 400");
  auto missing_token = request("GET", "/api/v1/occurrences/occurrence-1" + scope);
  missing_token.access_token.clear();
  require(fixture.gateway.handle(missing_token)->status == 401,
          "occurrence requires authentication");
  fixture.tasks.failure = target::OwnerFailure{wire::NOT_FOUND, false};
  require(fixture.gateway.handle(request("GET", "/api/v1/occurrences/missing-1" + scope))->status ==
              404,
          "occurrence not-found maps 404");
  fixture.tasks.failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  require(
      fixture.gateway.handle(request("GET", "/api/v1/occurrences/occurrence-1" + scope))->status ==
          503,
      "occurrence unavailable maps 503");
  fixture.tasks.failure.reset();
  require(fixture.delegation.permission == wire::PERMISSION_TASK_READ &&
              fixture.delegation.audience == "scheduler",
          "occurrence uses scheduler task.read delegation");
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
  // BLOCKED is a successful authoritative snapshot, never a failed operation command.
  fixture.operations.state = wire::OPERATION_BLOCKED;
  auto blocked = fixture.gateway.handle(
      request("GET", "/api/v1/operations/runtime/op-blocked?scope=" + scope_json()));
  require(blocked && blocked->status == 200 &&
              blocked->body.find("\"state\":\"OPERATION_BLOCKED\"") != std::string::npos,
          "blocked operation is a 200 snapshot, not a failure");
  fixture.operations.state = wire::OPERATION_RUNNING;
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

void instance_plugin_state_and_configuration() {
  OwnerCommands fixture;
  const std::string installation = "installation-1";
  const std::string instance = "inst-1";
  const std::string base = "/api/v1/installations/" + installation + "/instances/" + instance;
  auto state = request("PUT", base + "/plugin-state");
  state.body =
      R"({"installation_id":"installation-1","instance_id":"inst-1","desired":"DESIRED_RUNNING","expected_version":"2"})";
  auto started = fixture.gateway.handle(state);
  require(started && started->status == 200, "plugin-state 200");
  require(started->body.find("\"desired\":\"DESIRED_RUNNING\"") != std::string::npos &&
              started->body.find("\"process_outcome\":\"EXECUTION_KNOWN_SUCCEEDED\"") !=
                  std::string::npos &&
              started->body.find("\"apply_state\":\"CONFIG_APPLY_PENDING\"") != std::string::npos &&
              started->body.find("operation-3") != std::string::npos,
          "three separate facts + operation, non-APPLIED explicit");
  require(fixture.resources.last_state.expected_version() == 2 &&
              fixture.delegation.permission == wire::PERMISSION_PLUGIN_MANAGE,
          "plugin-state expected_version/delegation");
  auto mismatch = state;
  mismatch.body =
      R"({"installation_id":"other","instance_id":"inst-1","desired":"DESIRED_RUNNING"})";
  require(fixture.gateway.handle(mismatch)->status == 400, "plugin-state path/body mismatch");
  fixture.resources.failure = target::OwnerFailure{wire::CONFLICT, false};
  require(fixture.gateway.handle(state)->status == 409, "plugin-state conflict maps 409");
  fixture.resources.failure.reset();

  auto get_config = request("GET", base + "/configuration");
  auto read = fixture.gateway.handle(get_config);
  require(read && read->status == 200, "configuration GET 200");
  require(read->body.find("\"desired_config_version\":\"4\"") != std::string::npos &&
              read->body.find("\"apply_state\":\"CONFIG_APPLY_PENDING\"") != std::string::npos &&
              read->body.find("delivery-1") != std::string::npos,
          "configuration facts include pending apply state");
  require(fixture.delegation.permission == wire::PERMISSION_PLUGIN_CONFIG_READ,
          "configuration GET delegation");
  auto put_config = request("PUT", base + "/configuration");
  put_config.body = R"({"configuration":{"timeout":5},"expected_version":"4"})";
  auto written = fixture.gateway.handle(put_config);
  require(written && written->status == 200, "configuration PUT 200");
  require(written->body.find("\"apply_state\":\"CONFIG_APPLY_PENDING\"") != std::string::npos &&
              written->body.find("operation-config") != std::string::npos &&
              written->body.find("bootstrap") == std::string::npos,
          "configuration PUT returns pending apply + operation, no secret echo");
  require(fixture.resources.last_config.expected_version() == 4 &&
              fixture.resources.last_config.resource().id() == instance &&
              fixture.delegation.permission == wire::PERMISSION_PLUGIN_MANAGE,
          "configuration PUT expected_version/resource");
  auto bad_put = put_config;
  bad_put.body = R"({"configuration":{"timeout":5},"expected_version":"4"})";
  require(fixture.gateway.handle(bad_put)->status == 200,
          "second PUT same body is idempotent forward");
  auto missing_version = put_config;
  missing_version.body = R"({"configuration":{"timeout":5}})";
  require(fixture.gateway.handle(missing_version)->status == 400,
          "configuration PUT requires expected_version");
  auto unknown = request("PUT", base + "/unknown");
  require(fixture.gateway.handle(unknown)->status == 404, "unknown instance action");
}
void lifecycle_delete_restore() {
  OwnerCommands fixture;
  const std::string resource_id = "bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb";
  const std::string base = "/api/v1/resources/RESOURCE_INSTANCE/" + resource_id;
  auto remove = request("POST", base + "/delete");
  remove.headers["idempotency-key"] = "delete-idempotency-0001";
  remove.body =
      R"({"resource":{"kind":"RESOURCE_INSTANCE","id":"bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb"},"expected_version":"3"})";
  auto deleted = fixture.gateway.handle(remove);
  require(deleted && deleted->status == 202, "begin-delete accepted 202");
  require(deleted->body.find("delete-operation") != std::string::npos &&
              deleted->body.find("owner_service") != std::string::npos,
          "delete returns owner operation ref");
  require(fixture.delegation.permission == wire::PERMISSION_RESOURCE_MANAGE &&
              fixture.resources.last_delete.expected_version() == 3 &&
              fixture.resources.last_delete.resource().id() == resource_id,
          "delete delegation/permission/expected_version");
  auto restore = request("POST", base + "/restore");
  restore.headers["idempotency-key"] = "restore-idempotency-0001";
  restore.body =
      R"({"resource":{"kind":"RESOURCE_INSTANCE","id":"bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb"},"expected_version":"1"})";
  auto restored = fixture.gateway.handle(restore);
  require(restored && restored->status == 202 &&
              restored->body.find("restore-operation") != std::string::npos,
          "restore accepted 202 with operation");
  // Account resources use account.manage rather than resource.manage.
  auto account_delete = remove;
  account_delete.target = "/api/v1/resources/RESOURCE_ACCOUNT/" + resource_id + "/delete";
  account_delete.body =
      R"({"resource":{"kind":"RESOURCE_ACCOUNT","id":"bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb"},"expected_version":"2"})";
  require(fixture.gateway.handle(account_delete)->status == 202 &&
              fixture.delegation.permission == wire::PERMISSION_ACCOUNT_MANAGE,
          "account delete uses account.manage");
  // Path/body mismatch, missing version/idempotency and unknown kind are invalid.
  auto mismatch = remove;
  mismatch.body =
      R"({"resource":{"kind":"RESOURCE_INSTANCE","id":"cccccccc-1111-4111-8111-cccccccccccc"},"expected_version":"3"})";
  require(fixture.gateway.handle(mismatch)->status == 400, "path/body resource mismatch 400");
  auto missing_version = remove;
  missing_version.body =
      R"({"resource":{"kind":"RESOURCE_INSTANCE","id":"bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb"}})";
  require(fixture.gateway.handle(missing_version)->status == 400, "expected_version required 400");
  auto missing_idempotency = remove;
  missing_idempotency.headers.erase("idempotency-key");
  require(fixture.gateway.handle(missing_idempotency)->status == 400,
          "idempotency key required 400");
  auto missing_token = remove;
  missing_token.access_token.clear();
  require(fixture.gateway.handle(missing_token)->status == 401, "lifecycle requires auth");
  auto unsupported_kind = remove;
  unsupported_kind.target = "/api/v1/resources/RESOURCE_UNKNOWN/" + resource_id + "/delete";
  require(fixture.gateway.handle(unsupported_kind)->status == 400, "unknown kind 400");
  fixture.resources.failure = target::OwnerFailure{wire::CONFLICT, false};
  require(fixture.gateway.handle(remove)->status == 409, "delete conflict maps 409");
  fixture.resources.failure = target::OwnerFailure{wire::NOT_FOUND, false};
  require(fixture.gateway.handle(remove)->status == 404, "delete not-found maps 404");
  fixture.resources.failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  require(fixture.gateway.handle(remove)->status == 503, "delete owner unavailable maps 503");
  fixture.resources.failure.reset();
}

void audit_logs_aggregation() {
  OwnerCommands fixture;
  auto logs = request("GET", "/api/v1/logs?page_size=20&occurred_from=2026-11-02T00:00:00Z");
  auto page = fixture.gateway.handle(logs);
  require(page && page->status == 200, "logs aggregation 200");
  require(page->body.find("\"owner\":\"identity\"") != std::string::npos &&
              page->body.find("\"owner\":\"runtime\"") != std::string::npos &&
              page->body.find("runtime-entry-1") != std::string::npos,
          "logs mark both owners and merge rows");
  // occurred_at DESC: runtime 2s row precedes identity 1s row.
  require(page->body.find("runtime-entry-1") < page->body.find("identity-entry-1"),
          "logs merge sorted by occurred_at desc");
  require(fixture.audit_logs.logs && fixture.delegation.permission == wire::PERMISSION_LOG_READ &&
              fixture.delegation.audience == "runtime",
          "logs uses log.read delegation only");
  require(fixture.audit_logs.last_identity_filter.occurred_from.has_value() &&
              fixture.audit_logs.last_runtime_filter.page_size == 20,
          "filters forwarded to both owners");
  fixture.delegation.failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  fixture.delegation.calls = 0;
  require(fixture.gateway.handle(request("GET", "/api/v1/audit"))->status == 403,
          "missing audit.read denied before owners");
  fixture.delegation.failure.reset();
  auto audit = fixture.gateway.handle(request("GET", "/api/v1/audit"));
  require(audit && audit->status == 200 &&
              fixture.delegation.permission == wire::PERMISSION_AUDIT_READ &&
              !fixture.audit_logs.logs,
          "audit uses audit.read and never substitutes logs");
  auto invalid_cursor =
      fixture.gateway.handle(request("GET", "/api/v1/logs?page_token=not-a-cursor"));
  require(invalid_cursor && invalid_cursor->status == 400, "invalid cursor 400");
  auto invalid_time =
      fixture.gateway.handle(request("GET", "/api/v1/audit?occurred_from=not-a-time"));
  require(invalid_time && invalid_time->status == 400, "invalid time filter 400");
  auto invalid_actor =
      fixture.gateway.handle(request("GET", "/api/v1/logs?actor=" + std::string(300, 'a')));
  require(invalid_actor && invalid_actor->status == 400, "oversized actor filter 400");
  fixture.audit_logs.identity_failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
  require(fixture.gateway.handle(request("GET", "/api/v1/logs"))->status == 403,
          "identity source denial maps 403");
  fixture.audit_logs.identity_failure.reset();
  fixture.audit_logs.runtime_failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  require(fixture.gateway.handle(request("GET", "/api/v1/logs"))->status == 503,
          "runtime source unavailable maps 503 (no partial page)");
  fixture.audit_logs.runtime_failure.reset();
  auto no_token = request("GET", "/api/v1/audit");
  no_token.access_token.clear();
  require(fixture.gateway.handle(no_token)->status == 401, "logs/audit require authentication");
  auto keyset =
      request("GET", "/api/v1/logs?page_token=1000000000:aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa");
  require(fixture.gateway.handle(keyset)->status == 200 &&
              fixture.audit_logs.last_identity_filter.page_token ==
                  "1000000000:aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa",
          "keyset token passed through to owners");
}

void audit_logs_cross_owner_keyset() {
  OwnerCommands fixture;
  fixture.audit_logs.paged = true;
  const auto row = [](std::string owner, std::string id, std::int64_t nanos, std::string actor) {
    target::AuditLogRow value;
    value.owner = std::move(owner);
    value.entry_id = std::move(id);
    value.occurred_at = timepoint(nanos);
    value.actor = std::move(actor);
    value.component = value.owner;
    value.method = "List";
    value.message = "request.recorded";
    value.severity = "info";
    value.resource_id = "catalog";
    value.resource_kind = wire::RESOURCE_CATALOG;
    return value;
  };
  // Ties at 4s and 3s prove owner ASC + numeric id DESC ordering; interleaving proves no
  // cross-owner row is skipped or repeated when a merged page contains a prefix of one source.
  fixture.audit_logs.paged_identity = {
      row("identity", "10", 5'000'000'000, "alice"),
      row("identity", "9", 4'000'000'000, "alice"),
      row("identity", "30", 3'000'000'000, "alice"),
      row("identity", "8", 1'000'000'000, "alice"),
  };
  fixture.audit_logs.paged_runtime = {
      row("runtime", "20", 4'500'000'000, "bob"),
      row("runtime", "40", 3'000'000'000, "bob"),
      row("runtime", "19", 2'000'000'000, "bob"),
  };
  std::vector<std::string> ids;
  std::vector<std::string> pages;
  std::string token;
  for (int page = 0; page < 10; ++page) {
    auto query = request("GET", "/api/v1/logs?page_size=2" +
                                    (token.empty() ? std::string{} : "&page_token=" + token));
    auto result = fixture.gateway.handle(query);
    require(result && result->status == 200, "paged logs 200");
    const auto body = nlohmann::json::parse(result->body);
    pages.push_back(result->body);
    for (const auto& item : body["items"]) {
      ids.push_back(item["id"].get<std::string>());
      require(item.contains("owner") && !item["owner"].get<std::string>().empty(),
              "paged log row owner label");
    }
    token = body["page"].value("next_page_token", "");
    if (token.empty()) {
      break;
    }
  }
  const std::vector<std::string> expected{"10", "20", "9", "30", "40", "19", "8"};
  require(ids == expected, "cross-owner keyset order/duplicates/loss");
  require(pages.size() == 4, "cross-owner two-row pages");
  // A same-timestamp tie resolves owner ASC: identity row 30 precedes runtime row 40.
  const auto identity_tie = std::find(ids.begin(), ids.end(), "30") - ids.begin();
  const auto runtime_tie = std::find(ids.begin(), ids.end(), "40") - ids.begin();
  require(identity_tie < runtime_tie, "same-timestamp owner ordering");
  // Filters are forwarded to both owner sources; the composite token is decoded per owner.
  auto filtered = fixture.gateway.handle(
      request("GET", "/api/v1/audit?page_size=2&actor=alice&resource=" +
                         std::string{"{\"kind\":\"RESOURCE_CATALOG\",\"id\":\"catalog\"}"}));
  require(filtered && filtered->status == 200, "filtered audit 200");
  require(fixture.audit_logs.last_identity_filter.actor == "alice" &&
              fixture.audit_logs.last_runtime_filter.actor == "alice" &&
              fixture.audit_logs.last_runtime_filter.resource.has_value(),
          "filters forwarded to both owners");
  // Legacy cursor remains accepted and forwarded to both owners.
  auto legacy = fixture.gateway.handle(
      request("GET", "/api/v1/logs?page_token=1000000000:aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa"));
  require(legacy && legacy->status == 200 &&
              fixture.audit_logs.last_identity_filter.page_token ==
                  "1000000000:aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa" &&
              fixture.audit_logs.last_runtime_filter.page_token ==
                  "1000000000:aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa",
          "legacy cursor forwarded to both owners");
}

void sensitive_read_permissions() {
  Gateway fixture;
  require(fixture.gateway.handle(request("GET", "/api/v1/logs"))->status == 503,
          "logs UNAVAILABLE without an authoritative source");
  auto missing_token = request("GET", "/api/v1/audit");
  missing_token.access_token.clear();
  require(fixture.gateway.handle(missing_token)->status == 401,
          "logs/audit require authentication");
  OwnerCommands granted;
  require(granted.gateway.handle(request("GET", "/api/v1/logs"))->status == 200,
          "granted log.read returns authoritative page");
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
  require(listed && listed->status == 200 &&
              listed->body.find("\"items\":[") != std::string::npos &&
              listed->body.find("\"id\":\"model-1\"") != std::string::npos &&
              listed->body.find("\"name\":\"default\"") != std::string::npos &&
              listed->body.find("\"state\":\"LIFECYCLE_ACTIVE\"") != std::string::npos &&
              listed->body.find("\"page\":{") != std::string::npos &&
              listed->body.find("\"models\"") == std::string::npos &&
              model.list_request.scope().resources_size() == 1,
          "model list projects OpenAPI ModelPage items/page");
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
    runtime_catalogs();
    operation_snapshots();
    audit_logs_aggregation();
    lifecycle_delete_restore();
    occurrence_snapshot();
    audit_logs_cross_owner_keyset();
    instance_plugin_state_and_configuration();
    validation_rules();
    model_management_surface();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "control owner gateway: ok\n";
  return 0;
}
