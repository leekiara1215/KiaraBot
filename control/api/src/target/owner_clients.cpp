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
#include "kiarabot/v1/model.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"

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

class OwnerClients::Impl final : private DelegationPort,
                                 private RuntimeActionsPort,
                                 private IdentityPrincipalsPort,
                                 private HealthPort,
                                 private RuntimeResourcesPort,
                                 private SchedulerTasksPort,
                                 private OperationPort {
public:
  Impl(const std::string& identity_endpoint, const std::string& runtime_endpoint,
       const std::string& scheduler_endpoint, const std::string& model_endpoint,
       const std::string& host_endpoint, const core::platform::TlsMaterial& tls) {
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
    if (!scheduler_endpoint.empty()) {
      scheduler_channel_ = grpc::CreateChannel(scheduler_endpoint, credentials);
      scheduler_ = wire::SchedulerService::NewStub(scheduler_channel_);
    }
    if (!model_endpoint.empty()) {
      model_channel_ = grpc::CreateChannel(model_endpoint, credentials);
      model_ = wire::ModelService::NewStub(model_channel_);
    }
    if (!host_endpoint.empty()) {
      host_channel_ = grpc::CreateChannel(host_endpoint, credentials);
    }
    fingerprint_ = certificate_fingerprint(tls.certificate);
    gateway_ = std::make_unique<OwnerGateway>(
        static_cast<DelegationPort&>(*this), static_cast<RuntimeActionsPort&>(*this),
        static_cast<IdentityPrincipalsPort*>(this), static_cast<HealthPort*>(this),
        static_cast<RuntimeResourcesPort*>(this), static_cast<SchedulerTasksPort*>(this),
        static_cast<OperationPort*>(this));
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
    auto result = gateway_->handle(input);
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

  template <typename Request, typename Response>
  OwnerOutcome<Response>
  unary(grpc::Status (wire::RuntimeService::Stub::*method)(grpc::ClientContext*, const Request&,
                                                           Response*),
        Request input, std::string_view delegation) {
    if (!runtime_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    input.mutable_context()->set_delegation(std::string{delegation});
    Response output;
    grpc::ClientContext call;
    const auto deadline = codec::from_wire(input.context().deadline());
    if (const auto* timestamp = std::get_if<core::target::TimePoint>(&deadline)) {
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
    const auto status = (runtime_.get()->*method)(&call, input, &output);
    if (!status.ok()) {
      const auto code = extract_error_code(status);
      return OwnerFailure{code.value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    return output;
  }
  template <typename Request, typename Response>
  OwnerOutcome<Response>
  unary_task(grpc::Status (wire::SchedulerService::Stub::*method)(grpc::ClientContext*,
                                                                  const Request&, Response*),
             Request input, std::string_view delegation) {
    if (!scheduler_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    input.mutable_context()->set_delegation(std::string{delegation});
    Response output;
    grpc::ClientContext call;
    const auto deadline = codec::from_wire(input.context().deadline());
    if (const auto* timestamp = std::get_if<core::target::TimePoint>(&deadline)) {
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
    const auto status = (scheduler_.get()->*method)(&call, input, &output);
    if (!status.ok()) {
      const auto code = extract_error_code(status);
      return OwnerFailure{code.value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    return output;
  }
  OwnerOutcome<wire::RuntimeBindAdapterResponse>
  bind_adapter(const wire::RuntimeBindAdapterRequest& input, std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::BindAdapter, input, delegation);
  }
  OwnerOutcome<wire::RuntimeUnbindAdapterResponse>
  unbind_adapter(const wire::RuntimeUnbindAdapterRequest& input,
                 std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::UnbindAdapter, input, delegation);
  }
  OwnerOutcome<wire::RuntimeCreateInstallationResponse>
  create_installation(const wire::RuntimeCreateInstallationRequest& input,
                      std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::CreateInstallation, input, delegation);
  }
  OwnerOutcome<wire::RuntimeCreateInstanceResponse>
  create_instance(const wire::RuntimeCreateInstanceRequest& input,
                  std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::CreateInstance, input, delegation);
  }
  OwnerOutcome<wire::RuntimeSetDesiredStateResponse>
  set_desired_state(const wire::RuntimeSetDesiredStateRequest& input,
                    std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::SetDesiredState, input, delegation);
  }
  OwnerOutcome<wire::RuntimeUpdateConfigResponse>
  update_config(const wire::RuntimeUpdateConfigRequest& input,
                std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::UpdateConfig, input, delegation);
  }
  OwnerOutcome<wire::RuntimeGetInstanceConfigResponse>
  get_instance_config(const wire::RuntimeGetInstanceConfigRequest& input,
                      std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::GetInstanceConfig, input, delegation);
  }
  OwnerOutcome<wire::RuntimeGetActionPolicyResponse>
  get_action_policy(const wire::RuntimeGetActionPolicyRequest& input,
                    std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::GetActionPolicy, input, delegation);
  }
  OwnerOutcome<wire::RuntimeUpdateActionPolicyResponse>
  update_action_policy(const wire::RuntimeUpdateActionPolicyRequest& input,
                       std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::UpdateActionPolicy, input, delegation);
  }
  OwnerOutcome<wire::SchedulerListTasksResponse>
  list_tasks(const wire::SchedulerListTasksRequest& input, std::string_view delegation) override {
    return unary_task(&wire::SchedulerService::Stub::ListTasks, input, delegation);
  }
  OwnerOutcome<wire::SchedulerCancelTaskResponse>
  cancel_task(const wire::SchedulerCancelTaskRequest& input, std::string_view delegation) override {
    return unary_task(&wire::SchedulerService::Stub::CancelTask, input, delegation);
  }
  OwnerOutcome<wire::SchedulerGetTaskResponse> get_task(const wire::SchedulerGetTaskRequest& input,
                                                        std::string_view delegation) override {
    return unary_task(&wire::SchedulerService::Stub::GetTask, input, delegation);
  }

  OwnerOutcome<wire::Operation> get(std::string_view owner_service,
                                    const wire::OperationRef& operation,
                                    std::string_view delegation) override {
    auto set_context = [&](wire::RequestContext* context,
                           const google::protobuf::Timestamp& deadline_from) {
      context->set_request_id(random_identifier());
      context->set_trace_id(random_identifier());
      *context->mutable_deadline() = deadline_from;
      context->set_delegation(std::string{delegation});
    };
    google::protobuf::Timestamp deadline;
    deadline.set_seconds(std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count() +
                         5);
    if (owner_service == "runtime") {
      if (!runtime_) {
        return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
      }
      wire::RuntimeGetOperationRequest input;
      set_context(input.mutable_context(), deadline);
      *input.mutable_operation() = operation;
      wire::RuntimeGetOperationResponse output;
      grpc::ClientContext call;
      call.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
      const auto status = runtime_->GetOperation(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      return output.operation();
    }
    if (owner_service == "scheduler") {
      if (!scheduler_) {
        return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
      }
      wire::SchedulerGetOperationRequest input;
      set_context(input.mutable_context(), deadline);
      *input.mutable_operation() = operation;
      wire::SchedulerGetOperationResponse output;
      grpc::ClientContext call;
      call.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
      const auto status = scheduler_->GetOperation(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      return output.operation();
    }
    if (owner_service == "model") {
      if (!model_) {
        return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
      }
      wire::ModelGetOperationRequest input;
      set_context(input.mutable_context(), deadline);
      *input.mutable_operation() = operation;
      wire::ModelGetOperationResponse output;
      grpc::ClientContext call;
      call.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
      const auto status = model_->GetOperation(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      return output.operation();
    }
    return OwnerFailure{wire::INVALID_ARGUMENT, false};
  }

  OwnerOutcome<wire::IdentityListPrincipalsResponse>
  list_principals(const wire::IdentityListPrincipalsRequest& request,
                  std::string_view delegation) override {
    if (!identity_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    wire::IdentityListPrincipalsRequest input = request;
    input.mutable_context()->set_delegation(std::string{delegation});
    wire::IdentityListPrincipalsResponse output;
    grpc::ClientContext call;
    const auto deadline = codec::from_wire(request.context().deadline());
    if (const auto* timestamp = std::get_if<core::target::TimePoint>(&deadline)) {
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
    const auto status = identity_->ListPrincipals(&call, input, &output);
    if (!status.ok()) {
      const auto code = extract_error_code(status);
      return OwnerFailure{code.value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    return output;
  }

  HealthObservation observe() override {
    HealthObservation observation;
    observation.identity_ready =
        identity_channel_ && identity_channel_->GetState(true) == GRPC_CHANNEL_READY;
    observation.runtime_configured = static_cast<bool>(runtime_channel_);
    observation.scheduler_configured = static_cast<bool>(scheduler_channel_);
    observation.model_configured = static_cast<bool>(model_channel_);
    observation.runtime_ready =
        runtime_channel_ && runtime_channel_->GetState(true) == GRPC_CHANNEL_READY;
    observation.scheduler_ready =
        scheduler_channel_ && scheduler_channel_->GetState(true) == GRPC_CHANNEL_READY;
    observation.model_ready =
        model_channel_ && model_channel_->GetState(true) == GRPC_CHANNEL_READY;
    observation.checked_at =
        core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
            std::chrono::system_clock::now().time_since_epoch())};
    return observation;
  }

  std::unique_ptr<OwnerGateway> gateway_;
  std::shared_ptr<grpc::Channel> identity_channel_;
  std::unique_ptr<wire::IdentityService::Stub> identity_;
  std::shared_ptr<grpc::Channel> runtime_channel_;
  std::unique_ptr<wire::RuntimeService::Stub> runtime_;
  std::shared_ptr<grpc::Channel> scheduler_channel_;
  std::unique_ptr<wire::SchedulerService::Stub> scheduler_;
  std::shared_ptr<grpc::Channel> model_channel_;
  std::unique_ptr<wire::ModelService::Stub> model_;
  std::shared_ptr<grpc::Channel> host_channel_;
  std::optional<std::array<unsigned char, SHA256_DIGEST_LENGTH>> fingerprint_;
};

OwnerClients::OwnerClients(const std::string& identity_endpoint,
                           const std::string& runtime_endpoint,
                           const std::string& scheduler_endpoint, const std::string& model_endpoint,
                           const std::string& host_endpoint, const core::platform::TlsMaterial& tls)
    : impl_(std::make_unique<Impl>(identity_endpoint, runtime_endpoint, scheduler_endpoint,
                                   model_endpoint, host_endpoint, tls)) {}
OwnerClients::~OwnerClients() = default;

std::optional<AuthHttpResponse> OwnerClients::handle(const AuthHttpRequest& request) {
  return impl_->handle(request);
}

} // namespace kiarabot::control::target
