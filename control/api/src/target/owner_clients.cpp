#include "kiarabot/control/target/owner_clients.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/host.grpc.pb.h"
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
    return "00000000-0000-4000-8000-000000000000";
  }
  // Owner RPC request/trace ids are canonical UUIDv4 strings at the Identity boundary; a raw
  // 32-hex token is rejected with INVALID_ARGUMENT before the delegation handler runs.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string value;
  value.reserve(36);
  for (std::size_t index = 0; index < sizeof(bytes); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      value.push_back('-');
    }
    value.push_back(kHex[bytes[index] >> 4U]);
    value.push_back(kHex[bytes[index] & 0x0fU]);
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
  case grpc::StatusCode::NOT_FOUND:
    return wire::NOT_FOUND;
  case grpc::StatusCode::ABORTED:
    return wire::CONFLICT;
  case grpc::StatusCode::RESOURCE_EXHAUSTED:
    return wire::RATE_LIMITED;
  case grpc::StatusCode::UNIMPLEMENTED:
    return wire::CAPABILITY_NOT_SUPPORTED;
  case grpc::StatusCode::INTERNAL:
    return wire::INTERNAL;
  case grpc::StatusCode::UNAVAILABLE:
    return wire::UPSTREAM_UNAVAILABLE;
  default:
    return std::nullopt;
  }
}
} // namespace

wire::ResourceScope audit_control_scope() {
  wire::ResourceScope scope;
  auto* resource = scope.add_resources();
  resource->set_kind(wire::RESOURCE_CATALOG);
  resource->set_id("control");
  return scope;
}
template <typename Record> AuditLogRow audit_row(const Record& record, std::string owner) {
  AuditLogRow row;
  row.entry_id = record.entry_id();
  row.owner = std::move(owner);
  row.actor = record.actor();
  row.resource_kind = record.resource().kind();
  row.resource_id = record.resource().id();
  row.request_id = record.request_id();
  row.trace_id = record.trace_id();
  row.method = record.method();
  row.rejection = wire::ErrorCode_Name(record.rejection());
  row.authorization_version = record.authorization_version();
  row.account_id = record.target_account_id();
  row.conversation_id = record.target_conversation_id();
  const auto occurred = codec::from_wire(record.occurred_at());
  if (const auto* value = std::get_if<core::target::TimePoint>(&occurred)) {
    row.occurred_at = *value;
  }
  return row;
}
template <typename Record> AuditLogRow log_row(const Record& record, std::string owner) {
  AuditLogRow row;
  row.entry_id = record.entry_id();
  row.owner = std::move(owner);
  row.actor = record.actor();
  row.resource_kind = record.resource().kind();
  row.resource_id = record.resource().id();
  row.request_id = record.request_id();
  row.trace_id = record.trace_id();
  row.method = record.method();
  row.severity = record.severity();
  row.component = record.component();
  row.message = record.message();
  const auto occurred = codec::from_wire(record.occurred_at());
  if (const auto* value = std::get_if<core::target::TimePoint>(&occurred)) {
    row.occurred_at = *value;
  }
  return row;
}
google::protobuf::Timestamp audit_deadline() {
  google::protobuf::Timestamp deadline;
  deadline.set_seconds(std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count() +
                       5);
  return deadline;
}

