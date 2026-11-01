#include <atomic>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

#include <grpc/grpc_security_constants.h>
#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace {
namespace platform = kiarabot::core::platform;
namespace target = kiarabot::core::target;

void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("peer certificate assertion failed");
  }
}
std::string read(const char* path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
std::string hex(const target::Sha256Digest& digest) {
  std::string result;
  for (const auto byte : digest.bytes) {
    const auto value = std::to_integer<unsigned int>(byte);
    result += "0123456789abcdef"[value >> 4];
    result += "0123456789abcdef"[value & 15];
  }
  return result;
}
class TestContext final : public grpc::AuthContext {
public:
  bool authenticated = true;
  std::map<std::string, std::vector<std::string>> properties;
  bool IsPeerAuthenticated() const override {
    return authenticated;
  }
  std::vector<grpc::string_ref> GetPeerIdentity() const override {
    throw std::logic_error("identity projection must not replace certificate evidence");
  }
  std::string GetPeerIdentityPropertyName() const override {
    throw std::logic_error("unused fixture method");
  }
  std::vector<grpc::string_ref> FindPropertyValues(const std::string& name) const override {
    std::vector<grpc::string_ref> result;
    const auto found = properties.find(name);
    if (found != properties.end()) {
      for (const auto& value : found->second) {
        result.emplace_back(value.data(), value.size());
      }
    }
    return result;
  }
  grpc::AuthPropertyIterator begin() const override {
    throw std::logic_error("unused fixture iterator");
  }
  grpc::AuthPropertyIterator end() const override {
    throw std::logic_error("unused fixture iterator");
  }
  void AddProperty(const std::string&, const grpc::string_ref&) override {
    throw std::logic_error("immutable fixture");
  }
  bool SetPeerIdentityPropertyName(const std::string&) override {
    throw std::logic_error("immutable fixture");
  }
};
void rejected(const TestContext& context) {
  auto result = platform::extract_peer_certificate(context);
  const auto* error = std::get_if<target::Failure>(&result);
  require(error && error->code == target::ErrorCode::Unauthenticated &&
          error->acceptance == target::Acceptance::NotAccepted);
}
class Handler final : public kiarabot::v1::IdentityService::Service {
public:
  explicit Handler(std::string fingerprint) : fingerprint_(std::move(fingerprint)) {}
  grpc::Status Login(grpc::ServerContext* context, const kiarabot::v1::IdentityLoginRequest*,
                     kiarabot::v1::IdentityLoginResponse*) override {
    ++calls;
    const auto auth = context->auth_context();
    if (!auth) {
      return {grpc::StatusCode::UNAUTHENTICATED, "missing transport evidence"};
    }
    const auto result = platform::extract_peer_certificate(*auth);
    const auto* evidence = std::get_if<platform::PeerCertificateEvidence>(&result);
    if (!evidence || hex(evidence->leaf_fingerprint) != fingerprint_ ||
        evidence->der_chain.empty()) {
      return {grpc::StatusCode::UNAUTHENTICATED, "invalid transport evidence"};
    }
    // Evidence fixture only: not a successful business login or authorization.
    return grpc::Status::OK;
  }
  std::atomic<int> calls{0};

private:
  std::string fingerprint_;
};
struct GrpcLifetime {
  GrpcLifetime() {
    grpc_init();
  }
  ~GrpcLifetime() {
    grpc_shutdown();
  }
};
} // namespace

int main(int argc, char* argv[]) {
  if (argc != 5) {
    return 2;
  }
  const GrpcLifetime grpc_lifetime;
  const auto leaf = read(argv[1]);
  const auto ca = read(argv[2]);
  const auto key = read(argv[3]);
  const std::string fingerprint = argv[4];
  TestContext valid;
  valid.properties[GRPC_TRANSPORT_SECURITY_TYPE_PROPERTY_NAME] = {"ssl"};
  valid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME] = {leaf};
  auto check = platform::extract_peer_certificate(valid);
  require(std::holds_alternative<platform::PeerCertificateEvidence>(check));
  require(hex(std::get<platform::PeerCertificateEvidence>(check).leaf_fingerprint) == fingerprint);
  const auto leaf_der = std::get<platform::PeerCertificateEvidence>(check).der_chain;
  auto duplicate_leaf = valid;
  duplicate_leaf.properties[GRPC_X509_PEM_CERT_CHAIN_PROPERTY_NAME] = {leaf};
  check = platform::extract_peer_certificate(duplicate_leaf);
  require(std::get<platform::PeerCertificateEvidence>(check).der_chain == leaf_der);
  auto chain = valid;
  chain.properties[GRPC_X509_PEM_CERT_CHAIN_PROPERTY_NAME] = {ca};
  check = platform::extract_peer_certificate(chain);
  require(std::get<platform::PeerCertificateEvidence>(check).der_chain.size() > leaf_der.size());
  for (int mode = 0; mode < 10; ++mode) {
    auto invalid = valid;
    if (mode == 0) {
      invalid.authenticated = false;
    } else if (mode == 1) {
      invalid.properties[GRPC_TRANSPORT_SECURITY_TYPE_PROPERTY_NAME] = {"insecure"};
    } else if (mode == 2) {
      invalid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME].clear();
    } else if (mode == 3) {
      invalid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME] = {leaf, leaf};
    } else if (mode == 4) {
      invalid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME] = {"untrusted prefix" + leaf};
    } else if (mode == 5) {
      invalid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME] = {leaf + "trailing junk"};
    } else if (mode == 6) {
      invalid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME] = {std::string(65537, 'x')};
    } else if (mode == 7) {
      invalid.properties[GRPC_X509_PEM_CERT_CHAIN_PROPERTY_NAME] = {ca, ca};
    } else if (mode == 8) {
      invalid.properties[GRPC_X509_PEM_CERT_CHAIN_PROPERTY_NAME] = {ca + ca};
    } else {
      invalid.properties[GRPC_X509_PEM_CERT_PROPERTY_NAME] = {leaf + '\0'};
    }
    rejected(invalid);
  }
  Handler handler{fingerprint};
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = ca;
  options.pem_key_cert_pairs.push_back({key, leaf});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.RegisterService(&handler);
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  auto server = builder.BuildAndStart();
  require(server && port > 0);
  for (const bool authenticate : {true, false}) {
    grpc::SslCredentialsOptions credentials;
    credentials.pem_root_certs = ca;
    if (authenticate) {
      credentials.pem_cert_chain = leaf;
      credentials.pem_private_key = key;
    }
    const auto channel =
        grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(credentials));
    auto stub = kiarabot::v1::IdentityService::NewStub(channel);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{2});
    kiarabot::v1::IdentityLoginRequest request;
    kiarabot::v1::IdentityLoginResponse response;
    const auto status = stub->Login(&context, request, &response);
    require(status.ok() == authenticate);
    require(handler.calls.load() == 1);
  }
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds{2});
  server->Wait();
  return 0;
}
