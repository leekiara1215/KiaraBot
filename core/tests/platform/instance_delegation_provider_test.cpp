// P05b-5 provider boundary matrix (unit, fake Identity/Exchange). The parent delivery mechanism is
// pending a user decision (internal RPC vs sealed short-lived material), so this test only proves
// the fail-closed skeleton: no source/missing/expired/wrong-scope/denied parent never reaches a
// Scheduler call and never fabricates a child proof. It is not positive production-chain evidence.
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "kiarabot/core/runtime/platform/instance_delegation_provider.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

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
constexpr auto kInstallation = "22222222-2222-4222-8222-222222222222";

std::string self_signed_pem() {
  EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
  check(raw_context != nullptr, "key context");
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(raw_context,
                                                                      EVP_PKEY_CTX_free);
  check(EVP_PKEY_keygen_init(context.get()) == 1, "keygen init");
  check(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1, "curve");
  EVP_PKEY* raw_key = nullptr;
  check(EVP_PKEY_keygen(context.get(), &raw_key) == 1, "keygen");
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
  X509* raw_cert = X509_new();
  check(raw_cert != nullptr, "cert alloc");
  std::unique_ptr<X509, decltype(&X509_free)> cert(raw_cert, X509_free);
  X509_set_version(cert.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
  X509_gmtime_adj(X509_get_notBefore(cert.get()), 0);
  X509_gmtime_adj(X509_get_notAfter(cert.get()), 3600);
  check(X509_set_pubkey(cert.get(), key.get()) == 1, "set pubkey");
  X509_NAME* subject = X509_get_subject_name(cert.get());
  check(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("runtime-test"), -1, -1,
                                   0) == 1,
        "subject");
  check(X509_sign(cert.get(), key.get(), EVP_sha256()) > 0, "sign");
  BIO* raw_bio = BIO_new(BIO_s_mem());
  check(raw_bio != nullptr, "bio");
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(raw_bio, BIO_free);
  check(PEM_write_bio_X509(bio.get(), cert.get()) == 1, "pem");
  char* data = nullptr;
  const auto length = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<std::size_t>(length));
}

class FakeSource final : public platform::InstanceParentDelegationSource {
public:
  t::Outcome<platform::InstanceParentDelegation> acquire(const t::SchedulerTaskContext&,
                                                         t::Permission) override {
    ++calls;
    if (failure) {
      return *failure;
    }
    return platform::InstanceParentDelegation{proof, platform::delegation_provider_now() +
                                                         std::chrono::seconds{30}};
  }
  int calls = 0;
  std::string proof = "parent-proof";
  std::optional<t::Failure> failure;
};

class FakeIdentity final : public v1::IdentityService::Service {
public:
  grpc::Status ExchangeDelegation(grpc::ServerContext*,
                                  const v1::IdentityExchangeDelegationRequest* request,
                                  v1::IdentityExchangeDelegationResponse* response) override {
    ++calls;
    if (reject) {
      return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "rejected");
    }
    if (request->scope().resources_size() != 2 || request->scope().resources(0).id() != kInstance ||
        request->scope().resources(1).id() != kInstallation ||
        request->client_certificate_sha256().size() != 32 ||
        request->parent_proof() != "parent-proof") {
      return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "bad exchange request");
    }
    auto* proof = response->mutable_delegation();
    proof->set_proof("child-proof");
    proof->set_audience("scheduler");
    proof->set_business_hops(2);
    auto* expires = proof->mutable_expires_at();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    expires->set_seconds(static_cast<std::int64_t>(seconds.count()) + (expired ? -30 : 30));
    return grpc::Status::OK;
  }
  int calls = 0;
  bool reject = false;
  bool expired = false;
};

class Server {
public:
  explicit Server(FakeIdentity& service) {
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

t::SchedulerTaskContext context(std::string instance = kInstance) {
  t::SchedulerTaskContext value;
  value.owner = t::InstanceId{std::move(instance)};
  value.installation = t::InstallationId{kInstallation};
  value.request_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
  value.trace_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  value.deadline = platform::delegation_provider_now() + std::chrono::seconds{2};
  value.idempotency_key = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
  return value;
}
bool failure_is(const t::Outcome<std::string>& value, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&value);
  return failure != nullptr && failure->code == code;
}
} // namespace

