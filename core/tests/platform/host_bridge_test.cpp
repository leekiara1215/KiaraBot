#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/host.grpc.pb.h>
#include <kiarabot/v1/host.pb.h>
#include <kiarabot/v1/platform.pb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "kiarabot/core/runtime/platform/host_bridge_client.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace v1 = kiarabot::v1;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("host bridge assertion failed at line " +
                             std::to_string(location.line()));
  }
}

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 3000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

int free_loopback_port() {
  const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) {
    return 0;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  socklen_t length = sizeof(address);
  if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::close(descriptor);
    return 0;
  }
  ::close(descriptor);
  return ntohs(address.sin_port);
}

class GrpcLifetime {
public:
  GrpcLifetime() {
    grpc_init();
  }
  ~GrpcLifetime() {
    grpc_shutdown();
  }
  GrpcLifetime(const GrpcLifetime&) = delete;
  GrpcLifetime& operator=(const GrpcLifetime&) = delete;
};

class FakeRuntime final : public v1::HostBridge::Service {
public:
  grpc::Status
  Connect(grpc::ServerContext*,
          grpc::ServerReaderWriter<v1::RuntimeToHost, v1::HostToRuntime>* stream) override {
    v1::HostToRuntime frame;
    if (!stream->Read(&frame) || frame.body_case() != v1::HostToRuntime::kRegistration) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "registration");
    }
    {
      std::lock_guard lock(mutex_);
      host_id_ = frame.registration().host().host_id();
      registered_ = true;
    }
    v1::Fence fence;
    fence.set_instance_id(host_id_);
    fence.set_runtime_epoch("epoch-1");
    fence.set_connection_generation(1);
    {
      std::lock_guard lock(mutex_);
      fence_ = fence;
    }
    v1::RuntimeToHost registered;
    registered.mutable_fence()->CopyFrom(fence);
    registered.mutable_registered()->mutable_host()->set_host_id(host_id_);
    registered.mutable_registered()->mutable_fence()->CopyFrom(fence);
    if (!stream->Write(registered)) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "registered write");
    }
    // Send the three command shapes immediately after registration.
    v1::RuntimeToHost apply;
    apply.mutable_fence()->CopyFrom(fence);
    auto* apply_body = apply.mutable_apply();
    apply_body->set_command_id("command-apply");
    apply_body->mutable_handoff()->set_payload_sha256(std::string(32, 'a'));
    auto* process = apply_body->mutable_process();
    process->set_instance_id("11111111-1111-4111-8111-111111111111");
    process->set_installation_id("33333333-3333-4333-8333-333333333333");
    process->set_execution_generation(1);
    process->set_desired_version(1);
    process->set_desired_state(v1::DESIRED_RUNNING);
    process->set_package_sha256(std::string(32, 'b'));
    process->set_launch_profile("python");
    stream->Write(apply);
    v1::RuntimeToHost stop;
    stop.mutable_fence()->CopyFrom(fence);
    auto* stop_body = stop.mutable_stop();
    stop_body->set_command_id("command-stop");
    stop_body->set_instance_id("11111111-1111-4111-8111-111111111111");
    stop_body->set_execution_generation(1);
    stop_body->set_grace_ms(1000);
    stop_body->mutable_handoff()->set_payload_sha256(std::string(32, 'c'));
    stream->Write(stop);
    v1::RuntimeToHost drain;
    drain.mutable_fence()->CopyFrom(fence);
    drain.mutable_drain()->set_command_id("command-drain");
    drain.mutable_drain()->set_grace_ms(1000);
    drain.mutable_drain()->mutable_handoff()->set_payload_sha256(std::string(32, 'd'));
    stream->Write(drain);
    while (stream->Read(&frame)) {
      if (!frame.has_fence() ||
          frame.fence().connection_generation() != fence.connection_generation()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "fence");
      }
      std::lock_guard lock(mutex_);
      switch (frame.body_case()) {
      case v1::HostToRuntime::kHeartbeat: {
        ++heartbeats_;
        v1::RuntimeToHost ack;
        ack.mutable_fence()->CopyFrom(fence);
        ack.mutable_heartbeat_ack()->mutable_fence()->CopyFrom(fence);
        ack.mutable_heartbeat_ack()->set_resync_required(send_resync_);
        send_resync_ = false;
        stream->Write(ack);
        break;
      }
      case v1::HostToRuntime::kReceipt: {
        receipts_.push_back(frame.receipt());
        break;
      }
      case v1::HostToRuntime::kObservation: {
        observations_.push_back(frame.observation());
        break;
      }
      default:
        break;
      }
    }
    return grpc::Status::OK;
  }

  std::mutex mutex_;
  bool registered_ = false;
  std::string host_id_;
  v1::Fence fence_;
  std::vector<v1::HostCommandReceipt> receipts_;
  std::vector<v1::HostWatchExecutionsResponse> observations_;
  int heartbeats_ = 0;
  bool send_resync_ = true;
};