class OwnerClients::Impl final : private DelegationPort,
                                 private RuntimeActionsPort,
                                 private IdentityPrincipalsPort,
                                 private HealthPort,
                                 private RuntimeResourcesPort,
                                 private SchedulerTasksPort,
                                 private OperationPort,
                                 private ModelManagementPort,
                                 private AuditLogSourcePort,
                                 private HostManagementPort {
public:
  Impl(const std::string& identity_endpoint, const std::string& runtime_endpoint,
       const std::string& scheduler_endpoint, const std::string& model_endpoint,
       const std::string& host_endpoint, const core::platform::TlsMaterial& tls,
       SecretDeliveryApplication* secret_delivery, BootstrapRecipientsPort* bootstrap_recipients) {
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
      host_ = wire::HostManagementService::NewStub(host_channel_);
    }
    fingerprint_ = certificate_fingerprint(tls.certificate);
    gateway_ = std::make_unique<OwnerGateway>(
        static_cast<DelegationPort&>(*this), static_cast<RuntimeActionsPort&>(*this),
        static_cast<IdentityPrincipalsPort*>(this), static_cast<HealthPort*>(this),
        static_cast<RuntimeResourcesPort*>(this), static_cast<SchedulerTasksPort*>(this),
        static_cast<OperationPort*>(this), static_cast<ModelManagementPort*>(this),
        static_cast<AuditLogSourcePort*>(this), secret_delivery, bootstrap_recipients,
        static_cast<HostManagementPort*>(this));
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
  OwnerOutcome<wire::ResourceScope>
  catalog_scope(wire::Permission permission, const wire::ResourceRef& catalog,
                wire::ResourceKind child_kind, const std::optional<wire::ResourceScope>& selection,
                std::string_view access_token, core::target::TimePoint deadline) override {
    if (!identity_ || access_token.empty()) {
      return OwnerFailure{wire::UNAUTHENTICATED, false};
    }
    wire::IdentityValidateSessionRequest input;
    input.set_access_token(std::string{access_token});
    input.mutable_context()->set_request_id(random_identifier());
    input.mutable_context()->set_trace_id(random_identifier());
    const auto encoded = codec::to_wire(deadline);
    if (!std::holds_alternative<google::protobuf::Timestamp>(encoded)) {
      return OwnerFailure{wire::INVALID_ARGUMENT, false};
    }
    *input.mutable_context()->mutable_deadline() = std::get<google::protobuf::Timestamp>(encoded);
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            deadline.time_since_epoch())});
    wire::IdentityValidateSessionResponse current;
    const auto status = identity_->ValidateSession(&call, input, &current);
    if (!status.ok()) {
      return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    const auto checked = codec::from_wire(current.checked_at());
    const auto expires = codec::from_wire(current.session().expires_at());
    if (!current.has_principal() || !current.has_session() || !current.has_checked_at() ||
        current.principal().principal_id().empty() || current.authorization_version() == 0 ||
        current.principal().state() != wire::LIFECYCLE_ACTIVE || current.session().revoked() ||
        current.session().principal_id() != current.principal().principal_id() ||
        !std::holds_alternative<core::target::TimePoint>(checked) ||
        !std::holds_alternative<core::target::TimePoint>(expires) ||
        std::get<core::target::TimePoint>(expires) <= std::get<core::target::TimePoint>(checked)) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
    }
    std::set<std::string> children;
    std::set<std::string> assignment_ids;
    const auto neutral_permission = codec::from_wire(permission);
    if (!std::holds_alternative<core::target::Permission>(neutral_permission)) {
      return OwnerFailure{wire::INVALID_ARGUMENT, false};
    }
    for (const auto& assignment : current.assignments()) {
      if (assignment.principal_id() != current.principal().principal_id() ||
          assignment.assignment_id().empty() ||
          !assignment_ids.insert(assignment.assignment_id()).second || assignment.version() == 0 ||
          !assignment.has_scope() || assignment.scope().resources().empty()) {
        return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
      }
      bool includes = false;
      std::set<int> seen_permissions;
      for (const auto granted : assignment.permissions()) {
        if (!std::holds_alternative<core::target::Permission>(
                codec::from_wire(static_cast<wire::Permission>(granted))) ||
            !seen_permissions.insert(granted).second) {
          return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
        }
        includes = includes || granted == permission;
      }
      if (assignment.role() == wire::FIXED_ROLE_UNSPECIFIED && assignment.permissions().empty()) {
        return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
      }
      if (assignment.role() != wire::FIXED_ROLE_UNSPECIFIED) {
        if (!assignment.permissions().empty()) {
          return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
        }
        core::target::FixedRole role;
        switch (assignment.role()) {
        case wire::ROLE_ADMIN:
          role = core::target::FixedRole::Admin;
          break;
        case wire::ROLE_OPERATOR:
          role = core::target::FixedRole::Operator;
          break;
        case wire::ROLE_VIEWER:
          role = core::target::FixedRole::Viewer;
          break;
        default:
          return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
        }
        includes = core::target::default_role_allows(
            role, std::get<core::target::Permission>(neutral_permission));
      }
      std::set<std::pair<int, std::string>> seen_resources;
      for (const auto& resource : assignment.scope().resources()) {
        if (!std::holds_alternative<core::target::ResourceRef>(codec::from_wire(resource)) ||
            !seen_resources.emplace(resource.kind(), resource.id()).second) {
          return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, false};
        }
        if (includes && resource.kind() == child_kind) {
          children.insert(resource.id());
        }
      }
    }
    wire::ResourceScope result;
    *result.add_resources() = catalog;
    if (selection) {
      for (const auto& resource : selection->resources()) {
        if (!children.contains(resource.id())) {
          return OwnerFailure{wire::PERMISSION_DENIED, false};
        }
        *result.add_resources() = resource;
      }
    } else {
      if (children.size() > 127) {
        return OwnerFailure{wire::RATE_LIMITED, true};
      }
      for (const auto& id : children) {
        auto* resource = result.add_resources();
        resource->set_kind(child_kind);
        resource->set_id(id);
      }
    }
    return result;
  }
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
    // IdentityIssueDelegation is an idempotent owner RPC: the production Identity boundary rejects
    // an empty/invalid idempotency key with INVALID_ARGUMENT. Control issues a fresh one-off
    // delegation per owner call, so derive a unique key rather than forwarding caller headers.
    context->set_idempotency_key(random_identifier());
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
  OwnerOutcome<wire::RuntimeCreateAccountResponse>
  create_account(const wire::RuntimeCreateAccountRequest& input,
                 std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::CreateAccount, input, delegation);
  }
  OwnerOutcome<wire::RuntimeListAccountsResponse>
  list_accounts(const wire::RuntimeListAccountsRequest& input,
                std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ListAccounts, input, delegation);
  }
  OwnerOutcome<wire::RuntimeListInstallationsResponse>
  list_installations(const wire::RuntimeListInstallationsRequest& input,
                     std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ListInstallations, input, delegation);
  }
  OwnerOutcome<wire::RuntimeListInstancesResponse>
  list_instances(const wire::RuntimeListInstancesRequest& input,
                 std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ListInstances, input, delegation);
  }
  OwnerOutcome<wire::RuntimeListAdaptersResponse>
  list_adapters(const wire::RuntimeListAdaptersRequest& input,
                std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ListAdapters, input, delegation);
  }
  OwnerOutcome<wire::RuntimeListHostsResponse>
  list_hosts(const wire::RuntimeListHostsRequest& input, std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ListHosts, input, delegation);
  }
  OwnerOutcome<wire::RuntimeBindAdapterResponse>
  bind_adapter(const wire::RuntimeBindAdapterRequest& input, std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::BindAdapter, input, delegation);
  }
  OwnerOutcome<wire::RuntimeGetBootstrapDeliveryResponse>
  get_bootstrap_delivery(const wire::RuntimeGetBootstrapDeliveryRequest& input,
                         std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::GetBootstrapDelivery, input, delegation);
  }
  OwnerOutcome<wire::RuntimeReissueBootstrapResponse>
  reissue_adapter_bootstrap(const wire::RuntimeReissueAdapterBootstrapRequest& input,
                            std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ReissueAdapterBootstrap, input, delegation);
  }
  OwnerOutcome<wire::RuntimeReissueBootstrapResponse>
  reissue_plugin_bootstrap(const wire::RuntimeReissuePluginBootstrapRequest& input,
                           std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ReissuePluginBootstrap, input, delegation);
  }
  OwnerOutcome<wire::RuntimeGetBootstrapDeliveryResponse>
  report_adapter_bootstrap_delivery(const wire::RuntimeReportAdapterBootstrapDeliveryRequest& input,
                                    std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ReportAdapterBootstrapDelivery, input, delegation);
  }
  OwnerOutcome<wire::RuntimeGetBootstrapDeliveryResponse>
  report_plugin_bootstrap_delivery(const wire::RuntimeReportPluginBootstrapDeliveryRequest& input,
                                   std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::ReportPluginBootstrapDelivery, input, delegation);
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
  OwnerOutcome<wire::RuntimeBeginDeleteResponse>
  begin_delete(const wire::RuntimeBeginDeleteRequest& input, std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::BeginDelete, input, delegation);
  }
  OwnerOutcome<wire::RuntimeRestoreResourceResponse>
  restore_resource(const wire::RuntimeRestoreResourceRequest& input,
                   std::string_view delegation) override {
    return unary(&wire::RuntimeService::Stub::RestoreResource, input, delegation);
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
  OwnerOutcome<wire::SchedulerGetOccurrenceResponse>
  get_occurrence(const wire::SchedulerGetOccurrenceRequest& input,
                 std::string_view delegation) override {
    return unary_task(&wire::SchedulerService::Stub::GetOccurrence, input, delegation);
  }

  OwnerOutcome<AuditLogSourcePage> identity(AuditLogKind kind,
                                            const AuditLogFilter& filter) override {
    if (!identity_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    wire::RequestContext context;
    context.set_request_id(random_identifier());
    context.set_trace_id(random_identifier());
    *context.mutable_deadline() = audit_deadline();
    wire::ResourceScope scope = audit_control_scope();
    const auto fill_filter = [&](auto* target) {
      *target->mutable_scope() = scope;
      if (filter.occurred_from) {
        const auto encoded = codec::to_wire(*filter.occurred_from);
        if (const auto* value = std::get_if<google::protobuf::Timestamp>(&encoded)) {
          *target->mutable_occurred_from() = *value;
        }
      }
      if (filter.occurred_until) {
        const auto encoded = codec::to_wire(*filter.occurred_until);
        if (const auto* value = std::get_if<google::protobuf::Timestamp>(&encoded)) {
          *target->mutable_occurred_until() = *value;
        }
      }
      target->set_actor(filter.actor);
      if (filter.resource) {
        *target->mutable_resource() = *filter.resource;
      }
      target->mutable_page()->set_page_size(filter.page_size);
      target->mutable_page()->set_page_token(filter.page_token);
    };
    AuditLogSourcePage page;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
    if (kind == AuditLogKind::Audit) {
      wire::IdentityListAuditRequest input;
      *input.mutable_context() = context;
      fill_filter(input.mutable_filter());
      wire::IdentityListAuditResponse output;
      const auto status = identity_->ListAudit(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      for (const auto& record : output.entries()) {
        page.rows.push_back(audit_row(record, "identity"));
      }
      page.next_token = output.page().next_page_token();
    } else {
      wire::IdentityListLogsRequest input;
      *input.mutable_context() = context;
      fill_filter(input.mutable_filter());
      wire::IdentityListLogsResponse output;
      const auto status = identity_->ListLogs(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      for (const auto& record : output.entries()) {
        page.rows.push_back(log_row(record, "identity"));
      }
      page.next_token = output.page().next_page_token();
    }
    return page;
  }
  OwnerOutcome<AuditLogSourcePage> runtime(AuditLogKind kind, const AuditLogFilter& filter,
                                           std::string_view delegation) override {
    if (!runtime_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    wire::RequestContext context;
    context.set_request_id(random_identifier());
    context.set_trace_id(random_identifier());
    *context.mutable_deadline() = audit_deadline();
    context.set_delegation(std::string{delegation});
    wire::ResourceScope scope = audit_control_scope();
    const auto fill_filter = [&](auto* target) {
      *target->mutable_scope() = scope;
      if (filter.occurred_from) {
        const auto encoded = codec::to_wire(*filter.occurred_from);
        if (const auto* value = std::get_if<google::protobuf::Timestamp>(&encoded)) {
          *target->mutable_occurred_from() = *value;
        }
      }
      if (filter.occurred_until) {
        const auto encoded = codec::to_wire(*filter.occurred_until);
        if (const auto* value = std::get_if<google::protobuf::Timestamp>(&encoded)) {
          *target->mutable_occurred_until() = *value;
        }
      }
      target->set_actor(filter.actor);
      if (filter.resource) {
        *target->mutable_resource() = *filter.resource;
      }
      target->mutable_page()->set_page_size(filter.page_size);
      target->mutable_page()->set_page_token(filter.page_token);
    };
    AuditLogSourcePage page;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
    if (kind == AuditLogKind::Audit) {
      wire::RuntimeListAuditRequest input;
      *input.mutable_context() = context;
      fill_filter(input.mutable_filter());
      wire::RuntimeListAuditResponse output;
      const auto status = runtime_->ListAudit(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      for (const auto& record : output.entries()) {
        page.rows.push_back(audit_row(record, "runtime"));
      }
      page.next_token = output.page().next_page_token();
    } else {
      wire::RuntimeListLogsRequest input;
      *input.mutable_context() = context;
      fill_filter(input.mutable_filter());
      wire::RuntimeListLogsResponse output;
      const auto status = runtime_->ListLogs(&call, input, &output);
      if (!status.ok()) {
        return OwnerFailure{extract_error_code(status).value_or(wire::UPSTREAM_UNAVAILABLE), false};
      }
      for (const auto& record : output.entries()) {
        page.rows.push_back(log_row(record, "runtime"));
      }
      page.next_token = output.page().next_page_token();
    }
    return page;
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
    if (owner_service == "host") {
      if (!host_) {
        return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
      }
      wire::HostGetOperationRequest input;
      set_context(input.mutable_context(), deadline);
      *input.mutable_operation() = operation;
      wire::HostGetOperationResponse output;
      grpc::ClientContext call;
      call.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds{5});
      const auto status = host_->GetOperation(&call, input, &output);
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

  template <typename Request, typename Response, typename Method>
  OwnerOutcome<Response> model_call(Method method, Request request, std::string_view delegation) {
    if (!model_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    request.mutable_context()->set_delegation(std::string{delegation});
    Response output;
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
    const auto status = ((*model_).*method)(&call, request, &output);
    if (!status.ok()) {
      const auto code = extract_error_code(status);
      return OwnerFailure{code.value_or(wire::UPSTREAM_UNAVAILABLE), false};
    }
    return output;
  }

  OwnerOutcome<wire::ModelGetModelMetadataResponse>
  metadata(const wire::ModelGetModelMetadataRequest& request,
           std::string_view delegation) override {
    return model_call<wire::ModelGetModelMetadataRequest, wire::ModelGetModelMetadataResponse>(
        &wire::ModelService::Stub::GetModelMetadata, request, delegation);
  }
  OwnerOutcome<wire::ModelListModelMetadataResponse>
  list_metadata(const wire::ModelListModelMetadataRequest& request,
                std::string_view delegation) override {
    return model_call<wire::ModelListModelMetadataRequest, wire::ModelListModelMetadataResponse>(
        &wire::ModelService::Stub::ListModelMetadata, request, delegation);
  }

  OwnerOutcome<wire::ModelUpdateModelResponse> update(const wire::ModelUpdateModelRequest& request,
                                                      std::string_view delegation) override {
    return model_call<wire::ModelUpdateModelRequest, wire::ModelUpdateModelResponse>(
        &wire::ModelService::Stub::UpdateModel, request, delegation);
  }
  OwnerOutcome<wire::ModelDisableModelResponse>
  disable(const wire::ModelDisableModelRequest& request, std::string_view delegation) override {
    return model_call<wire::ModelDisableModelRequest, wire::ModelDisableModelResponse>(
        &wire::ModelService::Stub::DisableModel, request, delegation);
  }
  OwnerOutcome<wire::ModelEnableModelResponse> enable(const wire::ModelEnableModelRequest& request,
                                                      std::string_view delegation) override {
    return model_call<wire::ModelEnableModelRequest, wire::ModelEnableModelResponse>(
        &wire::ModelService::Stub::EnableModel, request, delegation);
  }
  OwnerOutcome<wire::ModelRotateProviderSecretResponse>
  rotate_secret(const wire::ModelRotateProviderSecretRequest& request,
                std::string_view delegation) override {
    return model_call<wire::ModelRotateProviderSecretRequest,
                      wire::ModelRotateProviderSecretResponse>(
        &wire::ModelService::Stub::RotateProviderSecret, request, delegation);
  }
  OwnerOutcome<wire::ModelGetModelConfigurationResponse>
  configuration(const wire::ModelGetModelConfigurationRequest& request,
                std::string_view delegation) override {
    return model_call<wire::ModelGetModelConfigurationRequest,
                      wire::ModelGetModelConfigurationResponse>(
        &wire::ModelService::Stub::GetModelConfiguration, request, delegation);
  }
  OwnerOutcome<wire::ModelGetCallStatusResponse>
  call_status(const wire::ModelGetCallStatusRequest& request,
              std::string_view delegation) override {
    return model_call<wire::ModelGetCallStatusRequest, wire::ModelGetCallStatusResponse>(
        &wire::ModelService::Stub::GetCallStatus, request, delegation);
  }
  OwnerOutcome<wire::ModelResolveModelCallResponse>
  resolve(const wire::ModelResolveModelCallRequest& request, std::string_view delegation) override {
    return model_call<wire::ModelResolveModelCallRequest, wire::ModelResolveModelCallResponse>(
        &wire::ModelService::Stub::ResolveModelCall, request, delegation);
  }
  OwnerOutcome<wire::ModelGetOperationResponse>
  operation(const wire::ModelGetOperationRequest& request, std::string_view delegation) override {
    return model_call<wire::ModelGetOperationRequest, wire::ModelGetOperationResponse>(
        &wire::ModelService::Stub::GetOperation, request, delegation);
  }

  OwnerOutcome<wire::HostGetMaintenanceResponse>
  get_maintenance(const wire::HostGetMaintenanceRequest& request,
                  std::string_view delegation) override {
    return host_call<wire::HostGetMaintenanceRequest, wire::HostGetMaintenanceResponse>(
        &wire::HostManagementService::Stub::GetMaintenance, request, delegation);
  }
  OwnerOutcome<wire::HostMaintenanceResponse>
  begin_maintenance(const wire::HostBeginMaintenanceRequest& request,
                    std::string_view delegation) override {
    return host_call<wire::HostBeginMaintenanceRequest, wire::HostMaintenanceResponse>(
        &wire::HostManagementService::Stub::BeginMaintenance, request, delegation);
  }
  OwnerOutcome<wire::HostMaintenanceResponse>
  resume_admission(const wire::HostResumeAdmissionRequest& request,
                   std::string_view delegation) override {
    return host_call<wire::HostResumeAdmissionRequest, wire::HostMaintenanceResponse>(
        &wire::HostManagementService::Stub::ResumeAdmission, request, delegation);
  }
  OwnerOutcome<wire::HostGetOperationResponse>
  get_operation(const wire::HostGetOperationRequest& request,
                std::string_view delegation) override {
    return host_call<wire::HostGetOperationRequest, wire::HostGetOperationResponse>(
        &wire::HostManagementService::Stub::GetOperation, request, delegation);
  }
  OwnerOutcome<wire::HostGetOperationResponse>
  cancel_operation(const wire::HostCancelOperationRequest& request,
                   std::string_view delegation) override {
    return host_call<wire::HostCancelOperationRequest, wire::HostGetOperationResponse>(
        &wire::HostManagementService::Stub::CancelOperation, request, delegation);
  }
  OwnerOutcome<wire::OperationChangesResponse>
  list_operation_changes(const wire::OperationChangesRequest& request,
                         std::string_view delegation) override {
    return host_call<wire::OperationChangesRequest, wire::OperationChangesResponse>(
        &wire::HostManagementService::Stub::ListOperationChanges, request, delegation);
  }

  template <typename Request, typename Response, typename Method>
  OwnerOutcome<Response> host_call(Method method, Request request, std::string_view delegation) {
    if (!host_) {
      return OwnerFailure{wire::UPSTREAM_UNAVAILABLE, true};
    }
    request.mutable_context()->set_delegation(std::string{delegation});
    Response output;
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
    const auto status = ((*host_).*method)(&call, request, &output);
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
  std::unique_ptr<wire::HostManagementService::Stub> host_;
  std::optional<std::array<unsigned char, SHA256_DIGEST_LENGTH>> fingerprint_;
};

OwnerClients::OwnerClients(const std::string& identity_endpoint,
                           const std::string& runtime_endpoint,
                           const std::string& scheduler_endpoint, const std::string& model_endpoint,
                           const std::string& host_endpoint, const core::platform::TlsMaterial& tls,
                           SecretDeliveryApplication* secret_delivery,
                           BootstrapRecipientsPort* bootstrap_recipients)
    : impl_(std::make_unique<Impl>(identity_endpoint, runtime_endpoint, scheduler_endpoint,
                                   model_endpoint, host_endpoint, tls, secret_delivery,
                                   bootstrap_recipients)) {}
OwnerClients::~OwnerClients() = default;

std::optional<AuthHttpResponse> OwnerClients::handle(const AuthHttpRequest& request) {
  return impl_->handle(request);
}

} // namespace kiarabot::control::target
