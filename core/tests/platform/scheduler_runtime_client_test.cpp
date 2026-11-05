// P10b wire test for SchedulerRuntimeClient: the Scheduler -> Runtime timer handoff must carry
// canonical RFC 4122 version 4 request/trace correlation ids. A malformed owner-side id is rejected
// by Runtime as INVALID_ARGUMENT, so this guards the generator at the frozen wire boundary while a
// local gRPC server stands in for the Runtime owner listener.
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <google/protobuf/util/time_util.h>
#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/scheduler_runtime_client.hpp"
#include "kiarabot/v1/runtime.grpc.pb.h"

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

// Frozen wire identity: correlation ids must be 8-4-4-4-12 lowercase hex, version 4 and the RFC
// 4122 10xx variant. Owners reject a non-canonical id as INVALID_ARGUMENT.
bool canonical_v4_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return value[14] == '4' &&
         (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}

class FakeRuntime final : public v1::RuntimeService::Service {
public:
  grpc::Status AcceptTimerTrigger(grpc::ServerContext*,
                                  const v1::RuntimeAcceptTimerTriggerRequest* request,
                                  v1::RuntimeAcceptTimerTriggerResponse* response) override {
    ++calls;
    last_request_id = request->context().request_id();
    last_trace_id = request->context().trace_id();
    last_task_id = request->task_id();
    auto* receipt = response->mutable_receipt();
    receipt->set_occurrence_id(request->occurrence_id());
    receipt->set_event_id("11111111-1111-4111-8111-111111111111");
    receipt->set_duplicate(false);
    *receipt->mutable_persisted_at() = google::protobuf::util::TimeUtil::GetCurrentTime();
    return grpc::Status::OK;
  }
  int calls = 0;
  std::string last_request_id;
  std::string last_trace_id;
  std::string last_task_id;
};

class GrpcLifetime {
public:
  GrpcLifetime() {
    grpc_init();
  }
  ~GrpcLifetime() {
    grpc_shutdown();
  }
};

t::TimerDeliveryRequest timer_request() {
  t::TimerDeliveryRequest request;
  request.occurrence_id = t::MessageId{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
  request.task_id = "task-1";
  request.owner_instance_id = t::InstanceId{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"};
  request.schedule_version = t::ResourceVersion{1};
  request.target = t::ConversationKey{t::ResourceId{"adapter-1"}, t::ResourceId{"account-1"},
                                      "conversation-1", t::ConversationKey::Kind::Group};
  request.payload = {std::byte{0x7b}, std::byte{0x7d}};
  request.handoff.source = t::Service::Scheduler;
  request.handoff.message = t::MessageId{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
  request.handoff.operation =
      t::OperationRef{t::Service::Scheduler, t::OperationId{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"},
                      t::ResourceRef{t::ResourceKind::Task, t::ResourceId{"task-1"}}};
  request.handoff.resource = t::ResourceRef{t::ResourceKind::Task, t::ResourceId{"task-1"}};
  request.handoff.version = t::ResourceVersion{1};
  request.handoff.actor = t::SubjectId{"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"};
  request.handoff.authorization_version = t::ResourceVersion{1};
  request.handoff.created_at = now();
  request.handoff.valid_until = now() + std::chrono::seconds{30};
  request.trace_id = "trace-1";
  return request;
}

void run() {
  const GrpcLifetime lifetime;
  FakeRuntime service;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  check(server != nullptr && port > 0, "fake runtime server failed");

  auto created = platform::SchedulerRuntimeClient::create(
      {.endpoint = "127.0.0.1:" + std::to_string(port),
       .channel_credentials = grpc::InsecureChannelCredentials(),
       .budget = std::chrono::milliseconds{1000}});
  check(std::holds_alternative<std::unique_ptr<platform::SchedulerRuntimeClient>>(created),
        "scheduler runtime client construction failed");
  auto client = std::move(std::get<std::unique_ptr<platform::SchedulerRuntimeClient>>(created));
  const auto accepted = client->accept(timer_request());
  check(std::holds_alternative<t::TimerDeliveryReceipt>(accepted),
        "runtime timer acceptance rejected");
  check(service.calls == 1 && service.last_task_id == "task-1",
        "runtime timer acceptance did not reach the owner with the task id");
  check(canonical_v4_uuid(service.last_request_id) && canonical_v4_uuid(service.last_trace_id),
        "runtime timer handoff correlation ids are not canonical v4 UUIDs");
  check(client->available(), "runtime timer client availability not updated");

  server->Shutdown();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "scheduler runtime client fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
