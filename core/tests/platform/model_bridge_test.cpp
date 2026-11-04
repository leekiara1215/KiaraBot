// P11e target PluginBridge fixture: real TLS gRPC PluginBridge + registered plugin + fake
// RuntimeModelPort simulating the Model outcomes. Covers HostRequest.model_chat success, replay,
// UNKNOWN/QUERY_SAME_INTENT, 429, timeout, revoked delegation, permission and validation errors.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "kiarabot/core/application/target/runtime_bridge_application.hpp"
#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/application/target/runtime_task_application.hpp"
#include "kiarabot/core/ports/target/runtime_model.hpp"
#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"
#include "kiarabot/v1/plugins.pb.h"
#include "kiarabot/v1/services.pb.h"

namespace {
namespace target = kiarabot::core::target;
namespace wire = kiarabot::v1;
using namespace std::chrono_literals;

void require(bool value, std::string_view message) {
  if (!value) {
    throw std::runtime_error("P11e bridge assertion: " + std::string{message});
  }
}
target::TimePoint now() {
  return target::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
google::protobuf::Timestamp timestamp(target::TimePoint value, int64_t offset_seconds = 0) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch());
  google::protobuf::Timestamp result;
  result.set_seconds(seconds.count() + offset_seconds);
  return result;
}
target::Failure bridge_failure(target::ErrorCode code,
                               target::Acceptance acceptance = target::Acceptance::NotAccepted,
                               target::Recovery recovery = target::Recovery::Diagnose) {
  return {code, acceptance, recovery, {}, {}};
}

struct TlsMaterial {
  std::string certificate;
  std::string key;
};
std::string bio_string(BIO* bio) {
  BUF_MEM* memory = nullptr;
  BIO_get_mem_ptr(bio, &memory);
  return memory == nullptr ? std::string{} : std::string{memory->data, memory->length};
}
TlsMaterial make_tls_material() {
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_RSA_gen(2048), EVP_PKEY_free);
  require(key != nullptr, "RSA key generation");
  const std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
  require(certificate != nullptr, "X509 allocation");
  X509_set_version(certificate.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
  X509_set_pubkey(certificate.get(), key.get());
  X509_NAME* name = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
  X509_set_issuer_name(certificate.get(), name);
  X509V3_CTX context{};
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
  const std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
      X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name,
                          const_cast<char*>("DNS:localhost,IP:127.0.0.1")),
      X509_EXTENSION_free);
  require(san != nullptr, "SAN extension");
  X509_add_ext(certificate.get(), san.get(), -1);
  require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0, "certificate signing");
  const std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(BIO_new(BIO_s_mem()), BIO_free);
  const std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(BIO_new(BIO_s_mem()), BIO_free);
  require(PEM_write_bio_X509(cert_bio.get(), certificate.get()) == 1, "PEM certificate");
  require(PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                   nullptr) == 1,
          "PEM private key");
  return {bio_string(cert_bio.get()), bio_string(key_bio.get())};
}

// Fake application: registration, frame authorization, profile and stream sink ownership are
// implemented. Business surfaces unused by this fixture fail closed.
class FakeBridge final : public target::RuntimeBridgeApplication {
public:
  bool grant_model_chat = true;
  std::optional<target::InstallationId> installation;
  std::vector<std::string> features{"host.model.v1"};
  std::vector<std::string> subscriptions;
  std::vector<std::string> profile_permissions{"task.schedule", "task.read"};
  std::vector<std::string> registered_features;
  target::ErrorCode refresh_failure = target::ErrorCode::Unauthenticated;
  bool mismatched_profile = false;
  std::vector<std::string> cleanup;
  int registrations = 0;
  int config_mutations = 0;
  target::BridgeAuthorization authorization;
  std::optional<target::DeliverySink> delivery_sink;
  std::optional<target::ShutdownSink> shutdown_sink;

