#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <google/protobuf/util/time_util.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/scheduler.grpc.pb.h>

#include "kiarabot/core/runtime/platform/scheduler_rpc.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace v1 = kiarabot::v1;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("scheduler rpc assertion failed at line " +
                             std::to_string(location.line()));
  }
}
std::string uuid(char seed) {
  std::string text(36, '0');
  for (const int index : {8, 13, 18, 23}) {
    text[static_cast<std::size_t>(index)] = '-';
  }
  text[0] = seed;
  text[14] = '4';
  text[19] = '8';
  return text;
}
t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
class GrpcLifetime {
public:
  GrpcLifetime() {
    grpc_init();
  }
  ~GrpcLifetime() {
    grpc_shutdown();
  }
};

class FakeHandler final : public t::SchedulerCommandHandler {
public:
  t::Outcome<t::ScheduledTask> create_task(const t::SchedulerCommandContext& context,
                                           const t::TaskCreationInput& input) override {
    last_context = context;
    last_input = input;
    create_calls += 1;
    if (create_result) {
      return *create_result;
    }
    t::ScheduledTask task;
    task.key = t::TaskKey{context.owner, input.task_id};
    task.installation = context.installation;
    task.target = input.target;
    task.payload = input.payload;
    task.run_at = input.run_at.value_or(t::TimePoint{std::chrono::seconds{1000}});
    task.state = t::TaskState::Pending;
    task.version = t::ResourceVersion{1};
    task.occurrence = t::MessageId{uuid('e')};
    task.created_at = t::TimePoint{std::chrono::seconds{900}};
    task.updated_at = t::TimePoint{std::chrono::seconds{901}};
    return task;
  }
  t::Outcome<t::ScheduledTask> cancel_task(const t::SchedulerCommandContext& context,
                                           std::string_view task_id) override {
    last_context = context;
    cancel_calls += 1;
    if (cancel_result) {
      return *cancel_result;
    }
    t::TaskCreationInput input;
    input.task_id = std::string(task_id);
    input.target = t::ConversationKey{{}, {}, "conversation", t::ConversationKey::Kind::Private};
    return create_task(context, input);
  }
  t::Outcome<t::ScheduledTask> get_task(const t::SchedulerCommandContext& context,
                                        std::string_view task_id) override {
    last_context = context;
    get_calls += 1;
    if (get_result) {
      return *get_result;
    }
    if (task_id.empty()) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    return failure(t::ErrorCode::NotFound, t::Recovery::ReadCurrent);
  }
  t::Outcome<t::SchedulerListPage> list_tasks(const t::SchedulerCommandContext& context,
                                              std::optional<t::TaskState> state,
                                              std::uint32_t page_size,
                                              std::string_view page_token) override {
    last_context = context;
    last_page_size = page_size;
    last_page_token = std::string(page_token);
    last_state = state;
    list_calls += 1;
    t::SchedulerListPage page;
    t::ScheduledTask task;
    task.key = t::TaskKey{context.owner, "task-list"};
    task.installation = context.installation;
    task.target = t::ConversationKey{{}, {}, "conversation", t::ConversationKey::Kind::Private};
    task.run_at = t::TimePoint{std::chrono::seconds{1000}};
    task.created_at = t::TimePoint{std::chrono::seconds{900}};
    task.updated_at = t::TimePoint{std::chrono::seconds{901}};
    page.tasks.push_back(task);
    page.next_page_token = "next-token";
    return page;
  }
  t::Outcome<t::Occurrence> get_occurrence(const t::SchedulerCommandContext& context,
                                           std::string_view occurrence_id) override {
    last_context = context;
    occurrence_calls += 1;
    if (occurrence_result) {
      return *occurrence_result;
    }
    t::Occurrence occurrence;
    occurrence.owner = context.owner;
    occurrence.installation = context.installation;
    occurrence.task_id = "task-1";
    occurrence.occurrence_id = t::MessageId{std::string(occurrence_id)};
    occurrence.due_at = t::TimePoint{std::chrono::seconds{1000}};
    occurrence.created_at = t::TimePoint{std::chrono::seconds{900}};
    occurrence.updated_at = t::TimePoint{std::chrono::seconds{901}};
    occurrence.handoff.source = t::Service::Scheduler;
    occurrence.handoff.message = occurrence.occurrence_id;
    occurrence.handoff.operation = t::OperationRef{t::Service::Scheduler, t::OperationId{uuid('d')},
                                                   t::ResourceRef{t::ResourceKind::Task, "task-1"}};
    occurrence.handoff.resource = t::ResourceRef{t::ResourceKind::Task, "task-1"};
    occurrence.handoff.version = t::ResourceVersion{1};
    occurrence.handoff.actor = t::SubjectId{context.owner.value};
    occurrence.handoff.authorization_version = t::ResourceVersion{1};
    occurrence.handoff.created_at = occurrence.created_at;
    occurrence.handoff.valid_until = t::TimePoint{std::chrono::seconds{100000}};
    return occurrence;
  }
  t::Outcome<t::Operation> get_operation(const t::SchedulerCommandContext& context,
                                         const t::OperationRef& operation) override {
    last_context = context;
    operation_calls += 1;
    if (operation.owner != t::Service::Scheduler) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    t::Operation value;
    value.ref = operation;
    value.version = t::ResourceVersion{1};
    value.state = t::OperationState::Accepted;
    value.execution_outcome = t::ExecutionOutcome::NotStarted;
    return value;
  }

