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

// Fake application: only registration/frame authorization/profile are implemented; every other
// bridge surface is unused and fails closed.
class FakeBridge final : public target::RuntimeBridgeApplication {
public:
  bool grant_model_chat = true;
  int registrations = 0;
  int config_mutations = 0;
  target::BridgeAuthorization authorization;

  target::Outcome<target::BridgeRegistrationResult>
  register_adapter(const target::BridgeAdapterRegistration&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Outcome<target::BridgeRegistrationResult>
  register_plugin(const target::BridgePluginRegistration& request) override {
    ++registrations;
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
    return bridge_failure(target::ErrorCode::Unauthenticated);
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
    return std::monostate{};
  }
  target::Outcome<target::BridgeEventAck> ingest_event(const target::InstanceId&,
                                                       const target::BridgeEventIngress&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  target::Status attach_plugin_delivery(const target::InstanceId&, target::ConnectionGeneration,
                                        const target::DeliverySink&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  void detach_plugin_delivery(const target::InstanceId&, target::ConnectionGeneration) override {}
  target::Outcome<BridgePluginProfile> plugin_profile(const target::InstanceId&) override {
    return BridgePluginProfile{grant_model_chat ? std::vector<std::string>{"model.chat"}
                                                : std::vector<std::string>{},
                               std::nullopt};
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
  target::Status attach_shutdown_sink(target::BridgePeerKind, const target::InstanceId&,
                                      target::ConnectionGeneration,
                                      const target::ShutdownSink&) override {
    return bridge_failure(target::ErrorCode::CapabilityNotSupported);
  }
  void detach_shutdown_sink(target::BridgePeerKind, const target::InstanceId&,
                            target::ConnectionGeneration) override {}
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

struct Fixture {
  TlsMaterial tls = make_tls_material();
  FakeBridge app;
  FakeModel model;
  std::unique_ptr<wire::PluginBridge::Service> service;
  std::unique_ptr<grpc::Server> server;
  int port = 0;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<wire::PluginBridge::Stub> stub;

  explicit Fixture(bool grant_model_chat = true) {
    app.grant_model_chat = grant_model_chat;
    auto built = kiarabot::core::platform::make_plugin_bridge(
        app, kiarabot::core::platform::PluginBridgeOptions{nullptr, &model});
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
  std::unique_ptr<grpc::ClientContext> context;
  std::unique_ptr<grpc::ClientReaderWriter<wire::PluginToCore, wire::CoreToPlugin>> stream;
  wire::ConnectionAuthorization authorization;
  std::string instance = "44444444-4444-4444-8444-444444444444";

  explicit PluginConnection(Fixture& fixture) {
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
    registration->mutable_manifest()->add_permissions("model.chat");
    registration->add_supported_features("host.model.v1");
    require(stream->Write(register_frame), "register frame write");
    wire::CoreToPlugin response;
    require(stream->Read(&response) && response.has_register_result(), "register response");
    require(response.register_result().accepted(), "registration rejected");
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
  wire::HostResponse host(wire::HostRequest request) {
    require(stream->Write(frame(std::move(request))), "host frame write");
    wire::CoreToPlugin response;
    if (!stream->Read(&response) || !response.has_host_response()) {
      auto status = stream->Finish();
      std::cerr << "host read failed: code=" << static_cast<int>(status.error_code())
                << " message=" << status.error_message() << " body=" << response.DebugString()
                << '\n';
      throw std::runtime_error("P11e bridge assertion: host response");
    }
    return response.host_response();
  }
  void close() {
    stream->WritesDone();
    stream.reset();
    context.reset();
  }
};

void run() {
  Fixture fixture;
  PluginConnection connection(fixture);
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
  PluginConnection denied(denied_fixture);
  const auto denied_response = denied.host(denied.chat_request("call-denied"));
  require(denied_response.has_error() &&
              denied_response.error().code() == wire::PERMISSION_DENIED &&
              denied_fixture.model.attempts == 0,
          "missing model.chat grant denied before model");
  denied.close();
}

} // namespace

int main() {
  try {
    run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "model bridge TLS fixture: ok\n";
  return 0;
}
