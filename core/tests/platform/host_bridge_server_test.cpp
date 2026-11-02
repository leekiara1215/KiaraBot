#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/util/time_util.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/host.grpc.pb.h>
#include <kiarabot/v1/host.pb.h>
#include <kiarabot/v1/platform.pb.h>

#include "kiarabot/core/runtime/platform/host_bridge_server.hpp"

namespace t = kiarabot::core::target;
namespace platform = kiarabot::core::platform;
namespace v1 = kiarabot::v1;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "P08b HostBridge server assertion: " << message << '\n';
    std::exit(1);
  }
}

t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}

google::protobuf::Timestamp timestamp(t::TimePoint value) {
  google::protobuf::Timestamp output;
  output.set_seconds(value.time_since_epoch().count() / 1'000'000'000);
  output.set_nanos(static_cast<int>(value.time_since_epoch().count() % 1'000'000'000));
  return output;
}

v1::Fence fence_proto(const t::HostFence& fence) {
  v1::Fence output;
  output.set_instance_id(fence.host.value);
  output.set_runtime_epoch(fence.runtime_epoch);
  output.set_connection_generation(fence.connection_generation);
  return output;
}

t::Failure failure_of(t::ErrorCode code) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
}

class Store final : public t::HostRegistrationPort {
public:
  explicit Store(std::vector<t::HostCertificateBinding> allowlist)
      : allowlist_(std::move(allowlist)) {}

  t::Outcome<std::optional<t::HostCertificateBinding>>
  certificate_binding(std::string_view certificate) override {
    for (const auto& entry : allowlist_) {
      if (entry.certificate_sha256 == certificate) {
        return std::optional<t::HostCertificateBinding>{entry};
      }
    }
    return std::optional<t::HostCertificateBinding>{};
  }
  t::Outcome<std::optional<t::HostRegistration>> find_host(const t::ResourceId& host) override {
    const auto found = hosts_.find(host.value);
    if (found == hosts_.end())
      return std::optional<t::HostRegistration>{};
    return std::optional<t::HostRegistration>{found->second};
  }
  t::Status save_host(const t::HostRegistration& registration,
                      std::optional<t::ResourceVersion> expected) override {
    const auto found = hosts_.find(registration.host.value);
    if (expected) {
      if (found == hosts_.end() || found->second.version != *expected) {
        return failure_of(t::ErrorCode::Conflict);
      }
      auto stored = registration;
      stored.version.value = expected->value + 1;
      hosts_[stored.host.value] = stored;
      return std::monostate{};
    }
    if (found != hosts_.end())
      return failure_of(t::ErrorCode::Conflict);
    auto stored = registration;
    stored.version.value = 1;
    hosts_[stored.host.value] = stored;
    return std::monostate{};
  }
  t::Outcome<std::optional<t::HostConnectionState>> connection(const t::ResourceId& host) override {
    const auto found = connections_.find(host.value);
    if (found == connections_.end())
      return std::optional<t::HostConnectionState>{};
    return std::optional<t::HostConnectionState>{found->second};
  }
  t::Outcome<t::HostConnectionState>
  activate_connection(const t::HostConnectionState& connection) override {
    auto stored = connection;
    stored.fence.connection_generation = ++generation_;
    connections_[stored.fence.host.value] = stored;
    return stored;
  }
  t::Status save_connection(const t::HostConnectionState& connection,
                            std::uint64_t expected) override {
    const auto found = connections_.find(connection.fence.host.value);
    if (found == connections_.end() || found->second.fence.connection_generation != expected) {
      return failure_of(t::ErrorCode::Conflict);
    }
    connections_[connection.fence.host.value] = connection;
    return std::monostate{};
  }

private:
  std::vector<t::HostCertificateBinding> allowlist_;
  std::map<std::string, t::HostRegistration> hosts_;
  std::map<std::string, t::HostConnectionState> connections_;
  std::uint64_t generation_ = 0;
};

t::TimePoint clock_now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}

class Clock final : public t::HostClockPort {
public:
  t::Outcome<t::TimePoint> now() override {
    return clock_now();
  }
  t::Outcome<std::string> runtime_epoch() override {
    return std::string{"epoch-test"};
  }
  t::Outcome<std::uint64_t> next_connection_generation() override {
    return ++generation_;
  }

private:
  std::uint64_t generation_ = 0;
};

struct Fixture {
  explicit Fixture(std::vector<t::HostCertificateBinding> allowlist, std::string fingerprint)
      : store(std::move(allowlist)), fingerprint_(std::move(fingerprint)) {
    platform::HostBridgeServerDependencies dependencies{
        store, clock,
        [this](grpc::ServerContext&) -> t::Outcome<std::string> {
          if (fingerprint_.empty()) {
            return t::Failure{t::ErrorCode::Unauthenticated,
                              t::Acceptance::NotAccepted,
                              t::Recovery::Authenticate,
                              {},
                              {}};
          }
          return fingerprint_;
        },
        platform::HostBridgeCallbacks{}};
    auto service = platform::make_host_bridge(std::move(dependencies));
    require(std::holds_alternative<std::unique_ptr<v1::HostBridge::Service>>(service),
            "server construction");
    server = std::make_unique<grpc::ServerBuilder>();
    server->RegisterService(std::get<std::unique_ptr<v1::HostBridge::Service>>(service).get());
    service_ = std::move(std::get<std::unique_ptr<v1::HostBridge::Service>>(service));
    server->AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    instance_ = server->BuildAndStart();
    require(instance_ != nullptr && port != 0, "in-process server start");
    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                   grpc::InsecureChannelCredentials());
    stub_ = v1::HostBridge::NewStub(channel_);
  }
  ~Fixture() {
    if (instance_)
      instance_->Shutdown();
    if (instance_)
      instance_->Wait();
  }

  std::unique_ptr<v1::HostBridge::Stub> stub() {
    return v1::HostBridge::NewStub(channel_);
  }

  Store store;
  Clock clock;
  std::unique_ptr<grpc::ServerBuilder> server;
  std::unique_ptr<v1::HostBridge::Service> service_;
  std::unique_ptr<grpc::Server> instance_;
  int port = 0;
  std::shared_ptr<grpc::ChannelInterface> channel_;
  std::unique_ptr<v1::HostBridge::Stub> stub_;

private:
  std::string fingerprint_;
};