  std::optional<t::Outcome<t::ScheduledTask>> create_result;
  std::optional<t::Outcome<t::ScheduledTask>> cancel_result;
  std::optional<t::Outcome<t::ScheduledTask>> get_result;
  std::optional<t::Outcome<t::Occurrence>> occurrence_result;
  t::SchedulerCommandContext last_context;
  t::TaskCreationInput last_input;
  std::uint32_t last_page_size = 0;
  std::string last_page_token;
  std::optional<t::TaskState> last_state;
  int create_calls = 0, cancel_calls = 0, get_calls = 0, list_calls = 0, occurrence_calls = 0,
      operation_calls = 0;
};

class FakeAuthorizer {
public:
  t::Outcome<t::SchedulerCommandContext> operator()(grpc::ServerContext&, const v1::RequestContext&,
                                                    std::string_view method,
                                                    const v1::ResourceScope* scope) {
    last_method = std::string(method);
    ++calls;
    last_scope_resource =
        scope != nullptr && scope->resources_size() > 0 ? scope->resources(0).id() : std::string{};
    if (last_scope_resource == "denied") {
      return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
    }
    t::SchedulerCommandContext context;
    context.owner = t::InstanceId{uuid('a')};
    context.installation = t::InstallationId{uuid('b')};
    return context;
  }
  int calls = 0;
  std::string last_scope_resource;
  std::string last_method;
};

