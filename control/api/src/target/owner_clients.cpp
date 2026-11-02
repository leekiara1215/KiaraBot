#include "kiarabot/control/target/owner_clients.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include "kiarabot/control/target/owner_gateway.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"

namespace kiarabot::control::target {
namespace {
namespace wire = kiarabot::v1;
namespace codec = core::platform::target_codec;

std::string random_identifier() {
  unsigned char bytes[16]{};
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
    return "control-request";
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string value;
  for (const auto byte : bytes) {
    value.push_back(kHex[byte >> 4U]);
    value.push_back(kHex[byte & 0x0fU]);
  }
  return value;
}
std::optional<std::array<unsigned char, SHA256_DIGEST_LENGTH>>
certificate_fingerprint(const std::string& pem) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (!bio) {
    return std::nullopt;
  }
  const std::unique_ptr<X509, decltype(&X509_free)> certificate(
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
  if (!certificate) {
    return std::nullopt;
  }
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  unsigned length = 0;
  if (X509_digest(certificate.get(), EVP_sha256(), digest.data(), &length) != 1 ||
      length != digest.size()) {
    return std::nullopt;
  }
  return digest;
}
std::optional<wire::ErrorCode> extract_error_code(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::UNAUTHENTICATED:
    return wire::UNAUTHENTICATED;
  case grpc::StatusCode::PERMISSION_DENIED:
    return wire::PERMISSION_DENIED;
  case grpc::StatusCode::INVALID_ARGUMENT:
    return wire::INVALID_ARGUMENT;
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return wire::TIMEOUT;
  case grpc::StatusCode::UNAVAILABLE:
    return wire::UPSTREAM_UNAVAILABLE;
  default:
    return std::nullopt;
  }
}
} // namespace

class OwnerClients::Impl final : private DelegationPort, private RuntimeActionsPort {
public:
  Impl(const std::string& identity_endpoint, const std::string& runtime_endpoint,
       const core::platform::TlsMaterial& tls)
      : gateway_(*this, *this) {
    grpc::SslCredentialsOptions ssl;
    ssl.pem_root_certs = tls.ca;
    ssl.pem_cert_chain = tls.certificate;
    ssl.pem_private_key = tls.private_key;
    const auto credentials = grpc::SslCredentials(ssl);
    identity_channel_ = grpc::CreateChannel(identity_endpoint, credentials);
    identity_ = wire::IdentityService::NewStub(identity_channel_);
    if (!runtime_endpoint.empty()) {
      runtime_channel_ = grpc::CreateChannel(runtime_endpoint, credentials);
      runtime_ = wire::RuntimeService::NewStub(runtime_channel_);
    }
    fingerprint_ = certificate_fingerprint(tls.certificate);
  }

  std::optional<AuthHttpResponse> handle(const AuthHttpRequest& request) {
    OwnerHttpRequest input;
    input.method = request.method;
    input.target = request.target;
    input.body = request.body;
    for (const auto& [name, value] : request.headers) {
      input.headers.emplace(name, value);
    }
    const auto remaining = request.deadline - std::chrono::steady_clock::now();
    const auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch() + remaining);
    if (remaining > std::chrono::steady_clock::duration::zero()) {
      input.deadline = core::target::TimePoint{deadline_ns};
    }
    for (const auto& [name, value] : request.headers) {
      std::string lower;
      for (const char character : name) {
        lower.push_back(
            static_cast<char>(character >= 'A' && character <= 'Z' ? character + 32 : character));
      }
      if (lower == "authorization" && value.size() > 7 && (value[0] == 'B' || value[0] == 'b')) {
        input.access_token = value.substr(7);
      }
    }
    auto result = gateway_.handle(input);
    if (!result) {
      return std::nullopt;
    }
    return AuthHttpResponse{result->status, result->body, {}};
  }

private:
  OwnerOutcome<std::string> issue(std::string_view audience, wire::Permission permission,
                                  const wire::ResourceScope& scope, std::string_view access_token,
                                  core::target::TimePoint deadline) override {
    if (!identity_ || access_token.empty()) {
      return OwnerFailure{wire::UNAUTHENTICATED, false};
    }
    wire::IdentityIssueDelegationRequest input;
    auto* context = input.mutable_context();
    context->set_request_id(random_identifier());
    context->set_trace_id(random_identifier());
    const auto wire_deadline = codec::to_wire(deadline);
    if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&wire_deadline)) {
      *context->mutable_deadline() = *timestamp;
    }
    input.set_subject_proof(std::string{access_token});
    input.set_audience(std::string{audience});
    *input.mutable_scope() = scope;
    input.add_permissions(permission);
    if (fingerprint_) {
      input.set_client_certificate_sha256(
          std::string(reinterpret_cast<const char*>(fingerprint_->data()), fingerprint_->size()));
    }
    wire::IdentityIssueDelegationResponse output;
    grpc::ClientContext call;
    const auto remaining =
        deadline -
        core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
            std::chrono::system_clock::now().time_since_epoch())};
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            (std::chrono::system_clock::now() + remaining).time_since_epoch())});
    const auto status = identity_->IssueDelegation(&call, input, &output);
    if (!status.ok()) {
      const auto code = extract_error_code(status);
      return OwnerFailure{code.value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    if (!output.has_delegation() || output.delegation().proof().empty()) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
    }
    return output.delegation().proof();
  }

  OwnerOutcome<wire::RuntimeQueryActionResultResponse>
  query_action(const wire::RuntimeQueryActionResultRequest& request,
               std::string_view delegation) override {
    if (!runtime_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    wire::RuntimeQueryActionResultRequest input = request;
    input.mutable_context()->set_delegation(std::string{delegation});
    wire::RuntimeQueryActionResultResponse output;
    grpc::ClientContext call;
    const auto remaining = codec::from_wire(request.context().deadline());
    if (const auto* timestamp = std::get_if<core::target::TimePoint>(&remaining)) {
      const auto now =
          core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
              std::chrono::system_clock::now().time_since_epoch())};
      if (*timestamp > now) {
        call.set_deadline(std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                (std::chrono::system_clock::now() +
                 core::target::TimePoint::duration{*timestamp - now})
                    .time_since_epoch())});
      }
    }
    const auto status = runtime_->QueryActionResult(&call, input, &output);
    if (!status.ok()) {
      const auto code = extract_error_code(status);
      return OwnerFailure{code.value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    return output;
  }

  OwnerGateway gateway_;
  std::shared_ptr<grpc::Channel> identity_channel_;
  std::unique_ptr<wire::IdentityService::Stub> identity_;
  std::shared_ptr<grpc::Channel> runtime_channel_;
  std::unique_ptr<wire::RuntimeService::Stub> runtime_;
  std::optional<std::array<unsigned char, SHA256_DIGEST_LENGTH>> fingerprint_;
};

OwnerClients::OwnerClients(const std::string& identity_endpoint,
                           const std::string& runtime_endpoint,
                           const core::platform::TlsMaterial& tls)
    : impl_(std::make_unique<Impl>(identity_endpoint, runtime_endpoint, tls)) {}
OwnerClients::~OwnerClients() = default;

std::optional<AuthHttpResponse> OwnerClients::handle(const AuthHttpRequest& request) {
  return impl_->handle(request);
}

} // namespace kiarabot::control::target
