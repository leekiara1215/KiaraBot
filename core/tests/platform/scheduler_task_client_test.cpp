// P13h-2 wire matrix for SchedulerTaskClient's platform-identity lifecycle path. A local gRPC
// server stands in for Scheduler: P13h-1 (real Scheduler platform-peer authorization) is parallel
// work, so this test proves the Runtime client sends no delegation, scopes ListTasks/CancelTask to
// one instance and carries an idempotency key for CancelTask. The production mTLS handshake and
// Scheduler-side authorization are covered by the P13h-1/P13h-3 chains.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/scheduler_task_client.hpp"
#include "kiarabot/v1/scheduler.grpc.pb.h"

namespace {
namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace v1 = kiarabot::v1;

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}

constexpr auto kInstance = "11111111-1111-4111-8111-111111111111";
constexpr auto kTask = "task-1";

t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}

t::SchedulerTaskContext context(std::string idempotency = {}) {
  t::SchedulerTaskContext value;
  value.owner = t::InstanceId{kInstance};
  value.request_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  value.trace_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  value.deadline = now() + std::chrono::seconds{2};
  value.idempotency_key = std::move(idempotency);
  return value;
}

void fill_task(v1::TaskRecord* task, std::string owner, std::string task_id) {
  task->set_task_id(std::move(task_id));
  task->set_owner_instance_id(std::move(owner));
  task->set_installation_id("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
  task->mutable_target()->set_id("conversation-1");
  task->mutable_target()->set_type(v1::ConversationRef::GROUP);
  task->set_state(v1::SCHEDULED_TASK_PENDING);
  task->set_version(1);
}

class FakeScheduler final : public v1::SchedulerService::Service {
public:
  grpc::Status ListTasks(grpc::ServerContext*, const v1::SchedulerListTasksRequest* request,
                         v1::SchedulerListTasksResponse* response) override {
    ++lists;
    list_request = *request;
    fill_task(response->add_tasks(), request->scope().resources(0).id(), kTask);
    response->mutable_page()->set_page_size(20);
    return grpc::Status::OK;
  }
  grpc::Status CancelTask(grpc::ServerContext*, const v1::SchedulerCancelTaskRequest* request,
                          v1::SchedulerCancelTaskResponse* response) override {
    ++cancels;
    cancel_request = *request;
    fill_task(response->mutable_task(), request->scope().resources(0).id(), request->task_id());
    return grpc::Status::OK;
  }

  int lists = 0;
  int cancels = 0;
  v1::SchedulerListTasksRequest list_request;
  v1::SchedulerCancelTaskRequest cancel_request;
};

class Server {
public:
  explicit Server(FakeScheduler& service) {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server_ = builder.BuildAndStart();
    endpoint_ = "127.0.0.1:" + std::to_string(port);
  }
  ~Server() {
    server_->Shutdown();
  }
  const std::string& endpoint() const {
    return endpoint_;
  }

private:
  std::unique_ptr<grpc::Server> server_;
  std::string endpoint_;
};
} // namespace

int main() {
  grpc_init();
  FakeScheduler service;
  Server server(service);
  auto client =
      platform::SchedulerTaskClient::create({server.endpoint(), grpc::InsecureChannelCredentials(),
                                             nullptr, std::chrono::milliseconds{2000}, true});
  check(!std::holds_alternative<t::Failure>(client), "platform client create");
  auto& scheduler = std::get<std::unique_ptr<platform::SchedulerTaskClient>>(client);

  const auto listed = scheduler->list_tasks(context(), t::TaskState::Pending, 20);
  check(!std::holds_alternative<t::Failure>(listed), "ListTasks platform success");
  const auto& tasks = std::get<std::vector<t::ScheduledTask>>(listed);
  check(tasks.size() == 1 && tasks[0].key.task_id == kTask && tasks[0].key.owner.value == kInstance,
        "ListTasks decode");
  check(service.list_request.context().delegation().empty(),
        "platform ListTasks sends no delegation");
  check(service.list_request.scope().resources_size() == 1 &&
            service.list_request.scope().resources(0).kind() == v1::RESOURCE_INSTANCE &&
            service.list_request.scope().resources(0).id() == kInstance,
        "ListTasks explicit instance scope");
  check(service.list_request.has_state() &&
            service.list_request.state() == v1::SCHEDULED_TASK_PENDING,
        "ListTasks pending filter forwarded");
  check(service.list_request.page().page_size() == 20, "ListTasks page size forwarded");

  const auto cancelled =
      scheduler->cancel_task(context("dddddddd-dddd-4ddd-8ddd-dddddddddddd"), kTask);
  check(!std::holds_alternative<t::Failure>(cancelled), "CancelTask platform success");
  check(service.cancel_request.context().delegation().empty(),
        "platform CancelTask sends no delegation");
  check(service.cancel_request.context().idempotency_key() ==
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "CancelTask idempotency key forwarded");
  check(service.cancel_request.scope().resources_size() == 1 &&
            service.cancel_request.scope().resources(0).id() == kInstance,
        "CancelTask explicit instance scope");

  std::cout << "P13h-2 scheduler task client platform wire matrix passed\n";
  grpc_shutdown();
  return 0;
}