bool fill_context(v1::RequestContext* context, std::string key = std::string(16, 'k')) {
  context->set_request_id(uuid('1'));
  context->set_trace_id(uuid('2'));
  const auto seconds = google::protobuf::util::TimeUtil::TimestampToSeconds(
                           google::protobuf::util::TimeUtil::GetCurrentTime()) +
                       4;
  context->mutable_deadline()->CopyFrom(
      google::protobuf::util::TimeUtil::SecondsToTimestamp(seconds));
  if (!key.empty()) {
    context->set_idempotency_key(std::move(key));
  }
  return true;
}
void fill_target(v1::ConversationRef* target) {
  target->set_id("conversation");
  target->set_type(v1::ConversationRef::PRIVATE);
}
v1::SchedulerCreateTaskRequest create_request(std::string key = std::string(16, 'k')) {
  v1::SchedulerCreateTaskRequest request;
  fill_context(request.mutable_context(), std::move(key));
  request.set_task_id("task-1");
  fill_target(request.mutable_target());
  request.set_payload("payload");
  request.mutable_run_at()->CopyFrom(google::protobuf::util::TimeUtil::SecondsToTimestamp(
      google::protobuf::util::TimeUtil::TimestampToSeconds(
          google::protobuf::util::TimeUtil::GetCurrentTime()) +
      60));
  return request;
}
v1::SchedulerCancelTaskRequest cancel_request(bool denied = false) {
  v1::SchedulerCancelTaskRequest request;
  fill_context(request.mutable_context());
  request.set_task_id("task-1");
  if (denied) {
    auto* resource = request.mutable_scope()->add_resources();
    resource->set_kind(v1::RESOURCE_TASK);
    resource->set_id("denied");
  }
  return request;
}
v1::SchedulerGetTaskRequest get_request() {
  v1::SchedulerGetTaskRequest request;
  fill_context(request.mutable_context(), "");
  request.set_task_id("missing");
  return request;
}
v1::SchedulerListTasksRequest list_request() {
  v1::SchedulerListTasksRequest request;
  fill_context(request.mutable_context(), "");
  request.mutable_page()->set_page_size(2);
  request.mutable_page()->set_page_token("tok");
  request.set_state(v1::SCHEDULED_TASK_PENDING);
  return request;
}
v1::SchedulerGetOccurrenceRequest occurrence_request() {
  v1::SchedulerGetOccurrenceRequest request;
  fill_context(request.mutable_context(), "");
  request.set_occurrence_id(uuid('c'));
  return request;
}
v1::SchedulerGetOperationRequest operation_request() {
  v1::SchedulerGetOperationRequest request;
  fill_context(request.mutable_context(), "");
  auto* operation = request.mutable_operation();
  operation->set_owner_service("Scheduler");
  operation->set_operation_id(uuid('d'));
  auto* resource = operation->mutable_resource();
  resource->set_kind(v1::RESOURCE_TASK);
  resource->set_id("task-1");
  return request;
}

template <typename Stub, typename Request, typename Response>
grpc::Status call(Stub& stub, Request request, Response* response,
                  grpc::Status (Stub::*method)(grpc::ClientContext*, const Request&, Response*)) {
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + 3s);
  return (stub.*method)(&context, request, response);
}