v1::HostToRuntime registration_frame(const std::string& host_id,
                                     const std::vector<std::string>& capabilities) {
  v1::HostToRuntime frame;
  auto* host = frame.mutable_registration()->mutable_host();
  host->set_host_id(host_id);
  for (const auto& capability : capabilities)
    host->add_capabilities(capability);
  host->mutable_observed_at()->CopyFrom(google::protobuf::util::TimeUtil::GetCurrentTime());
  return frame;
}

v1::HostToRuntime heartbeat_frame(const v1::Fence& fence, t::TimePoint sent_at) {
  v1::HostToRuntime frame;
  frame.mutable_fence()->CopyFrom(fence);
  frame.mutable_heartbeat()->mutable_sent_at()->CopyFrom(timestamp(sent_at));
  auto* cursor = frame.mutable_heartbeat()->mutable_cursor();
  cursor->set_source_service("host");
  cursor->set_epoch(fence.runtime_epoch());
  cursor->set_sequence(1);
  return frame;
}

v1::RuntimeToHost
read_frame(grpc::ClientReaderWriter<v1::HostToRuntime, v1::RuntimeToHost>* stream) {
  v1::RuntimeToHost frame;
  require(stream->Read(&frame), "stream read");
  return frame;
}

std::vector<t::HostCertificateBinding> allowlist_for(const std::string& host_id,
                                                     const std::string& certificate) {
  return {t::HostCertificateBinding{host_id, certificate}};
}

