#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/model_gateway_application.hpp"
#include "kiarabot/core/runtime/platform/model_rpc.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/model.grpc.pb.h"

namespace {
namespace t = kiarabot::core::target;
namespace p = kiarabot::core::platform;
namespace v1 = kiarabot::v1;
using namespace std::chrono_literals;

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("P11 Model RPC assertion failed at line " +
                             std::to_string(location.line()));
  }
}

class StubProvider final : public t::ModelProviderPort {
public:
  t::ModelProviderResult chat(const t::ModelProviderRequest&) override {
    return {};
  }
};
class StubCrypto final : public t::ModelSecretCryptoPort {
public:
  t::Outcome<std::vector<std::byte>> seal(t::SecretView plaintext, std::span<const std::byte>,
                                          std::array<std::byte, 24>& nonce) override {
    nonce.fill(std::byte{0});
    return std::vector<std::byte>(plaintext.begin(), plaintext.end());
  }
  t::Outcome<t::OwnedSecret> open(std::span<const std::byte>, std::span<const std::byte>,
                                  const std::array<std::byte, 24>&) override {
    return t::Failure{t::ErrorCode::Internal};
  }
};
class StubDigest final : public t::ModelDigestPort {
public:
  t::Outcome<t::Sha256Digest> sha256(std::span<const std::byte>) override {
    return t::Sha256Digest{};
  }
};
class StubIds final : public t::ModelIdPort {
public:
  t::Outcome<std::string> uuid() override {
    return std::string{"66666666-6666-4666-8666-666666666666"};
  }
};
class StubAuthorization final : public t::ModelAuthorizationPort {
public:
  t::Outcome<t::ModelAuthorizationDecision> authorize(const t::ModelCaller&, t::Permission,
                                                      const t::ResourceRef&,
                                                      std::chrono::milliseconds) override {
    return t::ModelAuthorizationDecision{};
  }
};
class StubFactory final : public t::ModelUnitOfWorkFactory {
public:
  t::Outcome<std::unique_ptr<t::ModelUnitOfWork>> begin() override {
    return t::Failure{t::ErrorCode::UpstreamUnavailable};
  }
};

struct Fixture {
  StubProvider provider;
  StubCrypto crypto;
  StubDigest digest;
  StubIds ids;
  StubAuthorization authorization;
  StubFactory factory;
  std::unique_ptr<t::ModelGatewayApplication> application;
  Fixture() {
    application = t::make_model_gateway_application(
        {factory, provider, crypto, digest, ids, authorization, 1});
  }
};

class Server final {
public:
  explicit Server(p::ModelPeerAuthenticator authenticator) {
    fixture_ = std::make_unique<Fixture>();
    auto service = p::make_model_rpc(*fixture_->application, std::move(authenticator));
    require(std::holds_alternative<std::unique_ptr<v1::ModelService::Service>>(service));
    service_ = std::move(std::get<std::unique_ptr<v1::ModelService::Service>>(service));
    builder_.RegisterService(service_.get());
    builder_.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    server_ = builder_.BuildAndStart();
    require(server_ != nullptr && port_ > 0);
    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                   grpc::InsecureChannelCredentials());
    stub_ = v1::ModelService::NewStub(channel_);
  }
  v1::ModelService::Stub& stub() {
    return *stub_;
  }
  void stop() {
    if (server_) {
      server_->Shutdown(std::chrono::system_clock::now() + 2s);
      server_->Wait();
      server_.reset();
    }
  }

private:
  std::unique_ptr<Fixture> fixture_;
  std::unique_ptr<v1::ModelService::Service> service_;
  grpc::ServerBuilder builder_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::ModelService::Stub> stub_;
  int port_ = 0;
};

t::Outcome<t::ModelCaller> authenticate(grpc::ServerContext&) {
  t::ModelCaller caller;
  caller.subject = t::SubjectId{"44444444-4444-4444-8444-444444444444"};
  caller.owner = t::InstanceId{"22222222-2222-4222-8222-222222222222"};
  caller.delegation = "delegation";
  return caller;
}

void context(v1::RequestContext* value, bool idempotent, bool deadline = true) {
  value->set_request_id("request-1");
  value->set_trace_id("trace-1");
  value->set_idempotency_key(idempotent ? "idem-1" : "");
  if (deadline) {
    google::protobuf::Timestamp stamp;
    stamp.set_seconds(1'800'000'030);
    *value->mutable_deadline() = stamp;
  }
}

void rpc_surface() {
  Server server(authenticate);
  grpc::ClientContext context_wire;
  v1::ModelGatewayChatRequest chat;
  v1::ModelGatewayChatResponse chat_reply;
  auto status = server.stub().Chat(&context_wire, chat, &chat_reply);
  require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  const auto decoded = p::target_codec::from_grpc_failure(status);
  require(std::holds_alternative<v1::RpcFailure>(decoded));
  require(std::get<v1::RpcFailure>(decoded).error().code() == v1::INVALID_ARGUMENT);
  require(std::get<v1::RpcFailure>(decoded).context().acceptance() == v1::NOT_ACCEPTED);

  {
    grpc::ClientContext missing_deadline_context;
    v1::ModelGetModelMetadataRequest request;
    context(request.mutable_context(), false, false);
    request.set_model_id("11111111-1111-4111-8111-111111111111");
    v1::ModelGetModelMetadataResponse reply;
    status = server.stub().GetModelMetadata(&missing_deadline_context, request, &reply);
    require(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
  }
  {
    grpc::ClientContext update_context;
    v1::ModelUpdateModelRequest request;
    context(request.mutable_context(), true);
    request.mutable_config()->set_model_id("11111111-1111-4111-8111-111111111111");
    request.mutable_config()->set_alias("default");
    request.mutable_config()->set_provider_kind("openai_chat_completions");
    request.mutable_config()->set_endpoint("https://api.example.com/v1");
    request.mutable_config()->set_provider_model("gpt-test");
    v1::ModelUpdateModelResponse reply;
    status = server.stub().UpdateModel(&update_context, request, &reply);
    // The stub factory cannot persist; the stable storage failure must surface, not a fake success.
    require(status.error_code() == grpc::StatusCode::UNAVAILABLE);
  }

  Server unauthorized(p::ModelPeerAuthenticator{});
  {
    grpc::ClientContext denied_context;
    v1::ModelGetModelMetadataRequest request;
    context(request.mutable_context(), false);
    request.set_model_id("11111111-1111-4111-8111-111111111111");
    v1::ModelGetModelMetadataResponse reply;
    status = unauthorized.stub().GetModelMetadata(&denied_context, request, &reply);
    require(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
  }
  grpc::ClientContext shutdown_context;
  v1::ModelGatewayChatRequest empty;
  v1::ModelGatewayChatResponse empty_reply;
  status = unauthorized.stub().Chat(&shutdown_context, empty, &empty_reply);
  require(status.error_code() == grpc::StatusCode::UNAUTHENTICATED);
}

} // namespace

int main() {
  try {
    rpc_surface();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