  target::Outcome<target::BridgeRegistrationResult>
  register_adapter(const target::BridgeAdapterRegistration&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::BridgeRegistrationResult>
  register_plugin(const target::BridgePluginRegistration& request) override {
    ++registrations;
    registered_features = request.supported_features;
    authorization = target::BridgeAuthorization{};
    authorization.fence.instance = target::InstanceId{request.instance_id};
    authorization.fence.runtime_epoch = target::RuntimeEpoch{"epoch-1"};
    authorization.fence.execution_generation = target::ExecutionGeneration{1};
    authorization.fence.connection_generation =
        target::ConnectionGeneration{static_cast<std::uint64_t>(registrations)};
    authorization.proof.fill(std::byte{0x11});
    authorization.refresh_credential.fill(std::byte{0x22});
    authorization.refresh_generation = 1;
    authorization.credential_version = 1;
    authorization.authorization_version = 1;
    authorization.issued_at = now();
    authorization.expires_at = target::TimePoint{now().time_since_epoch() + 56s};
    target::BridgeRegistrationResult result;
    result.assigned_instance_id = authorization.fence.instance;
    result.authorization = authorization;
    for (const auto& permission : request.manifest.permissions) {
      if (permission != "model.chat" || grant_model_chat) {
        result.granted_permissions.push_back(permission);
      }
    }
    result.enabled_features = request.supported_features;
    result.required_api_version = "kiarabot.plugin.v1";
    result.required_protocol_version = "kiarabot.v1";
    return result;
  }
  target::Outcome<target::BridgeAuthorization>
  refresh(target::BridgePeerKind, const target::InstanceId&, const target::BridgeAuthorization&,
          const target::BridgeRefreshRequest&) override {
    return bridge_failure(refresh_failure);
  }
  target::Status authorize_frame(target::BridgePeerKind kind,
                                 const target::BridgeAuthorization& current,
                                 const target::BridgeFence& fence,
                                 const std::array<std::byte, 32>& proof,
                                 target::TimePoint at) override {
    if (kind != target::BridgePeerKind::Plugin || fence != current.fence ||
        proof != current.proof || at >= target::TimePoint{current.expires_at.time_since_epoch()}) {
      return bridge_failure(target::ErrorCode::Unauthenticated);
    }
    return std::monostate{};
  }
  target::Status disconnect(target::BridgePeerKind, const target::InstanceId&,
                            target::ConnectionGeneration) override {
    cleanup.push_back("disconnect");
    require(!delivery_sink && !shutdown_sink, "sinks detached before disconnect");
    return std::monostate{};
  }
  target::Outcome<target::BridgeEventAck> ingest_event(const target::InstanceId&,
                                                       const target::BridgeEventIngress&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Status attach_plugin_delivery(const target::InstanceId& instance,
                                        target::ConnectionGeneration generation,
                                        const target::DeliverySink& sink) override {
    if (instance != authorization.fence.instance ||
        generation != authorization.fence.connection_generation) {
      return bridge_failure(target::ErrorCode::PermissionDenied);
    }
    delivery_sink = sink;
    return std::monostate{};
  }
  void detach_plugin_delivery(const target::InstanceId& instance,
                              target::ConnectionGeneration generation) override {
    if (instance == authorization.fence.instance &&
        generation == authorization.fence.connection_generation) {
      cleanup.push_back("delivery");
      delivery_sink.reset();
    }
  }
  target::Outcome<BridgePluginProfile> plugin_profile(const target::InstanceId&) override {
    auto permissions = profile_permissions;
    if (grant_model_chat) {
      permissions.push_back("model.chat");
    }
    return BridgePluginProfile{
        std::move(permissions), installation,
        registered_features.empty() ? features : registered_features, subscriptions,
        mismatched_profile ? target::InstanceId{"another-instance"} : authorization.fence.instance};
  }
  target::Outcome<target::ActionRecord>
  submit_plugin_action(const target::InstanceId&, const target::BridgeActionSubmit&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::ActionRecord> query_plugin_action(const target::InstanceId&,
                                                            std::optional<target::ActionId>,
                                                            std::optional<std::string>) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::ActionRecord> cancel_plugin_action(const target::InstanceId&,
                                                             const target::ActionId&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::ActionReportReceipt>
  record_adapter_result(const target::InstanceId&, const target::BridgeActionReport&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::ActionReportReceipt>
  record_adapter_evidence(const target::InstanceId&, const target::BridgeEvidenceReport&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Status attach_adapter_action_sink(const target::InstanceId&, target::ConnectionGeneration,
                                            const target::ActionDispatchSink&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  void detach_adapter_action_sink(const target::InstanceId&,
                                  target::ConnectionGeneration) override {}
  target::Status maintenance() override {
    return std::monostate{};
  }
  target::Status attach_shutdown_sink(target::BridgePeerKind kind,
                                      const target::InstanceId& instance,
                                      target::ConnectionGeneration generation,
                                      const target::ShutdownSink& sink) override {
    if (kind != target::BridgePeerKind::Plugin || instance != authorization.fence.instance ||
        generation != authorization.fence.connection_generation) {
      return bridge_failure(target::ErrorCode::PermissionDenied);
    }
    shutdown_sink = sink;
    return std::monostate{};
  }
  void detach_shutdown_sink(target::BridgePeerKind kind, const target::InstanceId& instance,
                            target::ConnectionGeneration generation) override {
    if (kind == target::BridgePeerKind::Plugin && instance == authorization.fence.instance &&
        generation == authorization.fence.connection_generation) {
      cleanup.push_back("shutdown");
      shutdown_sink.reset();
    }
  }
  target::Status shutdown_notice() override {
    return std::monostate{};
  }
  target::Outcome<target::DeliveryReceipt>
  acknowledge_delivery(const target::InstanceId&, const target::BridgeAttemptRef&, bool,
                       std::optional<target::Failure>, bool) override {
    return bridge_failure(target::ErrorCode::NotFound);
  }
  target::Outcome<target::DeliveryReceipt>
  renew_delivery(const target::InstanceId&, const target::BridgeAttemptRef&) override {
    return bridge_failure(target::ErrorCode::NotFound);
  }
  target::Outcome<target::TimePoint> current_time() override {
    return now();
  }
  bool available() const override {
    return true;
  }
};

struct FakeModel final : target::RuntimeModelPort {
  enum class Mode { Success, Unknown, RateLimited, Timeout, Revoked };
  Mode mode = Mode::Success;
  int provider_calls = 0;
  int attempts = 0;
  std::vector<std::string> logical_call_ids;
  std::map<std::string, target::RuntimeModelChatReply> terminal;

  target::Outcome<target::RuntimeModelChatReply>
  chat(const target::RuntimeModelChatRequest& request, target::TimePoint) override {
    ++attempts;
    logical_call_ids.push_back(request.logical_call_id);
    if (mode == Mode::Revoked) {
      return bridge_failure(target::ErrorCode::PermissionDenied);
    }
    if (mode == Mode::Unknown) {
      return bridge_failure(target::ErrorCode::UpstreamUnavailable, target::Acceptance::Unknown,
                            target::Recovery::QuerySameIdentity);
    }
    if (mode == Mode::RateLimited) {
      return bridge_failure(target::ErrorCode::RateLimited, target::Acceptance::NotAccepted,
                            target::Recovery::Backoff);
    }
    if (mode == Mode::Timeout) {
      return bridge_failure(target::ErrorCode::Timeout, target::Acceptance::Unknown,
                            target::Recovery::QuerySameIdentity);
    }
    // Simulated Model replay semantics: a duplicate logical_call_id returns the recorded reply
    // without invoking the provider again.
    if (const auto found = terminal.find(request.logical_call_id); found != terminal.end()) {
      return found->second;
    }
    ++provider_calls;
    target::RuntimeModelChatReply reply{"answer", 3, 4};
    terminal.emplace(request.logical_call_id, reply);
    return reply;
  }
  bool available() const noexcept override {
    return true;
  }
};

struct FakeTaskPort final : target::SchedulerTaskPort {
  enum class Mode { Success, Denied, Conflict, Timeout, Unavailable };
  Mode mode = Mode::Success;
  int create_calls = 0;
  int get_calls = 0;
  int cancel_calls = 0;
  std::map<std::string, target::ScheduledTask> tasks;
  target::SchedulerTaskContext last_context;
  target::TaskCreationInput last_input;

  bool available() const override {
    return mode != Mode::Unavailable;
  }
  target::Outcome<target::ScheduledTask>
  create_task(const target::SchedulerTaskContext& context,
              const target::TaskCreationInput& input) override {
    ++create_calls;
    last_context = context;
    last_input = input;
    if (mode == Mode::Denied)
      return bridge_failure(target::ErrorCode::PermissionDenied);
    if (mode == Mode::Conflict)
      return bridge_failure(target::ErrorCode::Conflict);
    if (mode == Mode::Timeout) {
      return bridge_failure(target::ErrorCode::Timeout, target::Acceptance::Unknown,
                            target::Recovery::QuerySameIdentity);
    }
    auto found = tasks.find(input.task_id);
    if (found != tasks.end())
      return found->second;
    target::ScheduledTask task;
    task.key = {context.owner, input.task_id};
    task.installation = context.installation;
    task.target = input.target;
    task.payload = input.payload;
    task.run_at = input.run_at.value_or(now());
    task.version = target::ResourceVersion{1};
    task.occurrence = target::MessageId{"occurrence-" + input.task_id};
    tasks.emplace(input.task_id, task);
    return task;
  }
  target::Outcome<target::ScheduledTask> get_task(const target::SchedulerTaskContext& context,
                                                  std::string_view task_id) override {
    ++get_calls;
    last_context = context;
    const auto found = tasks.find(std::string{task_id});
    if (found == tasks.end())
      return bridge_failure(target::ErrorCode::NotFound);
    return found->second;
  }
  target::Outcome<target::ScheduledTask> cancel_task(const target::SchedulerTaskContext& context,
                                                     std::string_view task_id) override {
    ++cancel_calls;
    last_context = context;
    const auto found = tasks.find(std::string{task_id});
    if (found == tasks.end())
      return bridge_failure(target::ErrorCode::NotFound);
    if (found->second.state == target::TaskState::Completed) {
      return bridge_failure(target::ErrorCode::Conflict);
    }
    found->second.state = target::TaskState::Cancelled;
    found->second.version.value += 1;
    return found->second;
  }
};

struct CountingStorage final : target::RuntimeStorageApplication {
  int calls = 0;
  target::Failure rejected() {
    ++calls;
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::StorageEntryResult>
  get_storage(const target::StorageGetRequest&) override {
    return rejected();
  }
  target::Outcome<target::StorageEntryResult>
  set_storage(const target::StorageSetRequest&) override {
    return rejected();
  }
  target::Outcome<target::StorageDeleteResult>
  delete_storage(const target::StorageDeleteRequest&) override {
    return rejected();
  }
  target::Outcome<target::StorageEntryResult>
  compare_and_set_storage(const target::StorageCompareAndSetRequest&) override {
    return rejected();
  }
  target::Outcome<target::NamespaceMutation>
  create_shared_namespace(const target::CreateSharedNamespaceRequest&) override {
    return rejected();
  }
  target::Outcome<target::NamespaceView>
  get_shared_namespace(const target::GetSharedNamespaceQuery&) override {
    return rejected();
  }
  target::Outcome<target::SharedMembersMutation>
  set_shared_members(const target::SetSharedMembersRequest&) override {
    return rejected();
  }
  target::Outcome<target::SharedMembersMutation>
  disable_shared_namespace(const target::DisableSharedNamespaceRequest&) override {
    return rejected();
  }
};

struct Fixture {
  TlsMaterial tls = make_tls_material();
  FakeBridge app;
  FakeModel model;
  FakeTaskPort tasks;
  CountingStorage storage;
  std::unique_ptr<target::RuntimeTaskApplication> task_app;
  std::unique_ptr<wire::PluginBridge::Service> service;
  std::unique_ptr<grpc::Server> server;
  int port = 0;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<wire::PluginBridge::Stub> stub;

  explicit Fixture(bool grant_model_chat = true, bool task_provider = true,
                   bool storage_provider = false) {
    app.grant_model_chat = grant_model_chat;
    app.installation = target::InstallationId{"55555555-5555-4555-8555-555555555555"};
    app.features = {"host.model.v1", "host.tasks.v1"};
    app.subscriptions = {"event.timer.triggered"};
    task_app = std::make_unique<target::RuntimeTaskApplication>(
        target::RuntimeTaskApplicationDependencies{task_provider ? &tasks : nullptr});
    auto built = kiarabot::core::platform::make_plugin_bridge(
        app, kiarabot::core::platform::PluginBridgeOptions{storage_provider ? &storage : nullptr,
                                                           &model, task_app.get()});
    require(std::holds_alternative<std::unique_ptr<wire::PluginBridge::Service>>(built),
            "make_plugin_bridge");
    service = std::move(std::get<std::unique_ptr<wire::PluginBridge::Service>>(built));
    grpc::SslServerCredentialsOptions options;
    options.pem_key_cert_pairs.push_back({tls.key, tls.certificate});
    grpc::ServerBuilder builder;
    builder.RegisterService(service.get());
    builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
    server = builder.BuildAndStart();
    require(server != nullptr && port > 0, "TLS server start");
    grpc::SslCredentialsOptions client;
    client.pem_root_certs = tls.certificate;
    channel =
        grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(client));
    stub = wire::PluginBridge::NewStub(channel);
  }
  ~Fixture() {
    if (server) {
      server->Shutdown(std::chrono::system_clock::now() + 1s);
      server->Wait();
    }
  }
};

struct PluginConnection {
  std::string case_name;
  FakeBridge& application;
  std::unique_ptr<grpc::ClientContext> context;
  std::unique_ptr<grpc::ClientReaderWriter<wire::PluginToCore, wire::CoreToPlugin>> stream;
  wire::ConnectionAuthorization authorization;
  std::string instance = "44444444-4444-4444-8444-444444444444";
  std::vector<std::string> permissions;
  std::vector<std::string> features;
  std::vector<std::string> capabilities;

  explicit PluginConnection(Fixture& fixture, std::string_view scenario,
                            std::vector<std::string> plugin_permissions = {"model.chat"},
                            std::vector<std::string> plugin_features = {"host.model.v1"},
                            std::vector<std::string> plugin_capabilities = {})
      : case_name(scenario), application(fixture.app), permissions(std::move(plugin_permissions)),
        features(std::move(plugin_features)), capabilities(std::move(plugin_capabilities)) {
    context = std::make_unique<grpc::ClientContext>();
    context->set_deadline(std::chrono::system_clock::now() + 20s);
    context->AddMetadata("authorization", "Bearer " + std::string(32, 'p'));
    stream = fixture.stub->Connect(context.get());
    wire::PluginToCore register_frame;
    auto* registration = register_frame.mutable_register_();
    registration->set_instance_id(instance);
    registration->mutable_manifest()->set_id("plugin-1");
    registration->mutable_manifest()->set_name("plugin-1");
    registration->mutable_manifest()->set_version("1.0.0");
    registration->mutable_manifest()->set_api_version("kiarabot.plugin.v1");
    registration->mutable_manifest()->set_runtime("python-rpc");
    registration->mutable_manifest()->set_isolation("process");
    registration->mutable_manifest()->set_entrypoint("plugin:create");
    for (const auto& permission : permissions) {
      registration->mutable_manifest()->add_permissions(permission);
    }
    for (const auto& feature : features) {
      registration->add_supported_features(feature);
    }
    for (const auto& capability : capabilities) {
      registration->mutable_manifest()->add_capabilities(capability);
    }
    if (!stream->Write(register_frame)) {
      fail_stream("register-write", 0);
    }
    wire::CoreToPlugin response;
    if (!stream->Read(&response) || !response.has_register_result() ||
        !response.register_result().accepted()) {
      fail_stream("register-read", 0);
    }
    authorization = response.register_result().authorization();
  }
  wire::PluginToCore frame(wire::HostRequest request) {
    wire::PluginToCore frame;
    frame.mutable_fence()->CopyFrom(authorization.fence());
    frame.set_connection_proof(authorization.proof());
    *frame.mutable_host_request() = std::move(request);
    return frame;
  }
  wire::HostRequest chat_request(std::string call_id, std::string text = "hello") {
    wire::HostRequest request;
    request.set_request_id("request-" + call_id);
    request.set_trace_id("trace-" + call_id);
    *request.mutable_deadline() = timestamp(now(), 5);
    request.set_idempotency_key("idempotency-" + call_id);
    auto* chat = request.mutable_model_chat();
    chat->set_model("default");
    chat->set_logical_call_id(std::move(call_id));
    auto* message = chat->add_messages();
    message->set_role("user");
    message->set_text(std::move(text));
    return request;
  }
  wire::HostRequest task_request(const char* kind, std::string task_id = "task-1",
                                 std::string idem = "idem-1") {
    wire::HostRequest request;
    request.set_request_id("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    request.set_trace_id("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    request.set_idempotency_key(std::move(idem));
    *request.mutable_deadline() = timestamp(now(), 5);
    if (std::string_view{kind} == "create") {
      auto* task = request.mutable_task_create();
      task->set_task_id(std::move(task_id));
      task->set_target_conversation("conversation-1");
      task->set_payload("payload");
      *task->mutable_run_at() = timestamp(now(), 60);
    } else if (std::string_view{kind} == "get") {
      request.mutable_task_get()->set_task_id(std::move(task_id));
    } else {
      request.mutable_task_cancel()->set_task_id(std::move(task_id));
    }
    return request;
  }
  [[noreturn]] void fail_stream(std::string_view phase, int request_operation) {
    stream->WritesDone();
    wire::CoreToPlugin ignored;
    while (stream->Read(&ignored)) {
    }
    const auto status = stream->Finish();
    // Only fixed fixture labels and the stable status code are safe diagnostics. Frames and
    // arbitrary transport messages can contain connection proofs or other authorization material.
    std::cerr << "model bridge case=" << case_name << " phase=" << phase
              << " request_operation=" << request_operation
              << " grpc_code=" << static_cast<int>(status.error_code()) << '\n';
    throw std::runtime_error("P11e bridge assertion: stream failure");
  }
  wire::HostResponse host(wire::HostRequest request) {
    const auto request_operation = static_cast<int>(request.operation_case());
    if (!stream->Write(frame(std::move(request)))) {
      fail_stream("host-write", request_operation);
    }
    wire::CoreToPlugin response;
    if (!stream->Read(&response) || !response.has_host_response()) {
      fail_stream("host-read", request_operation);
    }
    return response.host_response();
  }
  void close(grpc::StatusCode expected = grpc::StatusCode::OK) {
    stream->WritesDone();
    wire::CoreToPlugin ignored;
    while (stream->Read(&ignored)) {
    }
    const auto status = stream->Finish();
    if (status.error_code() != expected) {
      std::cerr << "model bridge case=" << case_name
                << " phase=close grpc_code=" << static_cast<int>(status.error_code()) << '\n';
    }
    require(status.error_code() == expected,
            "stream completion preserves expected transport status");
    require(!application.delivery_sink && !application.shutdown_sink,
            "connection sinks detach before stream close completes");
    require(application.cleanup == std::vector<std::string>{"delivery", "shutdown", "disconnect"},
            "delivery detach then shutdown detach then disconnect");
    stream.reset();
    context.reset();
  }
};

std::string raw_field(unsigned int tag, bool length_delimited = true) {
  unsigned int value = (tag << 3) | (length_delimited ? 2U : 0U);
  std::string bytes;
  while (value > 127U) {
    bytes.push_back(static_cast<char>((value & 127U) | 128U));
    value >>= 7;
  }
  bytes.push_back(static_cast<char>(value));
  bytes.push_back(length_delimited ? '\0' : '\1');
  return bytes;
}

void host_request_shapes() {
  Fixture fixture(true, true, true);
  PluginConnection connection(fixture, "host-request-shapes", {"model.chat", "task.read"},
                              {"host.model.v1", "host.tasks.v1"});
  auto metadata = connection.task_request("get");
  metadata.clear_operation();
  const auto check = [&](const std::string& raw, wire::ErrorCode expected) {
    wire::HostRequest parsed;
    require(parsed.ParseFromString(raw), "raw HostRequest parse");
    const auto response = connection.host(std::move(parsed));
    require(response.has_error() && response.error().code() == expected &&
                !response.error().retryable() &&
                response.result_case() == wire::HostResponse::RESULT_NOT_SET &&
                response.request_id() == metadata.request_id() &&
                response.error().request_id() == metadata.request_id() &&
                response.error().trace_id() == metadata.trace_id(),
            "operation shape refusal is correlated and has no result");
    require(fixture.model.attempts == 0 && fixture.tasks.create_calls == 0 &&
                fixture.tasks.get_calls == 0 && fixture.tasks.cancel_calls == 0 &&
                fixture.storage.calls == 0,
            "shape refusal never invokes any downstream port");
  };
  const auto envelope = metadata.SerializeAsString();
  check(envelope, wire::INVALID_ARGUMENT);
  check(envelope + raw_field(128), wire::INVALID_ARGUMENT);
  check(envelope + raw_field(100), wire::CAPABILITY_NOT_SUPPORTED);
  check(envelope + raw_field(100, false), wire::INVALID_ARGUMENT);
  check(envelope + raw_field(100) + raw_field(101), wire::INVALID_ARGUMENT);
  check(envelope + raw_field(100) + raw_field(100), wire::INVALID_ARGUMENT);
  check(envelope + raw_field(10) + raw_field(100), wire::INVALID_ARGUMENT);
  check(envelope + raw_field(16) + raw_field(100), wire::INVALID_ARGUMENT);
  // An authenticated stream remains usable after refusal; ordinary metadata is ignored.
  wire::HostRequest accepted;
  require(accepted.ParseFromString(connection.chat_request("shape-valid").SerializeAsString() +
                                   raw_field(128)),
          "known operation with future metadata parse");
  const auto success = connection.host(std::move(accepted));
  require(success.has_model_chat() && !success.has_error() && fixture.model.attempts == 1,
          "known operation continues after shape refusals");
  wire::HostRequest last_wins;
  require(last_wins.ParseFromString(connection.chat_request("not-dispatched").SerializeAsString() +
                                    connection.task_request("get").SerializeAsString()),
          "standard repeated known oneof parse");
  require(last_wins.has_task_get(), "known protobuf oneof uses last-one-wins");
  const auto missing = connection.host(std::move(last_wins));
  require(missing.has_error() && missing.error().code() == wire::NOT_FOUND &&
              fixture.tasks.get_calls == 1 && fixture.model.attempts == 1,
          "only final known operation dispatches");
  connection.close();
}

void run() {
  Fixture fixture;
  PluginConnection connection(fixture, "model-outcomes");
  const auto success = connection.host(connection.chat_request("call-success"));
  require(!success.has_error() && success.has_model_chat() &&
              success.model_chat().text() == "answer" && success.model_chat().input_tokens() == 3 &&
              success.model_chat().output_tokens() == 4,
          "success response");
  require(fixture.model.logical_call_ids == std::vector<std::string>{"call-success"},
          "logical_call_id forwarded");
  // Replay: the same logical_call_id returns the recorded reply without a second provider call.
  const auto replay = connection.host(connection.chat_request("call-success"));
  require(!replay.has_error() && replay.model_chat().text() == "answer" &&
              fixture.model.provider_calls == 1,
          "replay does not invoke provider twice");
  fixture.model.mode = FakeModel::Mode::Unknown;
  const auto unknown = connection.host(connection.chat_request("call-unknown"));
  require(unknown.has_error() && unknown.error().code() == wire::UPSTREAM_UNAVAILABLE &&
              unknown.error().retryable(),
          "UNKNOWN maps to retryable UPSTREAM_UNAVAILABLE/QuerySameIntent");
  fixture.model.mode = FakeModel::Mode::RateLimited;
  const auto limited = connection.host(connection.chat_request("call-rate"));
  require(limited.has_error() && limited.error().code() == wire::RATE_LIMITED &&
              limited.error().retryable(),
          "429 maps to retryable RATE_LIMITED");
  fixture.model.mode = FakeModel::Mode::Timeout;
  const auto timed = connection.host(connection.chat_request("call-timeout"));
  require(timed.has_error() && timed.error().code() == wire::TIMEOUT && timed.error().retryable(),
          "timeout maps to retryable TIMEOUT");
  fixture.model.mode = FakeModel::Mode::Revoked;
  const auto revoked = connection.host(connection.chat_request("call-revoked"));
  require(revoked.has_error() && revoked.error().code() == wire::PERMISSION_DENIED &&
              !revoked.error().retryable(),
          "revoked delegation is a stable non-retryable denial");
  require(fixture.app.config_mutations == 0, "failed chat mutated desired state");
  // Validation: missing logical_call_id is rejected before the model port.
  auto invalid = connection.chat_request("");
  const auto before = fixture.model.attempts;
  const auto bad = connection.host(std::move(invalid));
  require(bad.has_error() && bad.error().code() == wire::INVALID_ARGUMENT &&
              fixture.model.attempts == before,
          "missing logical_call_id rejected");
  connection.close();

  // Revoked registration grant: model.chat not granted -> permission denied before the port.
  Fixture denied_fixture(false);
  PluginConnection denied(denied_fixture, "model-permission-denied");
  const auto denied_response = denied.host(denied.chat_request("call-denied"));
  require(denied_response.has_error() &&
              denied_response.error().code() == wire::PERMISSION_DENIED &&
              denied_fixture.model.attempts == 0,
          "missing model.chat grant denied before model");
  denied.close();

  // P10k: authenticated PluginBridge task ingress -> RuntimeTaskApplication -> SchedulerTaskPort.
  Fixture task_fixture;
  PluginConnection tasks(task_fixture, "task-outcomes", {"task.schedule", "task.read"},
                         {"host.tasks.v1"}, {"event.timer.triggered"});
  const auto created = tasks.host(tasks.task_request("create"));
  require(!created.has_error() && created.has_task_create() &&
              created.task_create().task_id() == "task-1" &&
              created.task_create().state() == wire::SCHEDULED_TASK_PENDING &&
              created.task_create().version() == 1 && task_fixture.tasks.create_calls == 1,
          "task create ingress reaches SchedulerTaskPort");
  require(task_fixture.tasks.last_context.owner.value == tasks.instance &&
              task_fixture.tasks.last_context.installation.value ==
                  "55555555-5555-4555-8555-555555555555" &&
              task_fixture.tasks.last_context.idempotency_key == "idem-1" &&
              task_fixture.tasks.last_input.task_id == "task-1" &&
              task_fixture.tasks.last_input.run_at.has_value(),
          "task owner/installation derived from authenticated session, not wire");
  const auto replay_task = tasks.host(tasks.task_request("create"));
  require(!replay_task.has_error() && replay_task.task_create().version() == 1 &&
              task_fixture.tasks.tasks.size() == 1,
          "duplicate task create is idempotent at the port");
  const auto fetched = tasks.host(tasks.task_request("get"));
  require(!fetched.has_error() && fetched.task_get().task_id() == "task-1" &&
              task_fixture.tasks.get_calls == 1,
          "task get ingress reaches SchedulerTaskPort");
  const auto cancelled = tasks.host(tasks.task_request("cancel"));
  require(!cancelled.has_error() &&
              cancelled.task_cancel().state() == wire::SCHEDULED_TASK_CANCELLED &&
              task_fixture.tasks.cancel_calls == 1,
          "task cancel ingress reaches SchedulerTaskPort");
  task_fixture.tasks.mode = FakeTaskPort::Mode::Conflict;
  const auto conflict = tasks.host(tasks.task_request("create", "task-2", "idem-2"));
  require(conflict.has_error() && conflict.error().code() == wire::CONFLICT &&
              !conflict.error().retryable(),
          "Scheduler CONFLICT maps to stable non-retryable wire CONFLICT");
  task_fixture.tasks.mode = FakeTaskPort::Mode::Timeout;
  const auto timeout = tasks.host(tasks.task_request("create", "task-3", "idem-3"));
  require(timeout.has_error() && timeout.error().code() == wire::TIMEOUT &&
              timeout.error().retryable(),
          "Scheduler timeout maps to retryable wire TIMEOUT");
  task_fixture.tasks.mode = FakeTaskPort::Mode::Denied;
  const auto denied_task = tasks.host(tasks.task_request("create", "task-4", "idem-4"));
  require(denied_task.has_error() && denied_task.error().code() == wire::PERMISSION_DENIED &&
              !denied_task.error().retryable(),
          "Scheduler authorization denial maps to stable wire PERMISSION_DENIED");
  const auto read_calls = task_fixture.tasks.get_calls;
  task_fixture.app.profile_permissions.clear();
  const auto revoked_task = tasks.host(tasks.task_request("get"));
  require(revoked_task.has_error() && revoked_task.error().code() == wire::PERMISSION_DENIED &&
              task_fixture.tasks.get_calls == read_calls,
          "each ingress reads current server grants instead of reusing an admitted profile");
  tasks.close();

  // A registered plugin without task.schedule cannot create tasks even with host.tasks.v1.
  Fixture missing_permission_fixture;
  missing_permission_fixture.app.profile_permissions = {"task.read"};
  PluginConnection missing_permission(missing_permission_fixture, "task-permission-denied",
                                      {"task.schedule", "task.read"}, {"host.tasks.v1"},
                                      {"event.timer.triggered"});
  const auto permission_denied = missing_permission.host(missing_permission.task_request("create"));
  require(permission_denied.has_error() &&
              permission_denied.error().code() == wire::PERMISSION_DENIED &&
              missing_permission_fixture.tasks.create_calls == 0,
          "missing task.schedule grant denied before the task port");
  missing_permission.close();

  // Port reports unavailable -> fail closed as retryable UPSTREAM_UNAVAILABLE.
  Fixture unavailable_fixture;
  unavailable_fixture.tasks.mode = FakeTaskPort::Mode::Unavailable;
  PluginConnection unavailable_task(unavailable_fixture, "task-port-unavailable",
                                    {"task.schedule", "task.read"}, {"host.tasks.v1"},
                                    {"event.timer.triggered"});
  const auto port_unavailable = unavailable_task.host(unavailable_task.task_request("create"));
  require(port_unavailable.has_error() &&
              port_unavailable.error().code() == wire::UPSTREAM_UNAVAILABLE &&
              port_unavailable.error().retryable() && unavailable_fixture.tasks.create_calls == 0,
          "unavailable SchedulerTaskPort fails closed");
  unavailable_task.close();

  // No host.tasks.v1 feature -> capability rejected before the port.
  Fixture no_feature_fixture;
  PluginConnection no_feature(no_feature_fixture, "task-feature-missing", {"task.schedule"},
                              {"host.model.v1"}, {"event.timer.triggered"});
  const auto unsupported = no_feature.host(no_feature.task_request("create"));
  require(unsupported.has_error() && unsupported.error().code() == wire::CAPABILITY_NOT_SUPPORTED &&
              no_feature_fixture.tasks.create_calls == 0,
          "missing host.tasks.v1 feature rejected before the task port");
  no_feature.close();

  Fixture no_subscription_fixture;
  no_subscription_fixture.app.subscriptions.clear();
  PluginConnection no_subscription(no_subscription_fixture, "task-subscription-missing",
                                   {"task.schedule"}, {"host.tasks.v1"});
  const auto unsubscribed = no_subscription.host(no_subscription.task_request("create"));
  require(unsubscribed.has_error() && unsubscribed.error().code() == wire::PERMISSION_DENIED &&
              no_subscription_fixture.tasks.create_calls == 0,
          "missing TimerTriggered subscription denied without downstream side effects");
  no_subscription.close();

  Fixture mismatched_fixture;
  mismatched_fixture.app.mismatched_profile = true;
  PluginConnection mismatched(mismatched_fixture, "task-profile-identity-mismatch",
                              {"task.schedule"}, {"host.tasks.v1"}, {"event.timer.triggered"});
  const auto wrong_owner = mismatched.host(mismatched.task_request("create"));
  require(wrong_owner.has_error() && wrong_owner.error().code() == wire::UNAUTHENTICATED &&
              mismatched_fixture.tasks.create_calls == 0,
          "profile binding mismatch denies before SchedulerTaskPort");
  // Business ingress refusal is a HostResponse and preserves the connection for a heartbeat.
  wire::PluginToCore heartbeat;
  heartbeat.mutable_fence()->CopyFrom(mismatched.authorization.fence());
  heartbeat.set_connection_proof(mismatched.authorization.proof());
  heartbeat.mutable_heartbeat();
  require(mismatched.stream->Write(heartbeat), "heartbeat after business denial");
  wire::CoreToPlugin heartbeat_ack;
  require(mismatched.stream->Read(&heartbeat_ack) && heartbeat_ack.has_heartbeat_ack(),
          "business rejection keeps the stream alive");
  mismatched.close();

  Fixture refresh_unavailable_fixture;
  refresh_unavailable_fixture.app.refresh_failure = target::ErrorCode::UpstreamUnavailable;
  PluginConnection refresh_unavailable(refresh_unavailable_fixture, "refresh-dependency-failure");
  wire::PluginToCore refresh_frame;
  refresh_frame.mutable_fence()->CopyFrom(refresh_unavailable.authorization.fence());
  refresh_frame.set_connection_proof(refresh_unavailable.authorization.proof());
  auto* refresh = refresh_frame.mutable_refresh_authorization();
  refresh->set_request_id("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
  refresh->mutable_fence()->CopyFrom(refresh_unavailable.authorization.fence());
  refresh->set_proof(refresh_unavailable.authorization.proof());
  refresh->set_refresh_credential(refresh_unavailable.authorization.refresh_credential());
  refresh->set_refresh_generation(refresh_unavailable.authorization.refresh_generation());
  require(refresh_unavailable.stream->Write(refresh_frame), "write refresh request");
  wire::CoreToPlugin refresh_response;
  // Baseline characterization: the existing refresh error builder supplies an empty trace_id,
  // which the shared codec maps to INTERNAL. Preserve this existing response while retaining the
  // original dependency failure as the terminal gRPC status; this is not the desired contract.
  require(refresh_unavailable.stream->Read(&refresh_response) &&
              refresh_response.has_authorization_result() &&
              refresh_response.authorization_result().has_error() &&
              refresh_response.authorization_result().error().code() == wire::INTERNAL,
          "baseline refresh error response precedes dependency transport termination");
  refresh_unavailable.close(grpc::StatusCode::UNAVAILABLE);

  Fixture malformed_ack_fixture;
  PluginConnection malformed_ack(malformed_ack_fixture, "malformed-ack-terminates");
  wire::PluginToCore ack_frame;
  ack_frame.mutable_fence()->CopyFrom(malformed_ack.authorization.fence());
  ack_frame.set_connection_proof(malformed_ack.authorization.proof());
  ack_frame.mutable_event_ack()->set_accepted(true);
  require(malformed_ack.stream->Write(ack_frame), "write malformed ACK");
  malformed_ack.close(grpc::StatusCode::INVALID_ARGUMENT);

  // Missing task port/provider -> fail closed, never a fabricated task.
  Fixture fail_closed_fixture(true, false);
  PluginConnection fail_closed(fail_closed_fixture, "task-provider-missing",
                               {"task.schedule", "task.read"}, {"host.tasks.v1"},
                               {"event.timer.triggered"});
  const auto unavailable = fail_closed.host(fail_closed.task_request("create"));
  require(unavailable.has_error() && unavailable.error().code() == wire::UPSTREAM_UNAVAILABLE &&
              unavailable.error().retryable() && fail_closed_fixture.tasks.create_calls == 0,
          "missing SchedulerTaskPort fails closed");
  fail_closed.close();
}

} // namespace

int main() {
  try {
    host_request_shapes();
    run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "model bridge TLS fixture: ok\n";
  return 0;
}