int main() {
  grpc_init();
  FakeIdentity identity_service;
  Server server(identity_service);
  auto identity = platform::RuntimeIdentityClient::create(
      {server.endpoint(), grpc::InsecureChannelCredentials(), std::chrono::milliseconds{500},
       std::chrono::milliseconds{1000}});
  check(!std::holds_alternative<t::Failure>(identity), "identity client create");
  auto& identity_client = std::get<std::unique_ptr<platform::RuntimeIdentityClient>>(identity);

  // Missing source is a construction failure, so composition keeps the old unavailable provider.
  auto no_source = platform::RuntimeInstanceDelegationProvider::create(
      {nullptr, self_signed_pem(), identity_client.get(), std::chrono::milliseconds{10000}});
  check(std::holds_alternative<t::Failure>(no_source), "missing source never constructs");

  auto source = std::make_shared<FakeSource>();
  auto provider = platform::RuntimeInstanceDelegationProvider::create(
      {source, self_signed_pem(), identity_client.get(), std::chrono::milliseconds{10000}});
  check(!std::holds_alternative<t::Failure>(provider), "provider create with source");
  auto& delegation =
      std::get<std::unique_ptr<platform::RuntimeInstanceDelegationProvider>>(provider);

  source->failure = t::Failure{t::ErrorCode::UpstreamUnavailable, t::Acceptance::Unknown,
                               t::Recovery::Backoff, std::nullopt, std::nullopt};
  check(failure_is(delegation->scheduler_task_delegation(context(), t::Permission::TaskRead),
                   t::ErrorCode::UpstreamUnavailable),
        "source unavailable stays fail-closed");

  source->failure.reset();
  source->proof.clear();
  check(failure_is(delegation->scheduler_task_delegation(context(), t::Permission::TaskRead),
                   t::ErrorCode::Unauthenticated),
        "empty parent proof rejected");

  source->proof = "parent-proof";
  identity_service.reject = true;
  check(failure_is(delegation->scheduler_task_delegation(context(), t::Permission::TaskRead),
                   t::ErrorCode::PermissionDenied),
        "Identity denial preserved");
  identity_service.reject = false;
  identity_service.expired = true;
  check(failure_is(delegation->scheduler_task_delegation(context(), t::Permission::TaskRead),
                   t::ErrorCode::Unauthenticated),
        "expired child proof rejected");
  identity_service.expired = false;

  const auto accepted = delegation->scheduler_task_delegation(context(), t::Permission::TaskRead);
  check(!std::holds_alternative<t::Failure>(accepted) &&
            std::get<std::string>(accepted) == "child-proof",
        "unit fake exchange returns opaque child proof");
  const auto cached = delegation->scheduler_task_delegation(context(), t::Permission::TaskRead);
  check(!std::holds_alternative<t::Failure>(cached) && identity_service.calls == 3,
        "short cache avoids repeated exchange");

  // Different instance must not reuse the cached proof or be silently accepted; the fake server
  // rejects the mismatched scope and the provider surfaces the denial.
  identity_service.reject = true;
  check(failure_is(delegation->scheduler_task_delegation(
                       context("33333333-3333-4333-8333-333333333333"), t::Permission::TaskRead),
                   t::ErrorCode::PermissionDenied),
        "cross-instance denial preserved");

  auto bad_cert = platform::RuntimeInstanceDelegationProvider::create(
      {source, "not-a-certificate", identity_client.get(), std::chrono::milliseconds{10000}});
  check(std::holds_alternative<t::Failure>(bad_cert), "invalid Runtime certificate rejects config");

  std::cout << "P05b-5 instance delegation provider fail-closed matrix passed\n";
  grpc_shutdown();
  return 0;
}
