// P12f fake Runtime/Scheduler owner services for the real Control acceptance matrix.
// Both services listen on one mutually-authenticated TLS endpoint; responses are typed, canonical
// and only cover the paths Control forwards. Marker ids containing "conflict"/"unavailable" force
// stable owner failures so the Control error mapping can be asserted without mutating fixtures.
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"

namespace {
namespace wire = kiarabot::v1;
constexpr std::int64_t kNow = 1700000000;
std::string read(const char* path) {
  std::ifstream input(path);
  std::ostringstream result;
  result << input.rdbuf();
  return result.str();
}
grpc::Status failure(wire::ErrorCode code, grpc::StatusCode transport,
                     const wire::RequestContext& context) {
  wire::RpcFailure rpc;
  rpc.mutable_context()->set_acceptance(wire::NOT_ACCEPTED);
  rpc.mutable_context()->set_recovery(wire::DO_NOT_RETRY);
  rpc.mutable_error()->set_code(code);
  rpc.mutable_error()->set_request_id(context.request_id());
  rpc.mutable_error()->set_trace_id(context.trace_id());
  rpc.mutable_error()->set_message("fixture-owner-error");
  return {transport, "fixture-owner-error", rpc.SerializeAsString()};
}
bool marked(const std::string& value, std::string_view marker) {
  return value.find(marker) != std::string::npos;
}
grpc::Status marker_status(const std::string& value, const wire::RequestContext& context,
                           bool* handled) {
  *handled = false;
  if (marked(value, "conflict")) {
    *handled = true;
    return failure(wire::CONFLICT, grpc::StatusCode::ABORTED, context);
  }
  if (marked(value, "unavailable")) {
    *handled = true;
    return failure(wire::UPSTREAM_UNAVAILABLE, grpc::StatusCode::UNAVAILABLE, context);
  }
  return grpc::Status::OK;
}
void operation(wire::OperationRef* output, std::string_view id, std::string_view owner,
               const wire::ResourceRef& resource) {
  output->set_owner_service(std::string{owner});
  output->set_operation_id(std::string{id});
  *output->mutable_resource() = resource;
}
class FakeRuntime final : public wire::RuntimeService::Service {
public:
  grpc::Status QueryActionResult(grpc::ServerContext*,
                                 const wire::RuntimeQueryActionResultRequest* input,
                                 wire::RuntimeQueryActionResultResponse* output) override {
    bool handled = false;
    auto status =
        marker_status(input->action_id() + input->owner_instance_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* result = output->mutable_result();
    result->set_action_id(input->action_id().empty() ? "action-fixture" : input->action_id());
    result->set_status(wire::SUCCEEDED);
    result->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    result->set_platform_message_id("platform-message-fixture");
    result->mutable_completed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status BindAdapter(grpc::ServerContext*, const wire::RuntimeBindAdapterRequest* input,
                           wire::RuntimeBindAdapterResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->binding().account_id(), input->context(), &handled);
    if (handled)
      return status;
    *output->mutable_binding() = input->binding();
    operation(output->mutable_operation(), "bind-operation", "runtime", {});
    return grpc::Status::OK;
  }
  grpc::Status UnbindAdapter(grpc::ServerContext*, const wire::RuntimeUnbindAdapterRequest* input,
                             wire::RuntimeUnbindAdapterResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->binding().account_id(), input->context(), &handled);
    if (handled)
      return status;
    operation(output->mutable_operation(), "unbind-operation", "runtime", {});
    return grpc::Status::OK;
  }
  grpc::Status CreateInstallation(grpc::ServerContext*,
                                  const wire::RuntimeCreateInstallationRequest* input,
                                  wire::RuntimeCreateInstallationResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->plugin_type_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* installation = output->mutable_installation();
    installation->set_installation_id("installation-fixture");
    installation->set_plugin_type_id(input->plugin_type_id());
    return grpc::Status::OK;
  }
  grpc::Status CreateInstance(grpc::ServerContext*, const wire::RuntimeCreateInstanceRequest* input,
                              wire::RuntimeCreateInstanceResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->installation_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* instance = output->mutable_instance();
    instance->set_instance_id("instance-fixture");
    instance->set_installation_id(input->installation_id());
    instance->set_host_id(input->host_id());
    instance->set_externally_managed(input->externally_managed());
    instance->mutable_state()->set_version(1);
    instance->mutable_state()->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    instance->mutable_state()->set_desired(wire::DESIRED_RUNNING);
    instance->mutable_state()->set_registered(false);
    instance->mutable_state()->set_process_outcome(wire::EXECUTION_NOT_STARTED);
    instance->mutable_state()->mutable_observed_at()->set_seconds(kNow);
    instance->mutable_state()->set_source_service("runtime");
    return grpc::Status::OK;
  }
  grpc::Status SetDesiredState(grpc::ServerContext*,
                               const wire::RuntimeSetDesiredStateRequest* input,
                               wire::RuntimeSetDesiredStateResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->instance_id(), input->context(), &handled);
    if (handled)
      return status;
    auto* instance = output->mutable_instance();
    instance->set_instance_id(input->instance_id());
    instance->set_installation_id("installation-fixture");
    instance->set_host_id("host-fixture");
    instance->set_externally_managed(false);
    auto* state = instance->mutable_state();
    state->mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
    state->mutable_resource()->set_id(input->instance_id());
    state->set_version(2);
    state->set_lifecycle(wire::LIFECYCLE_ACTIVE);
    state->set_desired(input->desired_state());
    state->set_registered(true);
    state->set_desired_config_version(2);
    state->set_applied_config_version(1);
    state->set_process_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    state->mutable_observed_at()->set_seconds(kNow);
    state->set_source_service("host");
    operation(output->mutable_operation(), "desired-state-operation", "runtime", state->resource());
    return grpc::Status::OK;
  }
  grpc::Status UpdateConfig(grpc::ServerContext*, const wire::RuntimeUpdateConfigRequest* input,
                            wire::RuntimeUpdateConfigResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    auto* state = output->mutable_state();
    *state->mutable_resource() = input->resource();
    state->set_version(3);
    state->mutable_observed_at()->set_seconds(kNow);
    operation(output->mutable_operation(), "config-operation", "runtime", input->resource());
    return grpc::Status::OK;
  }
  grpc::Status GetInstanceConfig(grpc::ServerContext*,
                                 const wire::RuntimeGetInstanceConfigRequest* input,
                                 wire::RuntimeGetInstanceConfigResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    auto* config = output->mutable_config();
    *config->mutable_instance() = input->resource();
    config->set_desired_version(2);
    config->set_applied_version(1);
    config->set_apply_state(wire::CONFIG_APPLY_PENDING);
    config->set_delivery_id("delivery-fixture");
    config->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status GetActionPolicy(grpc::ServerContext*,
                               const wire::RuntimeGetActionPolicyRequest* input,
                               wire::RuntimeGetActionPolicyResponse* output) override {
    auto* policy = output->mutable_policy();
    policy->set_account_id(input->account_id());
    policy->set_mute_enabled(true);
    policy->add_allowed_group_ids("group-fixture");
    policy->set_version(4);
    return grpc::Status::OK;
  }
  grpc::Status UpdateActionPolicy(grpc::ServerContext*,
                                  const wire::RuntimeUpdateActionPolicyRequest* input,
                                  wire::RuntimeUpdateActionPolicyResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->policy().account_id(), input->context(), &handled);
    if (handled)
      return status;
    *output->mutable_policy() = input->policy();
    output->mutable_policy()->set_version(5);
    return grpc::Status::OK;
  }
  grpc::Status BeginDelete(grpc::ServerContext*, const wire::RuntimeBeginDeleteRequest* input,
                           wire::RuntimeBeginDeleteResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    operation(output->mutable_operation(), "delete-operation", "runtime", input->resource());
    return grpc::Status::OK;
  }
  grpc::Status RestoreResource(grpc::ServerContext*,
                               const wire::RuntimeRestoreResourceRequest* input,
                               wire::RuntimeRestoreResourceResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->resource().id(), input->context(), &handled);
    if (handled)
      return status;
    operation(output->mutable_operation(), "restore-operation", "runtime", input->resource());
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext*, const wire::RuntimeGetOperationRequest* input,
                            wire::RuntimeGetOperationResponse* output) override {
    auto* operation_out = output->mutable_operation();
    *operation_out->mutable_ref() = input->operation();
    operation_out->set_version(7);
    operation_out->set_state(wire::OPERATION_BLOCKED);
    operation_out->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    auto* participant = operation_out->add_participants();
    participant->set_participant("runtime");
    participant->set_state(wire::OPERATION_BLOCKED);
    participant->mutable_next_attempt_at()->set_seconds(kNow + 30);
    operation_out->set_stalled(false);
    return grpc::Status::OK;
  }
  grpc::Status ListAudit(grpc::ServerContext*, const wire::RuntimeListAuditRequest* input,
                         wire::RuntimeListAuditResponse* output) override {
    auto* row = output->add_entries();
    row->set_entry_id("aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa");
    row->set_actor("runtime-fixture");
    row->mutable_resource()->set_kind(wire::RESOURCE_ACCOUNT);
    row->mutable_resource()->set_id("11111111-1111-4111-8111-111111111111");
    row->set_request_id(input->context().request_id());
    row->set_trace_id(input->context().trace_id());
    row->set_method("fixture.runtime.audit");
    row->set_authorization_version(18446744073709551615ULL);
    row->mutable_occurred_at()->set_seconds(kNow);
    output->mutable_page()->set_page_size(input->filter().page().page_size());
    output->mutable_checked_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
  grpc::Status ListLogs(grpc::ServerContext*, const wire::RuntimeListLogsRequest* input,
                        wire::RuntimeListLogsResponse* output) override {
    auto* row = output->add_entries();
    row->set_entry_id("bbbbbbbb-1111-4111-8111-bbbbbbbbbbbb");
    row->set_severity("info");
    row->set_component("runtime-fixture");
    row->set_method("fixture.runtime.log");
    row->set_actor("runtime-fixture");
    row->mutable_resource()->set_kind(wire::RESOURCE_ACCOUNT);
    row->mutable_resource()->set_id("11111111-1111-4111-8111-111111111111");
    row->set_request_id(input->context().request_id());
    row->set_trace_id(input->context().trace_id());
    row->set_message("fixture runtime log");
    row->mutable_occurred_at()->set_seconds(kNow);
    output->mutable_page()->set_page_size(input->filter().page().page_size());
    output->mutable_checked_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
};
void task(wire::TaskRecord* output, const std::string& id) {
  output->set_task_id(id);
  output->set_owner_instance_id("22222222-2222-4222-8222-222222222222");
  output->set_installation_id("installation-fixture");
  output->mutable_target()->set_id("11111111-1111-4111-8111-111111111111");
  output->set_state(wire::SCHEDULED_TASK_RUNNING);
  output->set_version(2);
  output->mutable_created_at()->set_seconds(kNow);
  output->mutable_updated_at()->set_seconds(kNow);
}
class FakeScheduler final : public wire::SchedulerService::Service {
public:
  grpc::Status ListTasks(grpc::ServerContext*, const wire::SchedulerListTasksRequest* input,
                         wire::SchedulerListTasksResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->context().request_id(), input->context(), &handled);
    if (handled)
      return status;
    task(output->add_tasks(), "task-fixture");
    output->mutable_page()->set_page_size(input->page().page_size());
    return grpc::Status::OK;
  }
  grpc::Status GetTask(grpc::ServerContext*, const wire::SchedulerGetTaskRequest* input,
                       wire::SchedulerGetTaskResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->task_id(), input->context(), &handled);
    if (handled)
      return status;
    task(output->mutable_task(), input->task_id());
    return grpc::Status::OK;
  }
  grpc::Status CancelTask(grpc::ServerContext*, const wire::SchedulerCancelTaskRequest* input,
                          wire::SchedulerCancelTaskResponse* output) override {
    bool handled = false;
    auto status = marker_status(input->task_id(), input->context(), &handled);
    if (handled)
      return status;
    task(output->mutable_task(), input->task_id());
    output->mutable_task()->set_state(wire::SCHEDULED_TASK_CANCELLED);
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext*, const wire::SchedulerGetOperationRequest* input,
                            wire::SchedulerGetOperationResponse* output) override {
    auto* operation_out = output->mutable_operation();
    *operation_out->mutable_ref() = input->operation();
    operation_out->set_version(1);
    operation_out->set_state(wire::OPERATION_SUCCEEDED);
    operation_out->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    operation_out->mutable_observed_at()->set_seconds(kNow);
    return grpc::Status::OK;
  }
};
} // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    return 2;
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = read(argv[1]);
  options.pem_key_cert_pairs.push_back({read(argv[3]), read(argv[2])});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  FakeRuntime runtime;
  FakeScheduler scheduler;
  builder.RegisterService(&runtime);
  builder.RegisterService(&scheduler);
  auto server = builder.BuildAndStart();
  if (!server) {
    return 1;
  }
  std::cout << port << '\n' << std::flush;
  server->Wait();
}