void run() {
  const GrpcLifetime lifetime;
  FakeHandler handler;
  FakeAuthorizer authorizer;
  kiarabot::core::platform::SchedulerRpcOptions options;
  options.authorize = [&](grpc::ServerContext& server, const v1::RequestContext& request,
                          std::string_view method, const v1::ResourceScope* scope) {
    return authorizer(server, request, method, scope);
  };
  auto adapter = kiarabot::core::platform::make_scheduler_rpc(handler, std::move(options));
  require(std::holds_alternative<std::unique_ptr<v1::SchedulerService::Service>>(adapter));
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(std::get<std::unique_ptr<v1::SchedulerService::Service>>(adapter).get());
  auto server = builder.BuildAndStart();
  require(server != nullptr && port > 0);
  auto channel =
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  auto stub = v1::SchedulerService::NewStub(channel);

  // Create: wire -> domain forwarding, idempotency key and scope-free authorization.
  v1::SchedulerCreateTaskResponse created;
  require(call(*stub, create_request(), &created, &v1::SchedulerService::Stub::CreateTask).ok());
  require(handler.create_calls == 1 && created.task().task_id() == "task-1");
  require(handler.last_context.idempotency_key == std::string(16, 'k'));
  require(handler.last_context.owner.value == uuid('a') &&
          handler.last_context.installation.value == uuid('b'));
  require(handler.last_input.target.conversation_id == "conversation");
  require(created.task().owner_instance_id() == uuid('a'));
  require(created.task().state() == v1::SCHEDULED_TASK_PENDING);
  // Missing idempotency key is rejected before any application call.
  v1::SchedulerCreateTaskResponse rejected;
  const auto missing_key =
      call(*stub, create_request(""), &rejected, &v1::SchedulerService::Stub::CreateTask);
  require(missing_key.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  require(handler.create_calls == 1);
  // Replayed/duplicate command surfaces application CONFLICT (same idempotency key path).
  handler.create_result = failure(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
  v1::SchedulerCreateTaskResponse conflict;
  const auto conflicted =
      call(*stub, create_request(), &conflict, &v1::SchedulerService::Stub::CreateTask);
  require(conflicted.error_code() == grpc::StatusCode::ABORTED);
  handler.create_result.reset();

  // Cancel scope: the authorizer receives the request scope; denial maps PERMISSION_DENIED.
  v1::SchedulerCancelTaskResponse denied;
  const auto denied_status =
      call(*stub, cancel_request(true), &denied, &v1::SchedulerService::Stub::CancelTask);
  require(denied_status.error_code() == grpc::StatusCode::PERMISSION_DENIED);
  require(authorizer.last_scope_resource == "denied");
  v1::SchedulerCancelTaskResponse cancelled;
  require(call(*stub, cancel_request(), &cancelled, &v1::SchedulerService::Stub::CancelTask).ok());
  require(handler.cancel_calls == 1);

  // Get: NOT_FOUND mapping and invalid empty id mapping.
  v1::SchedulerGetTaskResponse missing;
  const auto not_found = call(*stub, get_request(), &missing, &v1::SchedulerService::Stub::GetTask);
  require(not_found.error_code() == grpc::StatusCode::NOT_FOUND);
  auto empty_id = get_request();
  empty_id.set_task_id("");
  v1::SchedulerGetTaskResponse invalid;
  const auto invalid_status = call(*stub, empty_id, &invalid, &v1::SchedulerService::Stub::GetTask);
  require(invalid_status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);

  // List: page size/token/state forwarded and response page echoed.
  v1::SchedulerListTasksResponse listed;
  require(call(*stub, list_request(), &listed, &v1::SchedulerService::Stub::ListTasks).ok());
  require(handler.last_page_size == 2 && handler.last_page_token == "tok");
  require(handler.last_state == t::TaskState::Pending);
  require(listed.tasks_size() == 1 && listed.page().next_page_token() == "next-token");
  require(listed.tasks(0).target().id() == "conversation");

  // GetOccurrence: handoff/timestamps converted to the wire occurrence observation.
  v1::SchedulerGetOccurrenceResponse occurrence_response;
  require(call(*stub, occurrence_request(), &occurrence_response,
               &v1::SchedulerService::Stub::GetOccurrence)
              .ok());
  require(occurrence_response.occurrence().occurrence_id() == uuid('c'));
  require(occurrence_response.occurrence().handoff().source_service() == "Scheduler");
  require(occurrence_response.occurrence().attempts() == 0);

  // GetOperation: valid task operation maps; unknown resource kinds are rejected.
  v1::SchedulerGetOperationResponse operation_response;
  require(call(*stub, operation_request(), &operation_response,
               &v1::SchedulerService::Stub::GetOperation)
              .ok());
  require(operation_response.operation().ref().owner_service() == "Scheduler");
  require(handler.operation_calls == 1);
  auto bad_operation = operation_request();
  bad_operation.mutable_operation()->mutable_resource()->set_kind(v1::RESOURCE_ACCOUNT);
  v1::SchedulerGetOperationResponse bad_operation_response;
  const auto bad_operation_status = call(*stub, bad_operation, &bad_operation_response,
                                         &v1::SchedulerService::Stub::GetOperation);
  require(bad_operation_status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  require(handler.operation_calls == 1);

  stub.reset();
  channel.reset();
  server->Shutdown();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "scheduler rpc fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
