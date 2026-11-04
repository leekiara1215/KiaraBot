#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/runtime.grpc.pb.h>

#include "kiarabot/core/runtime/platform/host_runtime_client.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace p = kiarabot::core::platform;
namespace v1 = kiarabot::v1;
using namespace std::chrono_literals;
constexpr char kHost[] = "11111111-1111-4111-8111-111111111111";

int failures = 0;
void check(bool value, const char* name) {
  if (!value) {
    ++failures;
    std::cerr << "host runtime client: " << name << '\n';
  }
}
t::HostFence fence() {
  const std::string host{kHost};
  return t::HostFence{t::ResourceId{host}, 7, "runtime-epoch"};
}
t::HostAdmissionGateRequest gate_request(bool open = false) {
  return t::HostAdmissionGateRequest{
      {t::Service::Host, {std::string(36, 'a')}, {t::ResourceKind::Host, {kHost}}}, 11, 0, open};
}
void fill_success(const v1::RuntimeSetHostAdmissionGateRequest& request,
                  v1::RuntimeSetHostAdmissionGateResponse* response) {
  response->set_host_id(request.host_id());
  response->set_gate_version(request.expected_gate_version() + 1);
  response->set_maintenance_version(request.maintenance_version());
  response->set_admission(request.admission());
  *response->mutable_operation() = request.operation();
}
// One loopback fake Runtime; the wire fixture only, not a production authorization proof.
class FakeRuntime final : public v1::RuntimeService::Service {
public:
  std::mutex mutex;
  std::optional<v1::RuntimeSetHostAdmissionGateRequest> last;
  int calls = 0;
  enum class Mode {
    Success,
    MismatchHost,
    MismatchMaintenance,
    MismatchAdmission,
    ZeroGateVersion,
    WrongOperation,
    Unauthenticated,
    PermissionDenied,
    Conflict,
    Unavailable,
    Timeout,
    Internal
  };
  Mode mode = Mode::Success;
  grpc::Status SetHostAdmissionGate(grpc::ServerContext*,
                                    const v1::RuntimeSetHostAdmissionGateRequest* request,
                                    v1::RuntimeSetHostAdmissionGateResponse* response) override {
    {
      std::lock_guard lock(mutex);
      last = *request;
      ++calls;
    }
    switch (mode) {
    case Mode::Success:
      fill_success(*request, response);
      return grpc::Status::OK;
    case Mode::MismatchHost:
      fill_success(*request, response);
      response->set_host_id("other-host");
      return grpc::Status::OK;
    case Mode::MismatchMaintenance:
      fill_success(*request, response);
      response->set_maintenance_version(request->maintenance_version() + 1);
      return grpc::Status::OK;
    case Mode::MismatchAdmission:
      fill_success(*request, response);
      response->set_admission(v1::HOST_ADMISSION_OPEN);
      return grpc::Status::OK;
    case Mode::ZeroGateVersion:
      fill_success(*request, response);
      response->set_gate_version(0);
      return grpc::Status::OK;
    case Mode::WrongOperation:
      fill_success(*request, response);
      response->mutable_operation()->set_operation_id(std::string(36, 'b'));
      return grpc::Status::OK;
    case Mode::Unauthenticated:
      return {grpc::StatusCode::UNAUTHENTICATED, "fixture"};
    case Mode::PermissionDenied:
      return {grpc::StatusCode::PERMISSION_DENIED, "fixture"};
    case Mode::Conflict:
      return {grpc::StatusCode::ABORTED, "fixture"};
    case Mode::Unavailable:
      return {grpc::StatusCode::UNAVAILABLE, "fixture"};
    case Mode::Timeout:
      return {grpc::StatusCode::DEADLINE_EXCEEDED, "fixture"};
    default:
      return {grpc::StatusCode::INTERNAL, "fixture"};
    }
  }
};
struct Server {
  FakeRuntime fake;
  std::unique_ptr<grpc::Server> server;
  int port = 0;
  bool start() {
    grpc::ServerBuilder builder;
    builder.RegisterService(&fake);
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    server = builder.BuildAndStart();
    return server && port > 0;
  }
  std::string endpoint() const {
    return "127.0.0.1:" + std::to_string(port);
  }
};
p::HostRuntimeClientOptions options(const std::string& endpoint) {
  p::HostRuntimeClientOptions value;
  value.endpoint = endpoint;
  // Fixture transport only; the real HostComposition uses mTLS credentials.
  value.channel_credentials = grpc::InsecureChannelCredentials();
  value.budget = 2s;
  value.maximum_budget = 5s;
  value.fence = []() -> t::Outcome<t::HostFence> { return fence(); };
  return value;
}
std::unique_ptr<p::HostRuntimeClient> client(const std::string& endpoint) {
  auto created = p::HostRuntimeClient::create(options(endpoint));
  check(std::holds_alternative<std::unique_ptr<p::HostRuntimeClient>>(created), "client creation");
  return std::get<std::unique_ptr<p::HostRuntimeClient>>(std::move(created));
}
void creation_validation() {
  auto bad = options("");
  check(std::holds_alternative<t::Failure>(p::HostRuntimeClient::create(bad)), "empty endpoint");
  bad = options("127.0.0.1:1");
  bad.channel_credentials = nullptr;
  check(std::holds_alternative<t::Failure>(p::HostRuntimeClient::create(bad)), "no credentials");
  bad = options("127.0.0.1:1");
  bad.fence = {};
  check(std::holds_alternative<t::Failure>(p::HostRuntimeClient::create(bad)), "no fence source");
  bad = options("127.0.0.1:1");
  bad.budget = std::chrono::milliseconds::zero();
  check(std::holds_alternative<t::Failure>(p::HostRuntimeClient::create(bad)), "zero budget");
  bad = options("127.0.0.1:1");
  bad.maximum_budget = 1s;
  check(std::holds_alternative<t::Failure>(p::HostRuntimeClient::create(bad)), "budget above max");
}
void success_path(Server& server) {
  auto value = client(server.endpoint());
  const auto request = gate_request();
  const auto result = value->set_admission(request);
  check(std::holds_alternative<t::HostAdmissionGateReceipt>(result), "successful gate");
  const auto& receipt = std::get<t::HostAdmissionGateReceipt>(result);
  check(receipt.operation == request.operation, "receipt operation round trip");
  check(receipt.maintenance_version == request.maintenance_version, "receipt maintenance version");
  check(receipt.gate_version == 1 && !receipt.admission_open, "receipt gate version and admission");
  std::lock_guard lock(server.fake.mutex);
  check(server.fake.calls == 1 && server.fake.last.has_value(), "one runtime call");
  const auto& wire = *server.fake.last;
  check(wire.host_id() == kHost && wire.fence().instance_id() == kHost &&
            wire.fence().connection_generation() == 7 &&
            wire.fence().runtime_epoch() == "runtime-epoch" &&
            !wire.fence().has_execution_generation(),
        "current host bridge fence");
  check(wire.maintenance_version() == 11 && !wire.has_expected_gate_version() &&
            wire.admission() == v1::HOST_ADMISSION_CLOSED,
        "maintenance intent");
  check(wire.context().request_id().size() == 36 && wire.context().trace_id().size() == 36 &&
            wire.context().idempotency_key().size() == 36 && wire.context().has_deadline(),
        "typed bounded context");
  check(wire.operation().owner_service() == "host" &&
            wire.operation().resource().kind() == v1::RESOURCE_HOST &&
            wire.operation().resource().id() == kHost && !wire.operation().operation_id().empty(),
        "host-owned operation ref");
}
void expected_version_and_open(Server& server) {
  auto value = client(server.endpoint());
  auto request = gate_request(true);
  request.expected_gate_version = 9;
  check(std::holds_alternative<t::HostAdmissionGateReceipt>(value->set_admission(request)),
        "successful resume gate");
  std::lock_guard lock(server.fake.mutex);
  check(server.fake.last->has_expected_gate_version() &&
            server.fake.last->expected_gate_version() == 9 &&
            server.fake.last->admission() == v1::HOST_ADMISSION_OPEN,
        "expected gate version and open admission forwarded");
}
void fence_rejections() {
  auto broken = options("127.0.0.1:1");
  broken.fence = []() -> t::Outcome<t::HostFence> {
    return t::Failure{t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted,
                      t::Recovery::Backoff, std::nullopt, std::nullopt};
  };
  auto failed = p::HostRuntimeClient::create(broken);
  auto failed_client = std::get<std::unique_ptr<p::HostRuntimeClient>>(std::move(failed));
  const auto result = failed_client->set_admission(gate_request());
  check(std::holds_alternative<t::Failure>(result) &&
            std::get<t::Failure>(result).code == t::ErrorCode::UpstreamUnavailable,
        "fence dependency failure fails closed");
  auto foreign = options("127.0.0.1:1");
  foreign.fence = []() -> t::Outcome<t::HostFence> {
    return t::HostFence{t::ResourceId{std::string{"22222222-2222-4222-8222-222222222222"}}, 7,
                        "runtime-epoch"};
  };
  auto foreign_client =
      std::get<std::unique_ptr<p::HostRuntimeClient>>(p::HostRuntimeClient::create(foreign));
  const auto mismatched = foreign_client->set_admission(gate_request());
  check(std::holds_alternative<t::Failure>(mismatched) &&
            std::get<t::Failure>(mismatched).code == t::ErrorCode::Unauthenticated,
        "gate for another host never sent");
  auto empty = options("127.0.0.1:1");
  empty.fence = []() -> t::Outcome<t::HostFence> {
    return t::HostFence{t::ResourceId{std::string{kHost}}, 0, {}};
  };
  auto empty_client =
      std::get<std::unique_ptr<p::HostRuntimeClient>>(p::HostRuntimeClient::create(empty));
  check(std::holds_alternative<t::Failure>(empty_client->set_admission(gate_request())),
        "missing current registration never sends");
}
void response_validation(Server& server) {
  auto value = client(server.endpoint());
  const std::pair<FakeRuntime::Mode, t::ErrorCode> cases[] = {
      {FakeRuntime::Mode::MismatchHost, t::ErrorCode::UpstreamUnavailable},
      {FakeRuntime::Mode::MismatchMaintenance, t::ErrorCode::UpstreamUnavailable},
      {FakeRuntime::Mode::MismatchAdmission, t::ErrorCode::UpstreamUnavailable},
      {FakeRuntime::Mode::ZeroGateVersion, t::ErrorCode::UpstreamUnavailable},
      {FakeRuntime::Mode::WrongOperation, t::ErrorCode::UpstreamUnavailable},
      {FakeRuntime::Mode::Unauthenticated, t::ErrorCode::Unauthenticated},
      {FakeRuntime::Mode::PermissionDenied, t::ErrorCode::PermissionDenied},
      {FakeRuntime::Mode::Conflict, t::ErrorCode::Conflict},
      {FakeRuntime::Mode::Unavailable, t::ErrorCode::UpstreamUnavailable},
      {FakeRuntime::Mode::Timeout, t::ErrorCode::Timeout},
      {FakeRuntime::Mode::Internal, t::ErrorCode::Internal},
  };
  for (const auto& [mode, expected] : cases) {
    server.fake.mode = mode;
    const auto result = value->set_admission(gate_request());
    const auto* error = std::get_if<t::Failure>(&result);
    check(error && error->code == expected, "stable failure mapping");
  }
  server.fake.mode = FakeRuntime::Mode::Success;
}
void unreachable_runtime() {
  auto value = client("127.0.0.1:1");
  const auto result = value->set_admission(gate_request());
  check(std::holds_alternative<t::Failure>(result), "unreachable runtime never succeeds");
  check(!value->available(), "availability reflects the channel");
}
} // namespace

int main() {
  creation_validation();
  fence_rejections();
  Server server;
  check(server.start(), "loopback fake runtime starts");
  success_path(server);
  expected_version_and_open(server);
  response_validation(server);
  server.server->Shutdown();
  server.server->Wait();
  unreachable_runtime();
  if (failures != 0) {
    std::cerr << "host runtime client failures=" << failures << '\n';
    return 1;
  }
  std::cout << "Host runtime admission gate client wire/validation fixtures passed\n";
  return 0;
}
