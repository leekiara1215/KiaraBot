#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/host.grpc.pb.h>

#include "kiarabot/core/runtime/platform/host_management_rpc.hpp"
#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace p = kiarabot::core::platform;
namespace v1 = kiarabot::v1;
namespace codec = p::target_codec;
using namespace std::chrono_literals;

constexpr char kHost[] = "11111111-1111-4111-8111-111111111111";
constexpr char kAdmin[] = "22222222-2222-4222-8222-222222222222";
constexpr char kActor[] = "33333333-3333-4333-8333-333333333333";
constexpr char kRequest[] = "44444444-4444-4444-8444-444444444444";
constexpr char kTrace[] = "55555555-5555-4555-8555-555555555555";
constexpr char kOperation[] = "66666666-6666-4666-8666-666666666666";
constexpr char kIdempotency[] = "77777777-7777-4777-8777-777777777777";

std::string server_certificate;
std::string server_key;
std::string ca_certificate;
std::string client_certificate;
std::string client_key;
std::string client_fingerprint;

std::string read(const char* path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
int failures = 0;
void check(bool value, const char* name) {
  if (!value) {
    ++failures;
    std::cerr << "HostManagement RPC fixture assertion: " << name << '\n';
  }
}
t::TimePoint clock_now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
std::int64_t unix_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
t::AuthenticationContext caller() {
  t::AuthenticationContext context;
  context.subject = t::SubjectId{kAdmin};
  context.direct_peer = t::PlatformPeer{t::Service::Control, {kActor}};
  context.audience = t::Service::Host;
  context.actors = {t::SubjectId{kAdmin}};
  context.scopes = {t::ResourceScope{{t::ResourceKind::Host, {kHost}}, "host.read"}};
  context.authorization_version = {5};
  context.verified_at = clock_now();
  context.expires_at = clock_now() + 30s;
  context.proof = "delegation-proof-fixture";
  return context;
}
t::Operation operation() {
  t::Operation value;
  value.ref = t::OperationRef{t::Service::Host, {kOperation}, {t::ResourceKind::Host, {kHost}}};
  value.version = {2};
  value.state = t::OperationState::Running;
  value.execution_outcome = t::ExecutionOutcome::NotStarted;
  value.updated_at = clock_now();
  value.participants = {{"runtime_gate", t::OperationState::Succeeded, {}, {}},
                        {"managed_executions", t::OperationState::Running, {}, {}}};
  return value;
}
t::HostMaintenanceOperation maintenance() {
  t::HostMaintenanceOperation value;
  value.operation = operation();
  value.intent.action = t::HostMaintenanceAction::Begin;
  value.intent.grace_ms = 5000;
  return value;
}
t::HostMaintenanceState state(bool open) {
  t::HostMaintenanceState value;
  value.host = t::ResourceId{kHost};
  value.version = {4};
  value.admission_open = open;
  value.active_operation = operation().ref;
  value.observed_at = clock_now();
  return value;
}
t::OperationObservationPage page() {
  t::OperationObservationPage value;
  t::OperationObservation change;
  change.operation = operation();
  change.cursor = {t::Service::Host, "epoch", {{t::ResourceKind::Host, {kHost}}}, 3};
  value.next_cursor = change.cursor;
  value.changes.push_back(std::move(change));
  return value;
}
std::string base64url(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char character : input) {
    buffer = (buffer << 8U) | character;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      output.push_back(kAlphabet[(buffer >> static_cast<unsigned>(bits)) & 0x3fU]);
    }
  }
  if (bits > 0) {
    output.push_back(kAlphabet[(buffer << static_cast<unsigned>(6 - bits)) & 0x3fU]);
  }
  return output;
}
// Well-formed but unsigned delegation envelope. It proves only the transport-side structural
// decode; Identity signature/current-authority verification is the job of the online authorizer.
std::string delegation_proof(std::string_view audience = "host") {
  const auto now = unix_seconds();
  std::string document;
  document.reserve(1024);
  document += "{\"sub\":\"" + std::string{kAdmin} + "\",\"aud\":\"" + std::string{audience} +
              "\",\"jti\":\"" + kOperation + "\",\"iss\":\"" + kActor +
              "\",\"iat\":" + std::to_string(now - 1) + ",\"nbf\":" + std::to_string(now - 1) +
              ",\"exp\":" + std::to_string(now + 30) + ",\"delegation\":{\"direct_actor\":\"" +
              kActor + "\",\"actor_chain\":[\"" + kActor + "\"],\"permissions\":[[\"9\",\"5\",\"" +
              kHost + "\"]],\"client_certificate\":\"" + client_fingerprint +
              "\",\"authorization_version\":\"5\",\"business_hops\":\"0\","
              "\"source_credential\":[\"" +
              std::string{kActor} + "\",\"1\"],\"scope\":[]}}";
  return base64url("{}") + "." + base64url(document) + "." + base64url("sig");
}
class FakeApplication final : public t::HostManagementApplicationPort {
public:
  const t::ResourceId& host_id() const noexcept override {
    return host;
  }
  bool deny_state = false;
  t::Outcome<t::HostMaintenanceState> state_projection() override {
    return maintenance_state(t::AuthenticationContext{}, t::TimePoint::max());
  }
  t::ResourceId host{std::string{kHost}};
  std::optional<t::HostMaintenanceRequest> last_request;
  std::optional<t::HostMaintenanceCancelRequest> last_cancel;
  std::optional<t::AuthenticationContext> last_context;
  int calls = 0;
  enum class Mode { Success, Failure, NotFound, Conflict };
  Mode mode = Mode::Success;
  t::Outcome<t::HostMaintenanceState> maintenance_state(const t::AuthenticationContext& context,
                                                        t::TimePoint) override {
    ++calls;
    last_context = context;
    if (mode == Mode::Failure) {
      return failure();
    }
    return state(true);
  }
  t::Outcome<t::HostMaintenanceOperation> begin(const t::HostMaintenanceRequest& request) override {
    ++calls;
    last_request = request;
    if (mode == Mode::Failure) {
      return failure();
    }
    return maintenance();
  }
  t::Outcome<t::HostMaintenanceOperation>
  resume(const t::HostMaintenanceRequest& request) override {
    ++calls;
    last_request = request;
    if (mode == Mode::Failure) {
      return failure();
    }
    return maintenance();
  }
  t::Outcome<t::HostMaintenanceOperation>
  cancel(const t::HostMaintenanceCancelRequest& request) override {
    ++calls;
    last_cancel = request;
    if (mode == Mode::Failure) {
      return failure();
    }
    if (mode == Mode::Conflict) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    return maintenance();
  }
  t::Outcome<t::HostMaintenanceOperation> operation(const t::AuthenticationContext& context,
                                                    t::TimePoint, const t::OperationId&) override {
    ++calls;
    last_context = context;
    if (mode == Mode::Failure) {
      return failure();
    }
    if (mode == Mode::NotFound) {
      return t::Failure{t::ErrorCode::NotFound, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    return maintenance();
  }
  t::Outcome<t::OperationObservationPage> changes(const t::AuthenticationContext& context,
                                                  t::TimePoint,
                                                  t::OperationObservationQuery) override {
    ++calls;
    last_context = context;
    if (mode == Mode::Failure) {
      return failure();
    }
    return page();
  }

private:
  static t::Failure failure() {
    return {t::ErrorCode::UpstreamUnavailable, t::Acceptance::Unknown,
            t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
};
void wire_context(v1::RequestContext* context, bool expired = false) {
  context->set_request_id(kRequest);
  context->set_trace_id(kTrace);
  context->set_idempotency_key(kIdempotency);
  const auto deadline = expired ? clock_now() - 5s : clock_now() + 20s;
  const auto encoded = codec::to_wire(deadline);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *context->mutable_deadline() = *wire;
  }
}
v1::ResourceRef host_ref() {
  v1::ResourceRef value;
  value.set_kind(v1::RESOURCE_HOST);
  value.set_id(kHost);
  return value;
}
v1::OperationRef operation_ref() {
  v1::OperationRef value;
  value.set_owner_service("host");
  value.set_operation_id(kOperation);
  *value.mutable_resource() = host_ref();
  return value;
}
struct Harness {
  FakeApplication application;
  bool reject = false;
  bool peer_certificate_seen = false;
  std::string last_method;
  std::optional<v1::ResourceRef> last_scope;
  std::unique_ptr<v1::HostManagementService::Service> service;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<v1::HostManagementService::Stub> stub;
  int port = 0;
  ~Harness() {
    if (server) {
      server->Shutdown();
      server->Wait();
    }
  }
  bool start() {
    auto created = p::make_host_management_rpc(p::HostManagementRpcDependencies{
        application,
        [this](grpc::ServerContext& transport, const v1::RequestContext&, std::string_view method,
               const v1::ResourceRef& scope) -> t::Outcome<p::HostManagementPeer> {
          last_method = std::string(method);
          last_scope = scope;
          const auto evidence = p::extract_peer_certificate(*transport.auth_context());
          const auto* certificate = std::get_if<p::PeerCertificateEvidence>(&evidence);
          peer_certificate_seen = certificate && !certificate->der_chain.empty();
          if (reject) {
            return t::Failure{t::ErrorCode::Unauthenticated, t::Acceptance::NotAccepted,
                              t::Recovery::Authenticate, std::nullopt, std::nullopt};
          }
          return p::HostManagementPeer{caller(), clock_now() + 30s};
        },
        {}});
    if (!std::holds_alternative<std::unique_ptr<v1::HostManagementService::Service>>(created)) {
      return false;
    }
    service = std::get<std::unique_ptr<v1::HostManagementService::Service>>(std::move(created));
    grpc::SslServerCredentialsOptions options;
    options.pem_root_certs = ca_certificate;
    options.pem_key_cert_pairs.push_back({server_key, server_certificate});
    options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    grpc::ServerBuilder builder;
    builder.RegisterService(service.get());
    builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
    server = builder.BuildAndStart();
    if (!server || port <= 0) {
      return false;
    }
    grpc::SslCredentialsOptions client;
    client.pem_root_certs = ca_certificate;
    client.pem_cert_chain = client_certificate;
    client.pem_private_key = client_key;
    stub = v1::HostManagementService::NewStub(
        grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(client)));
    return true;
  }
};
std::unique_ptr<grpc::ClientContext> client_context(bool expired = false) {
  auto context = std::make_unique<grpc::ClientContext>();
  context->set_deadline(std::chrono::system_clock::now() + (expired ? -1s : 10s));
  return context;
}
std::optional<v1::RpcFailure> stable_failure(const grpc::Status& status) {
  if (status.ok()) {
    return std::nullopt;
  }
  const auto decoded = codec::from_grpc_failure(status);
  const auto* failure = std::get_if<v1::RpcFailure>(&decoded);
  if (!failure) {
    return std::nullopt;
  }
  return *failure;
}
bool failure_code(const grpc::Status& status, t::ErrorCode expected) {
  const auto failure = stable_failure(status);
  if (!failure) {
    return false;
  }
  const auto code = codec::from_wire(failure->error().code());
  return std::holds_alternative<t::ErrorCode>(code) && std::get<t::ErrorCode>(code) == expected;
}
void get_maintenance(Harness& harness) {
  v1::HostGetMaintenanceRequest request;
  wire_context(request.mutable_context());
  request.set_host_id(kHost);
  v1::HostGetMaintenanceResponse response;
  auto context = client_context();
  check(harness.stub->GetMaintenance(context.get(), request, &response).ok(), "GetMaintenance ok");
  check(harness.peer_certificate_seen, "real mTLS peer certificate observed");
  check(harness.last_method == "kiarabot.v1.HostManagementService/GetMaintenance",
        "GetMaintenance method");
  check(harness.last_scope && harness.last_scope->kind() == v1::RESOURCE_HOST &&
            harness.last_scope->id() == kHost,
        "GetMaintenance host scope");
  check(response.maintenance().host_id() == kHost && response.maintenance().version() == 4 &&
            response.maintenance().admission() == v1::HOST_ADMISSION_OPEN &&
            response.maintenance().operation().operation_id() == kOperation &&
            response.maintenance().has_observed_at(),
        "maintenance record projection");
  auto foreign_host = request;
  foreign_host.set_host_id("22222222-2222-4222-8222-222222222222");
  v1::HostGetMaintenanceResponse foreign_response;
  auto foreign_context = client_context();
  check(failure_code(
            harness.stub->GetMaintenance(foreign_context.get(), foreign_host, &foreign_response),
            t::ErrorCode::NotFound),
        "another host label is NOT_FOUND, never a cross-host projection");
  auto denied = request;
  denied.set_host_id(std::string(129, 'x'));
  v1::HostGetMaintenanceResponse denied_response;
  auto denied_context = client_context();
  check(failure_code(harness.stub->GetMaintenance(denied_context.get(), denied, &denied_response),
                     t::ErrorCode::InvalidArgument),
        "empty/oversized host rejected");
  harness.reject = true;
  v1::HostGetMaintenanceResponse rejected_response;
  auto rejected_context = client_context();
  const auto status =
      harness.stub->GetMaintenance(rejected_context.get(), request, &rejected_response);
  check(harness.peer_certificate_seen && status.error_code() == grpc::StatusCode::UNAUTHENTICATED,
        "verified peer certificate still required for rejected authorizer");
  harness.reject = false;
  auto expired = request;
  wire_context(expired.mutable_context(), true);
  v1::HostGetMaintenanceResponse expired_response;
  auto expired_context = client_context();
  check(
      failure_code(harness.stub->GetMaintenance(expired_context.get(), expired, &expired_response),
                   t::ErrorCode::Timeout),
      "expired deadline");
}
void begin_and_resume(Harness& harness) {
  v1::HostBeginMaintenanceRequest request;
  wire_context(request.mutable_context());
  request.set_host_id(kHost);
  request.set_expected_version(3);
  request.set_grace_ms(4500);
  v1::HostMaintenanceResponse response;
  auto context = client_context();
  check(harness.stub->BeginMaintenance(context.get(), request, &response).ok(),
        "BeginMaintenance ok");
  check(harness.application.last_request && harness.application.last_request->host_id == kHost &&
            harness.application.last_request->expected_version == 3 &&
            harness.application.last_request->grace_ms == 4500 &&
            harness.application.last_request->request_id == kRequest &&
            harness.application.last_request->idempotency_key == kIdempotency,
        "BeginMaintenance typed application command");
  check(response.operation().ref().operation_id() == kOperation &&
            response.maintenance().version() == 4,
        "BeginMaintenance response");
  // The mutation response is authorized by host.execute only: a second host.read check would turn
  // a successful Begin into PERMISSION_DENIED for an execute-only delegation.
  harness.application.deny_state = true;
  v1::HostBeginMaintenanceRequest execute_only = request;
  execute_only.clear_expected_version();
  execute_only.set_expected_version(3);
  v1::HostMaintenanceResponse execute_only_response;
  auto execute_only_context = client_context();
  check(harness.stub
                ->BeginMaintenance(execute_only_context.get(), execute_only, &execute_only_response)
                .ok() &&
            execute_only_response.maintenance().version() == 4,
        "execute-only delegation still receives the mutation projection");
  harness.application.deny_state = false;
  auto missing_version = request;
  missing_version.clear_expected_version();
  v1::HostMaintenanceResponse missing_response;
  auto missing_context = client_context();
  check(failure_code(harness.stub->BeginMaintenance(missing_context.get(), missing_version,
                                                    &missing_response),
                     t::ErrorCode::InvalidArgument),
        "BeginMaintenance requires expected version");
  auto missing_idempotency = request;
  missing_idempotency.mutable_context()->clear_idempotency_key();
  v1::HostMaintenanceResponse idempotency_response;
  auto idempotency_context = client_context();
  check(failure_code(harness.stub->BeginMaintenance(idempotency_context.get(), missing_idempotency,
                                                    &idempotency_response),
                     t::ErrorCode::InvalidArgument),
        "BeginMaintenance requires idempotency key");
  harness.application.mode = FakeApplication::Mode::Failure;
  v1::HostMaintenanceResponse dependency_response;
  auto dependency_context = client_context();
  const auto dependency_status =
      harness.stub->BeginMaintenance(dependency_context.get(), request, &dependency_response);
  const auto dependency = stable_failure(dependency_status);
  check(dependency_status.error_code() == grpc::StatusCode::UNAVAILABLE && dependency &&
            dependency->context().acceptance() == v1::ACCEPTANCE_UNKNOWN,
        "application Unknown failure kept unconfirmed");
  harness.application.mode = FakeApplication::Mode::Success;
  v1::HostResumeAdmissionRequest resume;
  wire_context(resume.mutable_context());
  resume.set_host_id(kHost);
  resume.set_expected_version(4);
  v1::HostMaintenanceResponse resume_response;
  auto resume_context = client_context();
  check(harness.stub->ResumeAdmission(resume_context.get(), resume, &resume_response).ok(),
        "ResumeAdmission ok");
  check(harness.last_method == "kiarabot.v1.HostManagementService/ResumeAdmission",
        "ResumeAdmission method");
}
void operation_calls(Harness& harness) {
  v1::HostGetOperationRequest request;
  wire_context(request.mutable_context());
  *request.mutable_operation() = operation_ref();
  v1::HostGetOperationResponse response;
  auto context = client_context();
  check(harness.stub->GetOperation(context.get(), request, &response).ok(), "GetOperation ok");
  check(response.operation().ref().owner_service() == "host" &&
            response.operation().state() == v1::OPERATION_RUNNING,
        "GetOperation projection");
  harness.application.mode = FakeApplication::Mode::NotFound;
  v1::HostGetOperationResponse missing_response;
  auto missing_context = client_context();
  check(failure_code(harness.stub->GetOperation(missing_context.get(), request, &missing_response),
                     t::ErrorCode::NotFound),
        "GetOperation hidden operation not found");
  harness.application.mode = FakeApplication::Mode::Success;
  auto foreign_operation = request;
  foreign_operation.mutable_operation()->mutable_resource()->set_id(
      "22222222-2222-4222-8222-222222222222");
  v1::HostGetOperationResponse foreign_operation_response;
  auto foreign_operation_context = client_context();
  check(failure_code(harness.stub->GetOperation(foreign_operation_context.get(), foreign_operation,
                                                &foreign_operation_response),
                     t::ErrorCode::NotFound),
        "cross-host operation label is NOT_FOUND");
  auto wrong_owner = request;
  wrong_owner.mutable_operation()->set_owner_service("runtime");
  v1::HostGetOperationResponse wrong_response;
  auto wrong_context = client_context();
  check(failure_code(harness.stub->GetOperation(wrong_context.get(), wrong_owner, &wrong_response),
                     t::ErrorCode::InvalidArgument),
        "GetOperation rejects foreign owner");
  v1::HostCancelOperationRequest cancel;
  wire_context(cancel.mutable_context());
  *cancel.mutable_operation() = operation_ref();
  cancel.set_expected_version(2);
  v1::HostGetOperationResponse cancel_response;
  auto cancel_context = client_context();
  check(harness.stub->CancelOperation(cancel_context.get(), cancel, &cancel_response).ok(),
        "CancelOperation ok");
  check(harness.application.last_cancel &&
            harness.application.last_cancel->operation.value == kOperation &&
            harness.application.last_cancel->context.expected_version == 2 &&
            harness.application.last_cancel->context.host_id == kHost,
        "CancelOperation typed command");
  harness.application.mode = FakeApplication::Mode::Conflict;
  v1::HostGetOperationResponse conflict_response;
  auto conflict_context = client_context();
  check(failure_code(
            harness.stub->CancelOperation(conflict_context.get(), cancel, &conflict_response),
            t::ErrorCode::Conflict),
        "CancelOperation conflict");
  harness.application.mode = FakeApplication::Mode::Success;
  auto missing_version = cancel;
  missing_version.clear_expected_version();
  v1::HostGetOperationResponse version_response;
  auto version_context = client_context();
  check(failure_code(harness.stub->CancelOperation(version_context.get(), missing_version,
                                                   &version_response),
                     t::ErrorCode::InvalidArgument),
        "CancelOperation requires expected version");
}
void list_changes(Harness& harness) {
  v1::OperationChangesRequest request;
  wire_context(request.mutable_context());
  request.mutable_context()->set_delegation(delegation_proof());
  request.mutable_scope()->add_resources()->CopyFrom(host_ref());
  request.set_page_size(10);
  v1::OperationChangesResponse response;
  auto context = client_context();
  const auto status = harness.stub->ListOperationChanges(context.get(), request, &response);
  check(status.ok(), "ListOperationChanges ok");
  check(response.changes_size() == 1 &&
            response.changes(0).operation().ref().operation_id() == kOperation &&
            response.next_cursor().source_service() == "host" &&
            response.next_cursor().sequence() == 3,
        "ListOperationChanges projection");
  const auto* decoded_peer =
      harness.application.last_context
          ? std::get_if<t::PlatformPeer>(&harness.application.last_context->direct_peer)
          : nullptr;
  check(decoded_peer && decoded_peer->service == t::Service::Control &&
            harness.application.last_context->subject.value == kAdmin,
        "ListOperationChanges decoded Control delegation");
  auto wrong_audience = request;
  wrong_audience.mutable_context()->set_delegation(delegation_proof("runtime"));
  v1::OperationChangesResponse audience_response;
  auto audience_context = client_context();
  check(failure_code(harness.stub->ListOperationChanges(audience_context.get(), wrong_audience,
                                                        &audience_response),
                     t::ErrorCode::Unauthenticated),
        "ListOperationChanges rejects other audience");
  auto other_scope = request;
  other_scope.mutable_scope()->mutable_resources(0)->set_id("22222222-2222-4222-8222-222222222222");
  v1::OperationChangesResponse scope_response;
  auto scope_context = client_context();
  check(harness.stub->ListOperationChanges(scope_context.get(), other_scope, &scope_response).ok(),
        "ListOperationChanges keeps transport scope wiring");
}
} // namespace

int main(int argc, char* argv[]) {
  if (argc != 7) {
    return 2;
  }
  server_certificate = read(argv[1]);
  server_key = read(argv[2]);
  ca_certificate = read(argv[3]);
  client_certificate = read(argv[4]);
  client_key = read(argv[5]);
  client_fingerprint = argv[6];
  try {
    Harness harness;
    if (!harness.start()) {
      std::cerr << "HostManagement RPC fixture assertion line 0; diagnostics redacted\n";
      return 1;
    }
    get_maintenance(harness);
    begin_and_resume(harness);
    operation_calls(harness);
    list_changes(harness);
  } catch (...) {
    std::cerr << "HostManagement RPC fixture assertion line 0; diagnostics redacted\n";
    return 1;
  }
  if (failures != 0) {
    std::cerr << "HostManagement RPC fixture failures=" << failures << "; diagnostics redacted\n";
    return 1;
  }
  std::cout << "HostManagement RPC mTLS/application fixtures passed\n";
  return 0;
}