class Handler final : public t::HostBridgeHandler {
public:
  t::Outcome<t::HostReceipt> apply_desired_process(const t::HostProcessSpec& process,
                                                   std::string_view command_id,
                                                   std::string_view payload_sha256) override {
    std::lock_guard lock(mutex_);
    apply_calls_ += 1;
    apply_command_ = std::string(command_id);
    apply_hash_ = std::string(payload_sha256);
    apply_instance_ = process.instance_id;
    t::ManagedExecution execution;
    execution.host_id = "host-1";
    execution.instance_id = process.instance_id;
    execution.execution_generation = process.execution_generation;
    execution.state = t::ManagedExecutionState::Running;
    execution.outcome = t::ExecutionOutcome::NotStarted;
    execution.command_id = std::string(command_id);
    return t::HostReceipt{std::string(command_id), true, execution};
  }
  t::Outcome<t::HostReceipt> stop_execution(std::string_view instance_id,
                                            std::uint64_t execution_generation,
                                            std::uint32_t grace_ms, std::string_view command_id,
                                            std::string_view) override {
    std::lock_guard lock(mutex_);
    stop_calls_ += 1;
    stop_generation_ = execution_generation;
    stop_grace_ = grace_ms;
    return t::HostReceipt{std::string(command_id), true, std::nullopt};
  }
  t::Outcome<t::HostReceipt> drain_host(std::uint32_t grace_ms, std::string_view command_id,
                                        std::string_view) override {
    std::lock_guard lock(mutex_);
    drain_calls_ += 1;
    drain_grace_ = grace_ms;
    return t::HostReceipt{std::string(command_id), true, std::nullopt};
  }
  void bridge_ready() override {
    std::lock_guard lock(mutex_);
    ready_calls_ += 1;
  }
  void bridge_resync() override {
    std::lock_guard lock(mutex_);
    resync_calls_ += 1;
  }

  std::mutex mutex_;
  int apply_calls_ = 0, stop_calls_ = 0, drain_calls_ = 0, ready_calls_ = 0, resync_calls_ = 0;
  std::string apply_command_, apply_hash_, apply_instance_;
  std::uint64_t stop_generation_ = 0;
  std::uint32_t stop_grace_ = 0, drain_grace_ = 0;
};