void test_registration_and_frames() {
  Fixture fixture(allowlist_for("host-a", "aa"), "aa");
  grpc::ClientContext context;
  auto stream = fixture.stub()->Connect(&context);
  require(stream.get() != nullptr, "connect stream");
  auto registration = registration_frame("host-a", {"host.process.v1", "host.inventory.v1"});
  require(stream->Write(registration), "write registration");
  const auto registered = read_frame(stream.get());
  require(registered.body_case() == v1::RuntimeToHost::kRegistered, "registered body");
  const auto& fence = registered.registered().fence();
  require(fence.instance_id() == "host-a" && fence.connection_generation() == 1 &&
              !fence.has_execution_generation() && fence.runtime_epoch() == "epoch-test",
          "assigned fence");

  auto heartbeat = heartbeat_frame(fence, clock_now());
  require(stream->Write(heartbeat), "write heartbeat");
  const auto ack = read_frame(stream.get());
  require(ack.body_case() == v1::RuntimeToHost::kHeartbeatAck &&
              ack.fence().connection_generation() == 1 &&
              ack.heartbeat_ack().heartbeat_interval_seconds() == 5 &&
              !ack.heartbeat_ack().resync_required(),
          "heartbeat ack");

  auto inventory = v1::HostToRuntime{};
  inventory.mutable_fence()->CopyFrom(fence);
  inventory.mutable_inventory()->set_host_id("host-a");
  require(stream->Write(inventory), "write inventory");
  auto receipt = v1::HostToRuntime{};
  receipt.mutable_fence()->CopyFrom(fence);
  receipt.mutable_receipt()->set_command_id("command-1");
  receipt.mutable_receipt()->set_persisted(true);
  require(stream->Write(receipt), "write receipt");
  auto observation = v1::HostToRuntime{};
  observation.mutable_fence()->CopyFrom(fence);
  auto* execution = observation.mutable_observation()->mutable_execution();
  execution->set_host_id("host-a");
  execution->set_instance_id("instance-1");
  execution->mutable_observed_at()->CopyFrom(google::protobuf::util::TimeUtil::GetCurrentTime());
  require(stream->Write(observation), "write observation");
  require(stream->Write(heartbeat), "write trailing heartbeat");
  const auto trailing = read_frame(stream.get());
  require(trailing.body_case() == v1::RuntimeToHost::kHeartbeatAck, "trailing heartbeat ack");
  require(stream->WritesDone(), "writes done");
  require(stream->Finish().ok(), "finish");
}

void test_stale_generation_rejected() {
  Fixture fixture(allowlist_for("host-a", "aa"), "aa");
  grpc::ClientContext context;
  auto stream = fixture.stub()->Connect(&context);
  require(stream->Write(registration_frame("host-a", {})), "write registration 1");
  const auto first = read_frame(stream.get());
  auto fence = first.registered().fence();
  // A second registration is delivered as a failure frame for the new generation.
  grpc::ClientContext context2;
  auto second_stream = fixture.stub()->Connect(&context2);
  require(second_stream->Write(registration_frame("host-a", {})), "write registration 2");
  const auto second = read_frame(second_stream.get());
  require(second.registered().fence().connection_generation() == 2, "replacement generation");
  grpc::ClientContext context3;
  auto stale_stream = fixture.stub()->Connect(&context3);
  require(stale_stream->Write(registration_frame("host-a", {})), "write registration 3");
  const auto current = read_frame(stale_stream.get());
  require(current.registered().fence().connection_generation() == 3, "latest generation");
  auto stale = heartbeat_frame(fence, now() - std::chrono::seconds{1});
  stale.clear_fence();
  stale.mutable_fence()->CopyFrom(fence);
  stale.mutable_fence()->set_connection_generation(1);
  require(stale_stream->Write(stale), "write stale heartbeat");
  const auto failure = read_frame(stale_stream.get());
  require(failure.body_case() == v1::RuntimeToHost::kFailure &&
              failure.failure().error().code() == v1::CONFLICT,
          "stale generation failure");
  require(stale_stream->Finish().ok(), "stale finish");
}

void test_peer_certificate_binding() {
  Fixture mismatched(allowlist_for("host-a", "aa"), "bb");
  grpc::ClientContext context;
  auto stream = mismatched.stub()->Connect(&context);
  require(stream->Write(registration_frame("host-a", {})), "write registration");
  require(stream->WritesDone(), "writes done");
  require(stream->Finish().error_code() == grpc::StatusCode::PERMISSION_DENIED,
          "certificate mismatch status");

  Fixture empty({}, "aa");
  grpc::ClientContext empty_context;
  auto empty_stream = empty.stub()->Connect(&empty_context);
  require(empty_stream->Write(registration_frame("host-a", {})), "write empty registration");
  require(empty_stream->WritesDone(), "empty writes done");
  require(empty_stream->Finish().error_code() == grpc::StatusCode::PERMISSION_DENIED,
          "empty allowlist status");
}

void test_missing_registration_rejected() {
  Fixture fixture(allowlist_for("host-a", "aa"), "aa");
  grpc::ClientContext context;
  auto stream = fixture.stub()->Connect(&context);
  require(stream->WritesDone(), "writes done");
  require(stream->Finish().error_code() == grpc::StatusCode::INVALID_ARGUMENT,
          "missing registration status");
}

} // namespace

int main() {
  try {
    test_registration_and_frames();
    test_stale_generation_rejected();
    test_peer_certificate_binding();
    test_missing_registration_rejected();
    std::cout << "P08b HostBridge server conformance passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "P08b HostBridge server assertion failed: " << error.what() << '\n';
    return 1;
  }
}
