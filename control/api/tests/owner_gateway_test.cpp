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
  bool throws_ = false;
  std::string access_token_;
  core::target::TimePoint deadline_;
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
                                          const wire::ResourceScope& scope, std::string_view token,
                                          core::target::TimePoint until) override {
    ++calls;
    if (throws_) {
      throw std::runtime_error("private exception");
    }
    access_token_ = token;
    deadline_ = until;
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
  target::OwnerOutcome<wire::RuntimeListInstallationsResponse>
  list_installations(const wire::RuntimeListInstallationsRequest&, std::string_view) override {
    ++calls;
    return ok(wire::RuntimeListInstallationsResponse{});
  }
  target::OwnerOutcome<wire::RuntimeListInstancesResponse>
  list_instances(const wire::RuntimeListInstancesRequest&, std::string_view) override {
    ++calls;
    return ok(wire::RuntimeListInstancesResponse{});
  }
  target::OwnerOutcome<wire::RuntimeListAdaptersResponse>
  list_adapters(const wire::RuntimeListAdaptersRequest&, std::string_view) override {
    ++calls;
    return ok(wire::RuntimeListAdaptersResponse{});
  }
  target::OwnerOutcome<wire::RuntimeListHostsResponse>
  list_hosts(const wire::RuntimeListHostsRequest&, std::string_view) override {
    ++calls;
    return ok(wire::RuntimeListHostsResponse{});
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
  unbind_adapter(const wire::RuntimeUnbindAdapterRequest&, std::string_view) override {
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
  create_instance(const wire::RuntimeCreateInstanceRequest&, std::string_view) override {
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
    instance->set_host_id("11111111-1111-4111-8111-111111111111");
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
  const auto default_page =
      fixture.gateway.handle(request("GET", "/api/v1/accounts?catalog=" + catalog));
  require(default_page && default_page->status == 200 &&
              fixture.resources.last_accounts_page.page().page_size() == 20,
          "omitted page_size forwards the OpenAPI default to Runtime");
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

// One inventory exercises every owner-backed operation with the same public gateway. Each
// scenario checks the permission/audience/scope and both delegation and owner failure paths.
struct RoutedOwners {
  FakeDelegation delegation_;
  FakeRuntime runtime_;
  FakePrincipals principals_;
  FakeHealth health_;
  FakeResources resources_;
  FakeTasks tasks_;
  FakeOperations operations_;
  FakeAuditLogs audit_logs_;
  FakeModel model_;
  target::OwnerGateway gateway_{delegation_, runtime_,     &principals_, &health_,    &resources_,
                                &tasks_,     &operations_, &model_,      &audit_logs_};
};
struct OwnerRoute {
  std::string method_;
  std::string path_;
  std::string body_;
  std::string audience_;
  wire::Permission permission_;
  wire::ResourceKind scope_kind_;
  std::string scope_id_;
  bool preserves_retryable_ = true;
  unsigned int status_ = 200;
};
std::vector<OwnerRoute> owner_routes() {
  const std::string catalog_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  const std::string catalog = R"({"kind":"RESOURCE_CATALOG","id":")" + catalog_id + R"("})";
  const auto catalog_query = "?catalog=" + catalog;
  const auto account_query = "?scope=" + scope_json();
  const std::string account_id = "11111111-1111-4111-8111-111111111111";
  const std::string instance_base = "/api/v1/installations/installation-1/instances/inst-1";
  const std::string binding =
      R"({"binding":{"account_id":"account-1","adapter_instance_id":"adapter-1"},"expected_version":"2"})";
  const std::string state =
      R"({"instance_id":"inst-1","desired_state":"DESIRED_RUNNING","expected_version":"3"})";
  const std::string config =
      R"({"resource":{"kind":"RESOURCE_INSTANCE","id":"inst-1"},"config":{},"expected_version":"2"})";
  const std::string lifecycle =
      R"({"resource":{"kind":"RESOURCE_ACCOUNT","id":"account-1"},"expected_version":"2"})";
  return {
      {"GET",
       "/api/v1/health",
       {},
       "control",
       wire::PERMISSION_HEALTH_READ,
       wire::RESOURCE_CATALOG,
       "control"},
      {"GET",
       "/api/v1/principals",
       {},
       "identity",
       wire::PERMISSION_IDENTITY_PRINCIPAL_READ,
       wire::RESOURCE_CATALOG,
       "control"},
      {"GET",
       "/api/v1/accounts" + catalog_query,
       {},
       "runtime",
       wire::PERMISSION_ACCOUNT_READ,
       wire::RESOURCE_CATALOG,
       catalog_id},
      {"GET",
       "/api/v1/installations" + catalog_query,
       {},
       "runtime",
       wire::PERMISSION_PLUGIN_READ,
       wire::RESOURCE_CATALOG,
       catalog_id},
      {"GET",
       "/api/v1/instances" + catalog_query,
       {},
       "runtime",
       wire::PERMISSION_PLUGIN_READ,
       wire::RESOURCE_CATALOG,
       catalog_id},
      {"GET",
       "/api/v1/adapter-instances" + catalog_query,
       {},
       "runtime",
       wire::PERMISSION_ADAPTER_READ,
       wire::RESOURCE_CATALOG,
       catalog_id},
      {"GET",
       "/api/v1/hosts" + catalog_query,
       {},
       "runtime",
       wire::PERMISSION_HOST_READ,
       wire::RESOURCE_CATALOG,
       catalog_id},
      {"PUT", "/api/v1/adapter-bindings/account-1", binding, "runtime",
       wire::PERMISSION_ACCOUNT_MANAGE, wire::RESOURCE_ACCOUNT, "account-1", false},
      {"DELETE", "/api/v1/adapter-bindings/account-1", binding, "runtime",
       wire::PERMISSION_ACCOUNT_MANAGE, wire::RESOURCE_ACCOUNT, "account-1", false},
      {"POST", "/api/v1/accounts", R"({"catalog":)" + catalog + "}", "runtime",
       wire::PERMISSION_ACCOUNT_MANAGE, wire::RESOURCE_CATALOG, catalog_id, false},
      {"POST", "/api/v1/installations", R"({"catalog":)" + catalog + "}", "runtime",
       wire::PERMISSION_PLUGIN_MANAGE, wire::RESOURCE_CATALOG, catalog_id, false},
      {"POST", "/api/v1/installations/installation-1/instances",
       R"({"installation_id":"installation-1","externally_managed":true})", "runtime",
       wire::PERMISSION_PLUGIN_MANAGE, wire::RESOURCE_INSTALLATION, "installation-1", false},
      {"PUT", instance_base + "/plugin-state",
       R"({"installation_id":"installation-1","instance_id":"inst-1","desired":"DESIRED_RUNNING","expected_version":"2"})",
       "runtime", wire::PERMISSION_PLUGIN_MANAGE, wire::RESOURCE_INSTANCE, "inst-1"},
      {"GET",
       instance_base + "/configuration",
       {},
       "runtime",
       wire::PERMISSION_PLUGIN_CONFIG_READ,
       wire::RESOURCE_INSTANCE,
       "inst-1"},
      {"PUT", instance_base + "/configuration",
       R"({"configuration":{"timeout":5},"expected_version":"2"})", "runtime",
       wire::PERMISSION_PLUGIN_MANAGE, wire::RESOURCE_INSTANCE, "inst-1"},
      {"PUT", "/api/v1/instances/inst-1/desired-state", state, "runtime",
       wire::PERMISSION_PLUGIN_MANAGE, wire::RESOURCE_INSTANCE, "inst-1", false},
      {"PUT", "/api/v1/instances/inst-1/config", config, "runtime", wire::PERMISSION_PLUGIN_MANAGE,
       wire::RESOURCE_INSTANCE, "inst-1", false},
      {"GET",
       "/api/v1/instances/inst-1/config",
       {},
       "runtime",
       wire::PERMISSION_PLUGIN_CONFIG_READ,
       wire::RESOURCE_INSTANCE,
       "inst-1",
       false},
      {"GET",
       "/api/v1/accounts/account-1/action-policy",
       {},
       "runtime",
       wire::PERMISSION_ACCOUNT_POLICY_READ,
       wire::RESOURCE_ACCOUNT,
       "account-1",
       false},
      {"PUT", "/api/v1/accounts/account-1/action-policy",
       R"({"policy":{"account_id":"account-1"},"expected_version":"2"})", "runtime",
       wire::PERMISSION_ACCOUNT_POLICY_MANAGE, wire::RESOURCE_ACCOUNT, "account-1", false},
      {"GET",
       "/api/v1/tasks" + account_query,
       {},
       "scheduler",
       wire::PERMISSION_TASK_READ,
       wire::RESOURCE_ACCOUNT,
       account_id,
       false},
      {"GET",
       "/api/v1/tasks/task-1" + account_query,
       {},
       "scheduler",
       wire::PERMISSION_TASK_READ,
       wire::RESOURCE_ACCOUNT,
       account_id,
       false},
      {"POST",
       "/api/v1/tasks/task-1/cancel" + account_query,
       {},
       "scheduler",
       wire::PERMISSION_TASK_SCHEDULE,
       wire::RESOURCE_ACCOUNT,
       account_id,
       false},
      {"GET",
       "/api/v1/occurrences/occurrence-1" + account_query,
       {},
       "scheduler",
       wire::PERMISSION_TASK_READ,
       wire::RESOURCE_ACCOUNT,
       account_id},
      {"GET",
       "/api/v1/operations/runtime/operation-1" + account_query,
       {},
       "runtime",
       wire::PERMISSION_RESOURCE_READ,
       wire::RESOURCE_ACCOUNT,
       account_id},
      {"GET",
       "/api/v1/operations/scheduler/operation-1" + account_query,
       {},
       "scheduler",
       wire::PERMISSION_TASK_READ,
       wire::RESOURCE_ACCOUNT,
       account_id},
      {"GET",
       "/api/v1/operations/model/operation-1" + account_query,
       {},
       "model",
       wire::PERMISSION_RESOURCE_READ,
       wire::RESOURCE_ACCOUNT,
       account_id},
      {"GET",
       "/api/v1/logs",
       {},
       "runtime",
       wire::PERMISSION_LOG_READ,
       wire::RESOURCE_CATALOG,
       "control"},
      {"GET",
       "/api/v1/audit",
       {},
       "runtime",
       wire::PERMISSION_AUDIT_READ,
       wire::RESOURCE_CATALOG,
       "control"},
      {"POST", "/api/v1/resources/RESOURCE_ACCOUNT/account-1/delete", lifecycle, "runtime",
       wire::PERMISSION_ACCOUNT_MANAGE, wire::RESOURCE_ACCOUNT, "account-1", true, 202},
      {"POST", "/api/v1/resources/RESOURCE_ACCOUNT/account-1/restore", lifecycle, "runtime",
       wire::PERMISSION_ACCOUNT_MANAGE, wire::RESOURCE_ACCOUNT, "account-1", true, 202},
      {"GET",
       "/api/v1/models" + model_scope_query(),
       {},
       "model",
       wire::PERMISSION_MODEL_READ,
       wire::RESOURCE_MODEL,
       "model-1"},
      {"GET",
       model_path(),
       {},
       "model",
       wire::PERMISSION_MODEL_READ,
       wire::RESOURCE_MODEL,
       "model-1"},
      {"PUT", "/api/v1/models/model-1", R"({"config":{"model_id":"model-1"}})", "model",
       wire::PERMISSION_MODEL_MANAGE, wire::RESOURCE_MODEL, "model-1"},
      {"POST", "/api/v1/models/model-1/disable", R"({"expected_version":"2"})", "model",
       wire::PERMISSION_MODEL_MANAGE, wire::RESOURCE_MODEL, "model-1"},
      {"POST", "/api/v1/models/model-1/enable", R"({"expected_version":"2"})", "model",
       wire::PERMISSION_MODEL_MANAGE, wire::RESOURCE_MODEL, "model-1"},
      {"PUT", "/api/v1/models/model-1/secret",
       R"({"model_id":"model-1","new_secret":"c2VjcmV0","expected_version":"2"})", "model",
       wire::PERMISSION_MODEL_MANAGE, wire::RESOURCE_MODEL, "model-1"},
      {"GET",
       model_path("/configuration"),
       {},
       "model",
       wire::PERMISSION_MODEL_MANAGE,
       wire::RESOURCE_MODEL,
       "model-1"},
      {"GET",
       model_path("/calls/call-1"),
       {},
       "model",
       wire::PERMISSION_MODEL_CALL_INSPECT,
       wire::RESOURCE_MODEL,
       "model-1"},
      {"POST", model_path("/calls/call-1/resolve"),
       R"({"disposition":"EXECUTED_BILLED","expected_version":"2"})", "model",
       wire::PERMISSION_MODEL_MANAGE, wire::RESOURCE_MODEL, "model-1"},
      {"GET",
       action_scope_path(),
       {},
       "runtime",
       wire::PERMISSION_ACTION_INSPECT,
       wire::RESOURCE_ACCOUNT,
       account_id},
      {"GET",
       action_scope_path("by-idempotency-key") + "&owner_instance=instance-1",
       {},
       "runtime",
       wire::PERMISSION_ACTION_INSPECT,
       wire::RESOURCE_ACCOUNT,
       account_id},
  };
}
void require_failure(const std::optional<target::OwnerHttpResult>& result, wire::ErrorCode code,
                     bool retryable, std::string_view scenario) {
  require(result && result->status == target::owner_http_status(code), scenario);
  const auto body = nlohmann::json::parse(result->body);
  require(body["code"] == wire::ErrorCode_Name(code) && body["retryable"] == retryable &&
              body["request_id"] == "request-123" &&
              body["trace_id"] == "0123456789abcdef0123456789abcdef" && body["details"].empty(),
          scenario);
}
target::OwnerHttpRequest route_request(const OwnerRoute& route) {
  auto input = request(route.method_, route.path_);
  input.body = route.body_;
  input.headers["traceparent"] = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";
  input.headers["Idempotency-Key"] = "fixture-write-key-0001";
  return input;
}
void owner_route_inventory() {
  for (const auto& route : owner_routes()) {
    RoutedOwners fixture;
    const auto input = route_request(route);
    const auto result = fixture.gateway_.handle(input);
    const auto scenario = route.method_ + " " + route.path_;
    require(result && result->status == route.status_, scenario);
    require(fixture.delegation_.calls == 1 && fixture.delegation_.audience == route.audience_ &&
                fixture.delegation_.permission == route.permission_ &&
                fixture.delegation_.last_scope.resources_size() == 1 &&
                fixture.delegation_.last_scope.resources(0).kind() == route.scope_kind_ &&
                fixture.delegation_.last_scope.resources(0).id() == route.scope_id_ &&
                fixture.delegation_.access_token_ == input.access_token &&
                fixture.delegation_.deadline_ == input.deadline,
            scenario + " exact delegation and request budget");
    for (const auto code : {wire::UNAUTHENTICATED, wire::PERMISSION_DENIED,
                            wire::UPSTREAM_UNAVAILABLE, wire::TIMEOUT}) {
      fixture.delegation_.failure = target::OwnerFailure{code, true};
      require_failure(fixture.gateway_.handle(input), code, route.preserves_retryable_,
                      scenario + " delegation error");
      fixture.delegation_.failure.reset();
      fixture.runtime_.failure = target::OwnerFailure{code, true};
      fixture.principals_.failure = target::OwnerFailure{code, true};
      fixture.resources_.failure = target::OwnerFailure{code, true};
      fixture.tasks_.failure = target::OwnerFailure{code, true};
      fixture.operations_.failure = target::OwnerFailure{code, true};
      fixture.audit_logs_.runtime_failure = target::OwnerFailure{code, true};
      fixture.model_.failure = target::OwnerFailure{code, true};
      // Local health has no owner RPC after its Identity delegation.
      if (route.path_ != "/api/v1/health") {
        require_failure(fixture.gateway_.handle(input), code, route.preserves_retryable_,
                        scenario + " owner error");
      }
      fixture.runtime_.failure.reset();
      fixture.principals_.failure.reset();
      fixture.resources_.failure.reset();
      fixture.tasks_.failure.reset();
      fixture.operations_.failure.reset();
      fixture.audit_logs_.runtime_failure.reset();
      fixture.model_.failure.reset();
    }
  }
}
void head_and_unmatched_routes() {
  RoutedOwners fixture;
  const auto query = "?scope=" + scope_json();
  for (const auto& path :
       {std::string{"/health/live"}, std::string{"/health/ready"}, std::string{"/api/v1/health"},
        std::string{"/api/v1/principals"}, std::string{"/api/v1/instances/inst-1/config"},
        std::string{"/api/v1/tasks/task-1"} + query,
        std::string{"/api/v1/occurrences/occurrence-1"} + query,
        std::string{"/api/v1/operations/runtime/operation-1"} + query, std::string{"/api/v1/logs"},
        std::string{"/api/v1/audit"}, model_path(),
        std::string{"/api/v1/models"} + model_scope_query(), action_scope_path()}) {
    const auto result = fixture.gateway_.handle(request("HEAD", path));
    require(result && result->status == 200 && !result->body.empty(),
            "gateway HEAD preserves GET body for HTTPS suppression: " + path);
  }
  for (const auto& path : {"/unknown", "/api/v1/auth/me", "/api/v1/accounts/account-1",
                           "/api/v1/installations/installation-1", "/api/v1/tasks"}) {
    const int calls = fixture.delegation_.calls;
    require(!fixture.gateway_.handle(request("PATCH", path)) && fixture.delegation_.calls == calls,
            "unmatched method/path stays available to another handler");
  }
  auto nested_action = request("GET", action_scope_path("action-1/nested"));
  require(!fixture.gateway_.handle(nested_action), "nested Action selector remains unmatched");
  const auto unsupported_head =
      fixture.gateway_.handle(request("HEAD", model_path("/configuration")));
  require(unsupported_head && unsupported_head->status == 503,
          "configuration HEAD keeps existing capability response");
  const auto wrong_method = fixture.gateway_.handle(request("POST", action_scope_path()));
  require(wrong_method && wrong_method->status == 405, "Action method mismatch stays 405");
  fixture.delegation_.throws_ = true;
  const auto exception = fixture.gateway_.handle(request("GET", action_scope_path()));
  require(exception && exception->status == 500 &&
              nlohmann::json::parse(exception->body)["code"] == "INTERNAL" &&
              exception->body.find("private exception") == std::string::npos,
          "top-level boundary converts unexpected exceptions without leaking details");
}
void pagination_and_request_context() {
  RoutedOwners fixture;
  auto action = route_request(owner_routes().back());
  require(fixture.gateway_.handle(action)->status == 200, "Action request context forwarded");
  const auto& context = fixture.runtime_.last_request.context();
  require(
      context.request_id() == "request-123" &&
          context.trace_id() == "0123456789abcdef0123456789abcdef" &&
          context.deadline().seconds() ==
              std::chrono::duration_cast<std::chrono::seconds>(action.deadline.time_since_epoch())
                  .count() &&
          context.deadline().nanos() == action.deadline.time_since_epoch().count() % 1'000'000'000,
      "Action owner receives the exact identifiers and unextended deadline");
  for (const auto& size : {"0", "1", "100", "101", "4294967295", "4294967296", "1x", "-1"}) {
    const auto principals = fixture.gateway_.handle(
        request("GET", "/api/v1/principals?page_size=" + std::string{size}));
    const auto tasks =
        fixture.gateway_.handle(request("GET", "/api/v1/tasks?page_size=" + std::string{size}));
    const auto models = fixture.gateway_.handle(
        request("GET", "/api/v1/models" + model_scope_query() + "&page_size=" + size));
    const bool integer = std::string_view{size} == "0" || std::string_view{size} == "1" ||
                         std::string_view{size} == "100" || std::string_view{size} == "101" ||
                         std::string_view{size} == "4294967295";
    require(principals &&
                principals->status == (integer && std::string_view{size} != "0" ? 200 : 400),
            "principal pagination keeps owner validation range");
    require(tasks && tasks->status == (integer ? 200 : 400),
            "task pagination keeps owner validation range");
    require(models &&
                models->status ==
                    (std::string_view{size} == "1" || std::string_view{size} == "100" ? 200 : 400),
            "model pagination keeps Control bound");
  }
  const auto bad_query =
      fixture.gateway_.handle(request("GET", "/api/v1/tasks?page_size=1&page_size=2"));
  require(bad_query && bad_query->status == 400, "duplicate query fields remain invalid");
  require(fixture.gateway_.handle(request("GET", "/api/v1/principals"))->status == 200 &&
              !fixture.principals_.last_request.has_page(),
          "omitted page remains absent after shared pagination parsing");
}

void model_mutation_idempotency_context() {
  for (const auto& route : owner_routes()) {
    if (route.path_ != "/api/v1/models/model-1/disable" &&
        route.path_ != "/api/v1/models/model-1/enable" &&
        route.path_ != model_path("/calls/call-1/resolve")) {
      continue;
    }
    RoutedOwners fixture;
    auto input = route_request(route);
    input.headers.erase("idempotency-key");
    input.headers["Idempotency-Key"] = "model-mutation-exact-key";
    const auto first = fixture.gateway_.handle(input);
    const auto second = fixture.gateway_.handle(input);
    require(first && second && first->status == 200 && second->status == 200 &&
                first->body == second->body && fixture.model_.calls == 2 &&
                fixture.delegation_.calls == 2,
            "model mutation retries delegate again and forward to the owner");
    const auto& context =
        route.path_.ends_with("/disable")  ? fixture.model_.disable_request.context()
        : route.path_.ends_with("/enable") ? fixture.model_.enable_request.context()
                                           : fixture.model_.resolve_request.context();
    require(context.idempotency_key() == "model-mutation-exact-key" &&
                context.request_id() == "request-123" &&
                context.trace_id() == "0123456789abcdef0123456789abcdef",
            "model mutation forwards the exact case-insensitive header and correlation");
    input.headers.erase("Idempotency-Key");
    require_failure(fixture.gateway_.handle(input), wire::INVALID_ARGUMENT, false,
                    "missing model mutation key rejected");
    input.headers["Idempotency-Key"] = "";
    require_failure(fixture.gateway_.handle(input), wire::INVALID_ARGUMENT, false,
                    "empty model mutation key rejected");
    require(fixture.model_.calls == 2 && fixture.delegation_.calls == 2,
            "missing/empty mutation keys do not delegate or invoke the owner");
  }
}
void missing_nested_instance_owner() {
  for (const auto& route : owner_routes()) {
    if (!route.path_.starts_with("/api/v1/installations/installation-1/instances")) {
      continue;
    }
    FakeDelegation delegation;
    FakeRuntime runtime;
    target::OwnerGateway gateway{delegation, runtime};
    auto input = route_request(route);
    require_failure(gateway.handle(input), wire::UPSTREAM_UNAVAILABLE, true,
                    "missing nested instance owner is a stable dependency error");
    require(delegation.calls == 1 && runtime.calls == 0,
            "missing owner preserves delegation before dependency failure");
    delegation.failure = target::OwnerFailure{wire::UNAUTHENTICATED, false};
    input.access_token.clear();
    require_failure(gateway.handle(input), wire::UNAUTHENTICATED, false,
                    "nested instance authentication rejection precedes owner absence");
    input.access_token = "access-token";
    delegation.failure = target::OwnerFailure{wire::PERMISSION_DENIED, false};
    require_failure(gateway.handle(input), wire::PERMISSION_DENIED, false,
                    "nested instance permission rejection precedes owner absence");
    delegation.failure.reset();
    if (route.method_ != "GET") {
      input.body = "{";
      require_failure(gateway.handle(input), wire::INVALID_ARGUMENT, false,
                      "nested instance malformed body precedes owner absence");
      // Configuration historically authenticates before parsing its mutation body. Other
      // nested writes validate their shape first; the dependency guard must preserve both.
      delegation.failure = target::OwnerFailure{wire::UNAUTHENTICATED, false};
      const auto expected =
          route.path_.ends_with("/configuration") ? wire::UNAUTHENTICATED : wire::INVALID_ARGUMENT;
      require_failure(gateway.handle(input), expected, false,
                      "nested instance input/auth ordering survives the dependency guard");
    }
    require(runtime.calls == 0, "missing resources never invokes an unrelated owner");
  }
}

struct FakeHost final : target::HostManagementPort {
  std::optional<target::OwnerFailure> failure;
  int calls = 0;
  std::string method;
  wire::HostBeginMaintenanceRequest begin_request;
  wire::HostCancelOperationRequest cancel_request;
  wire::ResourceScope last_scope;
  target::OwnerOutcome<wire::HostGetMaintenanceResponse>
  get_maintenance(const wire::HostGetMaintenanceRequest& value,
                  std::string_view delegation) override {
    ++calls;
    method = "get";
    require(delegation == "delegation-proof", "host receives issued delegation");
    require(value.host_id() == "11111111-1111-4111-8111-111111111111",
            "host maintenance host id forwarded");
    if (failure) {
      return *failure;
    }
    wire::HostGetMaintenanceResponse output;
    auto* record = output.mutable_maintenance();
    record->set_host_id("11111111-1111-4111-8111-111111111111");
    record->set_version(4);
    record->set_admission(wire::HOST_ADMISSION_CLOSED);
    record->mutable_operation()->set_owner_service("host");
    record->mutable_operation()->set_operation_id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    record->mutable_observed_at()->set_seconds(1'700'000'000);
    return output;
  }
  target::OwnerOutcome<wire::HostMaintenanceResponse>
  begin_maintenance(const wire::HostBeginMaintenanceRequest& value,
                    std::string_view delegation) override {
    ++calls;
    method = "begin";
    require(delegation == "delegation-proof", "host receives issued delegation");
    require(value.host_id() == "11111111-1111-4111-8111-111111111111" &&
                value.has_expected_version() && value.expected_version() == 3 &&
                value.grace_ms() == 500,
            "begin maintenance body forwarded exactly");
    begin_request = value;
    if (failure) {
      return *failure;
    }
    wire::HostMaintenanceResponse output;
    auto* record = output.mutable_maintenance();
    record->set_host_id("11111111-1111-4111-8111-111111111111");
    record->set_version(4);
    record->set_admission(wire::HOST_ADMISSION_CLOSED);
    record->mutable_observed_at()->set_seconds(1'700'000'000);
    auto* operation = output.mutable_operation();
    operation->mutable_ref()->set_owner_service("host");
    operation->mutable_ref()->set_operation_id("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    *operation->mutable_ref()->mutable_resource() = value_host();
    operation->set_version(1);
    operation->set_state(wire::OPERATION_ACCEPTED);
    operation->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation->mutable_observed_at()->set_seconds(1'700'000'000);
    return output;
  }
  target::OwnerOutcome<wire::HostMaintenanceResponse>
  resume_admission(const wire::HostResumeAdmissionRequest& value,
                   std::string_view delegation) override {
    ++calls;
    method = "resume";
    require(delegation == "delegation-proof", "host receives issued delegation");
    require(value.host_id() == "11111111-1111-4111-8111-111111111111" &&
                value.has_expected_version() && value.expected_version() == 4,
            "resume admission body forwarded exactly");
    if (failure) {
      return *failure;
    }
    wire::HostMaintenanceResponse output;
    auto* record = output.mutable_maintenance();
    record->set_host_id("11111111-1111-4111-8111-111111111111");
    record->set_version(5);
    record->set_admission(wire::HOST_ADMISSION_OPEN);
    record->mutable_observed_at()->set_seconds(1'700'000'000);
    auto* operation = output.mutable_operation();
    operation->mutable_ref()->set_owner_service("host");
    operation->mutable_ref()->set_operation_id("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    operation->set_version(1);
    operation->set_state(wire::OPERATION_SUCCEEDED);
    operation->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation->mutable_observed_at()->set_seconds(1'700'000'000);
    return output;
  }
  target::OwnerOutcome<wire::HostGetOperationResponse>
  get_operation(const wire::HostGetOperationRequest&, std::string_view) override {
    ++calls;
    method = "get_operation";
    return target::OwnerFailure{wire::CAPABILITY_NOT_SUPPORTED, false};
  }
  target::OwnerOutcome<wire::HostGetOperationResponse>
  cancel_operation(const wire::HostCancelOperationRequest& value,
                   std::string_view delegation) override {
    ++calls;
    method = "cancel";
    require(delegation == "delegation-proof", "host receives issued delegation");
    require(value.operation().operation_id() == "dddddddd-dddd-4ddd-8ddd-dddddddddddd" &&
                value.expected_version() == 2 && value.operation().owner_service() == "host",
            "cancel operation body forwarded exactly");
    cancel_request = value;
    if (failure) {
      return *failure;
    }
    wire::HostGetOperationResponse output;
    auto* operation = output.mutable_operation();
    operation->mutable_ref()->set_owner_service("host");
    operation->mutable_ref()->set_operation_id(value.operation().operation_id());
    operation->set_version(2);
    operation->set_state(wire::OPERATION_CANCELLED);
    operation->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation->mutable_observed_at()->set_seconds(1'700'000'000);
    return output;
  }
  target::OwnerOutcome<wire::OperationChangesResponse>
  list_operation_changes(const wire::OperationChangesRequest&, std::string_view) override {
    ++calls;
    method = "changes";
    return target::OwnerOutcome<wire::OperationChangesResponse>{wire::OperationChangesResponse{}};
  }
  static wire::ResourceRef value_host() {
    wire::ResourceRef resource;
    resource.set_kind(wire::RESOURCE_HOST);
    resource.set_id("11111111-1111-4111-8111-111111111111");
    return resource;
  }
};

std::string host_scope_json() {
  return R"({"resources":[{"kind":"RESOURCE_HOST","id":"11111111-1111-4111-8111-111111111111"}]})";
}
std::string host_query() {
  return "?scope=" + host_scope_json();
}

void host_management_surface() {
  FakeDelegation delegation;
  FakeRuntime runtime;
  FakeHost host;
  FakeOperations operations;
  target::OwnerGateway gateway{delegation,  runtime, nullptr, nullptr, nullptr, nullptr,
                               &operations, nullptr, nullptr, nullptr, nullptr, &host};

  auto host_request = [](std::string method, std::string path) {
    auto value = request(std::move(method), std::move(path));
    value.headers["traceparent"] = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";
    return value;
  };
  auto maintenance = gateway.handle(
      host_request("GET", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/maintenance"));
  require(maintenance && maintenance->status == 200 &&
              maintenance->body.find("\"host_id\":\"11111111-1111-4111-8111-111111111111\"") !=
                  std::string::npos &&
              maintenance->body.find("\"version\":\"4\"") != std::string::npos &&
              maintenance->body.find("\"admission\":\"HOST_ADMISSION_CLOSED\"") !=
                  std::string::npos &&
              maintenance->body.find("\"observed_at\"") != std::string::npos,
          "host maintenance read projects the owner record");
  require(delegation.calls == 1 && delegation.audience == "host" &&
              delegation.permission == wire::PERMISSION_HOST_READ &&
              delegation.last_scope.resources_size() == 1 &&
              delegation.last_scope.resources(0).kind() == wire::RESOURCE_HOST &&
              delegation.last_scope.resources(0).id() == "11111111-1111-4111-8111-111111111111",
          "host maintenance read uses one exact host.read delegation");

  auto begin_request =
      host_request("POST", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/maintenance");
  begin_request.body = R"({"expected_version":"3","grace_ms":500})";
  const auto begin = gateway.handle(begin_request);
  require(begin && begin->status == 202 &&
              begin->body.find("\"state\":\"OPERATION_ACCEPTED\"") != std::string::npos &&
              begin->body.find("\"maintenance\":{") != std::string::npos,
          "accepted maintenance begin is 202 with owner operation, not completed work");
  require(delegation.calls == 2 && delegation.audience == "host" &&
              delegation.permission == wire::PERMISSION_HOST_EXECUTE,
          "begin maintenance delegation is host.execute");

  auto resume_request =
      host_request("POST", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/resume");
  resume_request.body = R"({"expected_version":"4"})";
  const auto resume = gateway.handle(resume_request);
  require(resume && resume->status == 200 &&
              resume->body.find("\"state\":\"OPERATION_SUCCEEDED\"") != std::string::npos,
          "completed resume is 200 with the owner snapshot");
  require(delegation.calls == 3 && delegation.permission == wire::PERMISSION_HOST_EXECUTE,
          "resume admission delegation is host.execute");

  auto missing_key =
      host_request("POST", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/maintenance");
  missing_key.headers.erase("idempotency-key");
  missing_key.body = R"({"expected_version":"3"})";
  require_failure(gateway.handle(missing_key), wire::INVALID_ARGUMENT, false,
                  "host write requires idempotency key");
  require(host.calls == 3 && delegation.calls == 3,
          "invalid host write never reaches owner or delegation");

  auto missing_version =
      host_request("POST", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/resume");
  missing_version.body = R"({})";
  require_failure(gateway.handle(missing_version), wire::INVALID_ARGUMENT, false,
                  "resume requires expected version");

  require_failure(gateway.handle(host_request("GET", "/api/v1/hosts/not-a-uuid/maintenance")),
                  wire::INVALID_ARGUMENT, false, "malformed host id rejected before delegation");

  host.failure = target::OwnerFailure{wire::CONFLICT, false};
  require_failure(gateway.handle(host_request(
                      "GET", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/maintenance")),
                  wire::CONFLICT, false, "host conflict maps to stable 409");
  host.failure.reset();
  host.failure = target::OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
  require_failure(gateway.handle(host_request(
                      "GET", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/maintenance")),
                  wire::UPSTREAM_UNAVAILABLE, true, "host outage stays retryable 503");
  host.failure.reset();

  target::OwnerGateway no_host{delegation, runtime};
  require_failure(no_host.handle(host_request(
                      "GET", "/api/v1/hosts/11111111-1111-4111-8111-111111111111/maintenance")),
                  wire::UPSTREAM_UNAVAILABLE, true,
                  "missing registered host endpoint fails closed, never fabricated state");

  auto cancel =
      host_request("POST", "/api/v1/operations/host/dddddddd-dddd-4ddd-8ddd-dddddddddddd/cancel");
  cancel.body = R"({"expected_version":"2","scope":)" + host_scope_json() + "}";
  const auto cancelled = gateway.handle(cancel);
  require(cancelled && cancelled->status == 200 &&
              cancelled->body.find("\"state\":\"OPERATION_CANCELLED\"") != std::string::npos,
          "host cancellation forwards the owner snapshot");
  require(host.cancel_request.operation().resource().kind() == wire::RESOURCE_HOST &&
              host.cancel_request.operation().resource().id() ==
                  "11111111-1111-4111-8111-111111111111" &&
              delegation.permission == wire::PERMISSION_HOST_EXECUTE,
          "cancel scope is forwarded exactly under host.execute");

  auto bad_scope =
      host_request("POST", "/api/v1/operations/host/dddddddd-dddd-4ddd-8ddd-dddddddddddd/cancel");
  bad_scope.body =
      R"({"expected_version":"2","scope":{"resources":[{"kind":"RESOURCE_ACCOUNT","id":"11111111-1111-4111-8111-111111111111"}]}})";
  require_failure(gateway.handle(bad_scope), wire::INVALID_ARGUMENT, false,
                  "cancel scope must be exactly one RESOURCE_HOST");

  const auto operation = gateway.handle(host_request(
      "GET", "/api/v1/operations/host/eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee" + host_query()));
  require(operation && operation->status == 200 && operations.owner == "host",
          "host owner operation routes through the operation port");
  require(delegation.permission == wire::PERMISSION_HOST_READ,
          "host operation query uses host.read");
  require_failure(
      gateway.handle(host_request(
          "GET", "/api/v1/operations/host/"
                 "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee?scope=" +
                     std::string{R"({"resources":[{"kind":"RESOURCE_MODEL","id":"model-1"}]})"})),
      wire::INVALID_ARGUMENT, false, "host operation query rejects a non-host scope");

  require(
      !gateway.handle(host_request("GET", "/api/v1/hosts/11111111-1111-4111-8111-111111111111")),
      "plain host projection is not an owner-gateway route");
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
    owner_route_inventory();
    host_management_surface();
    head_and_unmatched_routes();
    pagination_and_request_context();
    model_mutation_idempotency_context();
    missing_nested_instance_owner();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "control owner gateway: ok\n";
  return 0;
}