void run() {
  const GrpcLifetime lifetime;
  FakeRuntime runtime;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&runtime);
  auto server = builder.BuildAndStart();
  require(server != nullptr && port > 0);

  kiarabot::core::platform::HostBridgeClientConfig config;
  config.endpoint = "127.0.0.1:" + std::to_string(port);
  config.credentials = grpc::InsecureChannelCredentials();
  config.heartbeat_interval = 40ms;
  config.registration_timeout = 3s;
  config.identity.host_id = "host-1";
  t::HostPackage package;
  package.plugin_type_id = "observer";
  package.root = "/tmp/package";
  package.sha256 = std::string(64, 'b');
  package.launch_profile = "python";
  config.identity.packages.push_back(package);

  Handler handler;
  kiarabot::core::platform::HostBridgeClient client(std::move(config));
  require(std::holds_alternative<std::monostate>(client.start(handler)));
  require(wait_for([&] { return client.registered(); }));
  require(wait_for([&] {
    std::lock_guard lock(handler.mutex_);
    return handler.apply_calls_ == 1 && handler.stop_calls_ == 1 && handler.drain_calls_ == 1;
  }));
  {
    std::lock_guard lock(handler.mutex_);
    require(handler.apply_command_ == "command-apply" && handler.apply_hash_.size() == 64);
    require(handler.apply_instance_ == "11111111-1111-4111-8111-111111111111");
    require(handler.stop_generation_ == 1 && handler.stop_grace_ == 1000);
  }
  require(wait_for([&] {
    std::lock_guard lock(runtime.mutex_);
    return runtime.receipts_.size() >= 3;
  }));
  {
    std::lock_guard lock(runtime.mutex_);
    require(runtime.host_id_ == "host-1");
    require(runtime.fence_.instance_id() == "host-1");
  }
  require(wait_for([&] {
    std::lock_guard lock(handler.mutex_);
    return handler.resync_calls_ >= 1;
  }));
  require(wait_for([&] {
    std::lock_guard lock(runtime.mutex_);
    return runtime.heartbeats_ >= 1;
  }));
  t::ExecutionObservation observation;
  observation.sequence = 7;
  observation.execution.host_id = "host-1";
  observation.execution.instance_id = "11111111-1111-4111-8111-111111111111";
  observation.execution.execution_generation = 1;
  observation.execution.state = t::ManagedExecutionState::Running;
  observation.execution.outcome = t::ExecutionOutcome::NotStarted;
  observation.execution.observed_at = std::chrono::system_clock::now();
  require(std::holds_alternative<std::monostate>(client.send_observation(observation)));
  require(client.delivered_sequence() == 7);
  require(wait_for([&] {
    std::lock_guard lock(runtime.mutex_);
    return runtime.observations_.size() >= 1;
  }));
  {
    std::lock_guard lock(runtime.mutex_);
    require(runtime.observations_.front().cursor().sequence() == 7);
    require(runtime.observations_.front().cursor().source_service() == "host");
  }
  t::HostReceipt receipt{std::string("manual-command"), true, std::nullopt};
  require(std::holds_alternative<std::monostate>(client.send_receipt(receipt)));
  client.stop();
  require(!client.registered());
  server->Shutdown();

  // Runtime offline at Host start: the process/bridge must not fail; registration retries until
  // Runtime appears, and readiness stays false until the fence is accepted.
  const int late_port = free_loopback_port();
  require(late_port > 0);
  kiarabot::core::platform::HostBridgeClientConfig late_config;
  late_config.endpoint = "127.0.0.1:" + std::to_string(late_port);
  late_config.credentials = grpc::InsecureChannelCredentials();
  late_config.heartbeat_interval = 40ms;
  late_config.registration_timeout = 500ms;
  late_config.identity.host_id = "host-late";
  late_config.identity.packages.push_back(package);
  Handler late_handler;
  kiarabot::core::platform::HostBridgeClient late_client(std::move(late_config));
  require(std::holds_alternative<std::monostate>(late_client.start(late_handler)));
  require(!late_client.registered());
  std::this_thread::sleep_for(50ms);
  require(!late_client.registered());
  FakeRuntime late_runtime;
  grpc::ServerBuilder late_builder;
  int late_bound = 0;
  late_builder.AddListeningPort("127.0.0.1:" + std::to_string(late_port),
                                grpc::InsecureServerCredentials(), &late_bound);
  late_builder.RegisterService(&late_runtime);
  auto late_server = late_builder.BuildAndStart();
  require(late_server != nullptr && late_bound == late_port);
  require(wait_for([&] { return late_client.registered(); }, 8000ms));
  require(wait_for([&] {
    std::lock_guard lock(late_handler.mutex_);
    return late_handler.ready_calls_ >= 1;
  }));
  late_client.stop();
  require(!late_client.registered());
  late_server->Shutdown();
}

} // namespace

int main() {
  try {
    run();
    std::cout << "host bridge fixtures only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
