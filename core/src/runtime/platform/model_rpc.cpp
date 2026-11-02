#include "kiarabot/core/runtime/platform/model_rpc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/common.pb.h"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace codec = target_codec;
namespace t = kiarabot::core::target;

t::Failure transport_failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, {}, {}};
}
std::string correlation(std::string_view value) {
  return value.empty() ? std::string{"unknown"} : std::string{value};
}
grpc::Status invalid_status(std::string_view request_id = {}, std::string_view trace_id = {}) {
  v1::RpcFailure failure;
  const auto error = codec::make_error(t::ErrorCode::InvalidArgument, correlation(request_id),
                                       correlation(trace_id), false);
  if (const auto* value = std::get_if<v1::Error>(&error)) {
    *failure.mutable_error() = *value;
  }
  failure.mutable_context()->set_acceptance(v1::NOT_ACCEPTED);
  failure.mutable_context()->set_recovery(v1::CORRECT_INPUT);
  const auto status = codec::to_grpc_failure(failure);
  return std::holds_alternative<grpc::Status>(status)
             ? std::get<grpc::Status>(status)
             : grpc::Status(grpc::StatusCode::INTERNAL, "");
}
bool service_from_wire(std::string_view value, t::Service& out) {
  if (value == "identity") {
    out = t::Service::Identity;
  } else if (value == "runtime") {
    out = t::Service::Runtime;
  } else if (value == "host") {
    out = t::Service::Host;
  } else if (value == "scheduler") {
    out = t::Service::Scheduler;
  } else if (value == "model") {
    out = t::Service::Model;
  } else if (value == "control") {
    out = t::Service::Control;
  } else {
    return false;
  }
  return true;
}
v1::LifecycleState lifecycle_wire(t::LifecycleState value) {
  switch (value) {
  case t::LifecycleState::Active:
    return v1::LIFECYCLE_ACTIVE;
  case t::LifecycleState::Disabled:
    return v1::LIFECYCLE_DISABLED;
  case t::LifecycleState::Deleting:
    return v1::LIFECYCLE_DELETING;
  case t::LifecycleState::Deleted:
    return v1::LIFECYCLE_DELETED;
  case t::LifecycleState::Restoring:
    return v1::LIFECYCLE_RESTORING;
  case t::LifecycleState::Purged:
    return v1::LIFECYCLE_PURGED;
  }
  return v1::LIFECYCLE_UNSPECIFIED;
}
bool lifecycle_from_wire(v1::LifecycleState value, t::LifecycleState& out) {
  switch (value) {
  case v1::LIFECYCLE_ACTIVE:
    out = t::LifecycleState::Active;
    return true;
  case v1::LIFECYCLE_DISABLED:
    out = t::LifecycleState::Disabled;
    return true;
  default:
    return false;
  }
}
v1::OperationState operation_state_wire(t::OperationState value) {
  switch (value) {
  case t::OperationState::Accepted:
    return v1::OPERATION_ACCEPTED;
  case t::OperationState::Running:
    return v1::OPERATION_RUNNING;
  case t::OperationState::Blocked:
    return v1::OPERATION_BLOCKED;
  case t::OperationState::Succeeded:
    return v1::OPERATION_SUCCEEDED;
  case t::OperationState::Failed:
    return v1::OPERATION_FAILED;
  case t::OperationState::Cancelled:
    return v1::OPERATION_CANCELLED;
  case t::OperationState::Expired:
    return v1::OPERATION_EXPIRED;
  }
  return v1::OPERATION_STATE_UNSPECIFIED;
}
v1::ExecutionOutcome execution_wire(t::ExecutionOutcome value) {
  switch (value) {
  case t::ExecutionOutcome::NotStarted:
    return v1::EXECUTION_NOT_STARTED;
  case t::ExecutionOutcome::KnownSucceeded:
    return v1::EXECUTION_KNOWN_SUCCEEDED;
  case t::ExecutionOutcome::KnownFailed:
    return v1::EXECUTION_KNOWN_FAILED;
  case t::ExecutionOutcome::Unknown:
    return v1::EXECUTION_UNKNOWN;
  }
  return v1::EXECUTION_OUTCOME_UNSPECIFIED;
}
v1::ModelChatDisposition chat_disposition_wire(t::ModelChatDisposition value) {
  switch (value) {
  case t::ModelChatDisposition::FreshResult:
    return v1::FRESH_RESULT;
  case t::ModelChatDisposition::ReplayInProgress:
    return v1::REPLAY_IN_PROGRESS;
  case t::ModelChatDisposition::ReplayTerminal:
    return v1::REPLAY_TERMINAL;
  case t::ModelChatDisposition::Unspecified:
    break;
  }
  return v1::MODEL_CHAT_DISPOSITION_UNSPECIFIED;
}
v1::ModelCallDisposition call_disposition_wire(t::ModelCallDisposition value) {
  switch (value) {
  case t::ModelCallDisposition::NotExecuted:
    return v1::NOT_EXECUTED;
  case t::ModelCallDisposition::ExecutedNotBilled:
    return v1::EXECUTED_NOT_BILLED;
  case t::ModelCallDisposition::ExecutedBilled:
    return v1::EXECUTED_BILLED;
  case t::ModelCallDisposition::Abandoned:
    return v1::ABANDONED;
  case t::ModelCallDisposition::Unspecified:
    break;
  }
  return v1::MODEL_CALL_DISPOSITION_UNSPECIFIED;
}
bool call_disposition_from_wire(v1::ModelCallDisposition value, t::ModelCallDisposition& out) {
  switch (value) {
  case v1::NOT_EXECUTED:
    out = t::ModelCallDisposition::NotExecuted;
    return true;
  case v1::EXECUTED_NOT_BILLED:
    out = t::ModelCallDisposition::ExecutedNotBilled;
    return true;
  case v1::EXECUTED_BILLED:
    out = t::ModelCallDisposition::ExecutedBilled;
    return true;
  case v1::ABANDONED:
    out = t::ModelCallDisposition::Abandoned;
    return true;
  default:
    return false;
  }
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  if (!output) {
    return false;
  }
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
bool context_from_wire(const v1::RequestContext& wire, bool idempotent,
                       t::ModelCallContext& output) {
  if (wire.request_id().empty() || wire.trace_id().empty() || !wire.has_deadline() ||
      (idempotent && wire.idempotency_key().empty())) {
    return false;
  }
  const auto deadline = codec::from_wire(wire.deadline());
  if (!std::holds_alternative<t::TimePoint>(deadline)) {
    return false;
  }
  output.request_id = wire.request_id();
  output.trace_id = wire.trace_id();
  output.deadline = std::get<t::TimePoint>(deadline);
  output.idempotency_key = wire.idempotency_key();
  return true;
}
void metadata_wire(const t::ModelMetadataView& view, v1::ModelMetadata* output) {
  output->set_model_id(view.config.model_id.value);
  output->set_alias(view.config.alias);
  output->set_config_version(view.config.version.value);
  output->set_state(lifecycle_wire(view.config.state));
  output->set_provider_kind(std::string{t::model_provider_kind_name(view.config.provider_kind)});
}
void configuration_wire(const t::ModelConfiguration& config, v1::ModelConfiguration* output) {
  output->set_model_id(config.model_id.value);
  output->set_alias(config.alias);
  output->set_provider_kind(std::string{t::model_provider_kind_name(config.provider_kind)});
  output->set_endpoint(config.endpoint);
  output->set_provider_model(config.provider_model);
  output->set_timeout_ms(config.timeout_ms);
  output->set_version(config.version.value);
  output->set_state(lifecycle_wire(config.state));
  output->set_max_concurrency(config.max_concurrency);
}
bool reason_wire(const std::optional<t::ErrorCode>& reason, v1::Error* output,
                 std::string_view request_id, std::string_view trace_id) {
  if (!reason) {
    return true;
  }
  const auto error = codec::make_error(*reason, request_id, trace_id, false);
  if (const auto* wire = std::get_if<v1::Error>(&error)) {
    *output = *wire;
    return true;
  }
  return false;
}
void call_wire(const t::ModelIntent& call, v1::ModelCallRecord* output) {
  output->set_logical_call_id(call.logical_call_id);
  output->set_owner_instance_id(call.owner.value);
  output->set_model_id(call.model.value);
  output->set_config_version(call.config_version.value);
  output->set_state(operation_state_wire(t::model_operation_state(call.state)));
  output->set_execution_outcome(execution_wire(t::model_execution_outcome(call.state)));
  output->set_input_tokens(call.input_tokens);
  output->set_output_tokens(call.output_tokens);
  if (!timestamp(call.observed_at, output->mutable_observed_at())) {
    output->clear_observed_at();
  }
  if (const auto error = codec::make_error(call.error.value_or(t::ErrorCode::Internal),
                                           call.logical_call_id, {}, false);
      call.error) {
    if (const auto* wire = std::get_if<v1::Error>(&error)) {
      *output->mutable_reason() = *wire;
    }
  }
  output->set_secret_version(call.secret_version.value);
  output->set_usage_known(call.usage_known);
  output->set_may_have_billed(call.may_have_billed);
  output->set_provider_request_id(call.provider_request_id);
  output->set_latency_ms(call.latency_ms);
  output->set_disposition(call_disposition_wire(call.disposition));
}
grpc::Status error_status(const t::Failure& failure, std::string_view request_id,
                          std::string_view trace_id) {
  v1::RpcFailure result;
  const bool retryable = failure.recovery == t::Recovery::Backoff ||
                         failure.recovery == t::Recovery::QuerySameIdentity;
  const auto error =
      codec::make_error(failure.code, correlation(request_id), correlation(trace_id), retryable);
  if (const auto* stable = std::get_if<v1::Error>(&error)) {
    *result.mutable_error() = *stable;
  }
  auto* context = result.mutable_context();
  switch (failure.acceptance) {
  case t::Acceptance::NotAccepted:
    context->set_acceptance(v1::NOT_ACCEPTED);
    break;
  case t::Acceptance::Accepted:
    context->set_acceptance(v1::DURABLY_ACCEPTED);
    break;
  case t::Acceptance::Unknown:
    context->set_acceptance(v1::ACCEPTANCE_UNKNOWN);
    break;
  }
  switch (failure.recovery) {
  case t::Recovery::CorrectInput:
    context->set_recovery(v1::CORRECT_INPUT);
    break;
  case t::Recovery::Authenticate:
    context->set_recovery(v1::REAUTHENTICATE);
    break;
  case t::Recovery::ReadCurrent:
    context->set_recovery(v1::RESYNC_SNAPSHOT);
    break;
  case t::Recovery::QuerySameIdentity:
    context->set_recovery(v1::QUERY_SAME_INTENT);
    break;
  case t::Recovery::Backoff:
    context->set_recovery(v1::RETRY_SAME_ID);
    break;
  case t::Recovery::Diagnose:
    context->set_recovery(v1::DO_NOT_RETRY);
    break;
  }
  if (failure.retry_after) {
    context->set_retry_after_ms(
        static_cast<std::uint32_t>(std::min<std::int64_t>(failure.retry_after->count(), 60000)));
  }
  if (failure.operation) {
    auto* operation = context->mutable_operation();
    operation->set_owner_service(std::string{service_name(failure.operation->owner)});
    operation->set_operation_id(failure.operation->id.value);
    const auto resource = codec::to_wire(failure.operation->resource);
    if (const auto* wire = std::get_if<v1::ResourceRef>(&resource)) {
      *operation->mutable_resource() = *wire;
    }
  }
  const auto status = codec::to_grpc_failure(result);
  return std::holds_alternative<grpc::Status>(status)
             ? std::get<grpc::Status>(status)
             : grpc::Status(grpc::StatusCode::INTERNAL, "");
}
bool resource_ref_from_wire(const v1::ResourceRef& wire, t::ResourceRef& output) {
  const auto kind = codec::from_wire(wire.kind());
  if (!std::holds_alternative<t::ResourceKind>(kind)) {
    return false;
  }
  output = {std::get<t::ResourceKind>(kind), {wire.id()}};
  return true;
}

class ModelRpcImpl final : public v1::ModelService::Service {
public:
  ModelRpcImpl(t::ModelGatewayApplication& application, ModelPeerAuthenticator authenticate)
      : application_(application), authenticate_(std::move(authenticate)) {}

  grpc::Status Chat(grpc::ServerContext* context, const v1::ModelGatewayChatRequest* request,
                    v1::ModelGatewayChatResponse* response) override {
    t::ModelCaller caller;
    auto status =
        caller_for(*context, request->context(), "/kiarabot.v1.ModelService/Chat", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), false, call_context) ||
        request->model_id().empty()) {
      return invalid_status(request->has_context() ? request->context().request_id() : "",
                            request->has_context() ? request->context().trace_id() : "");
    }
    std::vector<t::ModelMessage> messages;
    messages.reserve(static_cast<std::size_t>(request->messages_size()));
    for (const auto& message : request->messages()) {
      messages.push_back({message.role(), message.text()});
    }
    auto reply = application_.chat({caller, call_context, request->logical_call_id(),
                                    request->model_id(), std::move(messages)});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    const auto& value = std::get<t::ModelChatReply>(reply);
    response->set_disposition(chat_disposition_wire(value.disposition));
    if (value.result) {
      auto* result = response->mutable_result();
      result->set_text(*value.result);
      result->set_input_tokens(value.call.input_tokens);
      result->set_output_tokens(value.call.output_tokens);
    }
    call_wire(value.call, response->mutable_call());
    return grpc::Status::OK;
  }

  grpc::Status GetCallStatus(grpc::ServerContext* context,
                             const v1::ModelGetCallStatusRequest* request,
                             v1::ModelGetCallStatusResponse* response) override {
    t::ModelCaller caller;
    auto status =
        caller_for(*context, request->context(), "/kiarabot.v1.ModelService/GetCallStatus", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), false, call_context)) {
      return invalid_status();
    }
    std::optional<t::ResourceId> scope;
    if (request->has_scope()) {
      if (request->scope().resources_size() != 1) {
        return invalid_status(call_context.request_id, call_context.trace_id);
      }
      t::ResourceRef resource;
      if (!resource_ref_from_wire(request->scope().resources(0), resource) ||
          resource.kind != t::ResourceKind::Model) {
        return invalid_status(call_context.request_id, call_context.trace_id);
      }
      scope = resource.id;
    }
    auto reply =
        application_.get_call_status({caller, call_context, request->logical_call_id(), scope});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    call_wire(std::get<t::ModelIntent>(reply), response->mutable_call());
    return grpc::Status::OK;
  }

  grpc::Status GetModelMetadata(grpc::ServerContext* context,
                                const v1::ModelGetModelMetadataRequest* request,
                                v1::ModelGetModelMetadataResponse* response) override {
    t::ModelCaller caller;
    auto status = caller_for(*context, request->context(),
                             "/kiarabot.v1.ModelService/GetModelMetadata", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), false, call_context)) {
      return invalid_status();
    }
    auto reply = application_.get_model_metadata({caller, call_context, {request->model_id()}});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    metadata_wire(std::get<t::ModelMetadataView>(reply), response->mutable_model());
    return grpc::Status::OK;
  }

  grpc::Status ListModelMetadata(grpc::ServerContext* context,
                                 const v1::ModelListModelMetadataRequest* request,
                                 v1::ModelListModelMetadataResponse* response) override {
    t::ModelCaller caller;
    auto status = caller_for(*context, request->context(),
                             "/kiarabot.v1.ModelService/ListModelMetadata", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), false, call_context) ||
        !request->has_scope()) {
      return invalid_status();
    }
    std::vector<t::ResourceId> scope;
    for (const auto& resource_wire : request->scope().resources()) {
      t::ResourceRef resource;
      if (!resource_ref_from_wire(resource_wire, resource) ||
          resource.kind != t::ResourceKind::Model) {
        return invalid_status(call_context.request_id, call_context.trace_id);
      }
      scope.push_back(resource.id);
    }
    auto reply = application_.list_model_metadata({caller, call_context, std::move(scope)});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    for (const auto& metadata : std::get<t::ModelListMetadataReply>(reply).models) {
      metadata_wire(metadata, response->add_models());
    }
    return grpc::Status::OK;
  }

  grpc::Status UpdateModel(grpc::ServerContext* context, const v1::ModelUpdateModelRequest* request,
                           v1::ModelUpdateModelResponse* response) override {
    t::ModelCaller caller;
    auto status =
        caller_for(*context, request->context(), "/kiarabot.v1.ModelService/UpdateModel", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), true, call_context) ||
        !request->has_config()) {
      return invalid_status();
    }
    t::ModelConfiguration config;
    config.model_id = {request->config().model_id()};
    config.alias = request->config().alias();
    const auto kind = t::model_provider_kind(request->config().provider_kind());
    if (!kind) {
      return invalid_status(call_context.request_id, call_context.trace_id);
    }
    config.provider_kind = *kind;
    config.endpoint = request->config().endpoint();
    config.provider_model = request->config().provider_model();
    config.timeout_ms = request->config().timeout_ms() == 0 ? t::kModelDefaultTimeoutMs
                                                            : request->config().timeout_ms();
    config.max_concurrency = request->config().max_concurrency() == 0
                                 ? t::kModelDefaultConcurrency
                                 : request->config().max_concurrency();
    config.state = t::LifecycleState::Disabled;
    std::optional<t::ResourceVersion> expected;
    if (request->has_expected_version()) {
      expected = t::ResourceVersion{request->expected_version()};
    }
    auto reply = application_.update_model({caller, call_context, std::move(config), expected});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    const auto& value = std::get<t::ModelMutationReply>(reply);
    if (value.metadata) {
      metadata_wire(*value.metadata, response->mutable_model());
    }
    return grpc::Status::OK;
  }

  grpc::Status RotateProviderSecret(grpc::ServerContext* context,
                                    const v1::ModelRotateProviderSecretRequest* request,
                                    v1::ModelRotateProviderSecretResponse* response) override {
    t::ModelCaller caller;
    auto status = caller_for(*context, request->context(),
                             "/kiarabot.v1.ModelService/RotateProviderSecret", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), true, call_context)) {
      return invalid_status();
    }
    std::vector<std::byte> secret(request->new_secret().size());
    for (std::size_t index = 0; index < secret.size(); ++index) {
      secret[index] =
          static_cast<std::byte>(static_cast<unsigned char>(request->new_secret()[index]));
    }
    std::optional<t::ResourceVersion> expected;
    if (request->has_expected_version()) {
      expected = t::ResourceVersion{request->expected_version()};
    }
    auto reply = application_.rotate_provider_secret(
        {caller, call_context, {request->model_id()}, std::move(secret), expected});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    const auto& value = std::get<t::ModelMutationReply>(reply);
    if (value.version) {
      response->set_version(value.version->value);
    }
    return grpc::Status::OK;
  }

  grpc::Status DisableModel(grpc::ServerContext* context,
                            const v1::ModelDisableModelRequest* request,
                            v1::ModelDisableModelResponse* response) override {
    return lifecycle(*context, *request, false, "/kiarabot.v1.ModelService/DisableModel",
                     response->mutable_model());
  }
  grpc::Status EnableModel(grpc::ServerContext* context, const v1::ModelEnableModelRequest* request,
                           v1::ModelEnableModelResponse* response) override {
    return lifecycle(*context, *request, true, "/kiarabot.v1.ModelService/EnableModel",
                     response->mutable_model());
  }

  grpc::Status GetModelConfiguration(grpc::ServerContext* context,
                                     const v1::ModelGetModelConfigurationRequest* request,
                                     v1::ModelGetModelConfigurationResponse* response) override {
    t::ModelCaller caller;
    auto status = caller_for(*context, request->context(),
                             "/kiarabot.v1.ModelService/GetModelConfiguration", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), false, call_context)) {
      return invalid_status();
    }
    auto reply = application_.get_configuration({caller, call_context, {request->model_id()}});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    const auto& value = std::get<t::ModelConfigurationView>(reply);
    configuration_wire(value.config, response->mutable_config());
    response->set_secret_configured(value.secret_configured);
    response->set_secret_version(value.secret_version.value);
    timestamp(value.observed_at, response->mutable_observed_at());
    return grpc::Status::OK;
  }

  grpc::Status ResolveModelCall(grpc::ServerContext* context,
                                const v1::ModelResolveModelCallRequest* request,
                                v1::ModelResolveModelCallResponse* response) override {
    t::ModelCaller caller;
    auto status = caller_for(*context, request->context(),
                             "/kiarabot.v1.ModelService/ResolveModelCall", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    t::ModelCallDisposition disposition;
    if (!request->has_context() || !context_from_wire(request->context(), true, call_context) ||
        !call_disposition_from_wire(request->disposition(), disposition)) {
      return invalid_status();
    }
    std::optional<t::ResourceVersion> expected;
    if (request->has_expected_version()) {
      expected = t::ResourceVersion{request->expected_version()};
    }
    auto reply = application_.resolve_call(
        {caller, call_context, request->logical_call_id(), expected, disposition});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    call_wire(std::get<t::ModelIntent>(reply), response->mutable_call());
    return grpc::Status::OK;
  }

  grpc::Status GetOperation(grpc::ServerContext* context,
                            const v1::ModelGetOperationRequest* request,
                            v1::ModelGetOperationResponse* response) override {
    t::ModelCaller caller;
    auto status =
        caller_for(*context, request->context(), "/kiarabot.v1.ModelService/GetOperation", caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request->has_context() || !context_from_wire(request->context(), false, call_context) ||
        !request->has_operation()) {
      return invalid_status();
    }
    t::OperationRef reference;
    if (!service_from_wire(request->operation().owner_service(), reference.owner) ||
        request->operation().operation_id().empty() || !request->operation().has_resource()) {
      return invalid_status(call_context.request_id, call_context.trace_id);
    }
    reference.id = {request->operation().operation_id()};
    if (!resource_ref_from_wire(request->operation().resource(), reference.resource)) {
      return invalid_status(call_context.request_id, call_context.trace_id);
    }
    auto reply = application_.get_operation({caller, call_context, reference});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    const auto& operation = std::get<t::Operation>(reply);
    auto* wire = response->mutable_operation();
    auto* ref = wire->mutable_ref();
    ref->set_owner_service(std::string{service_name(operation.ref.owner)});
    ref->set_operation_id(operation.ref.id.value);
    const auto resource = codec::to_wire(operation.ref.resource);
    if (const auto* value = std::get_if<v1::ResourceRef>(&resource)) {
      *ref->mutable_resource() = *value;
    }
    wire->set_version(operation.version.value);
    wire->set_state(operation_state_wire(operation.state));
    wire->set_execution_outcome(execution_wire(operation.execution_outcome));
    if (operation.blocked_reason) {
      const auto error = codec::make_error(*operation.blocked_reason, call_context.request_id,
                                           call_context.trace_id, false);
      if (const auto* value = std::get_if<v1::Error>(&error)) {
        *wire->mutable_reason() = *value;
      }
    }
    if (operation.next_attempt) {
      timestamp(*operation.next_attempt, wire->mutable_next_attempt_at());
    }
    return grpc::Status::OK;
  }

private:
  grpc::Status caller_for(grpc::ServerContext& context, const v1::RequestContext& request,
                          std::string_view method, t::ModelCaller& caller) {
    if (!authenticate_) {
      return error_status(
          transport_failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate), {}, {});
    }
    if (!request.request_id().empty()) {
      // Bound the method string to the exact compiled RPC; a caller cannot influence which
      // permission Identity evaluates for this entry point.
    }
    auto authenticated = authenticate_(context, request);
    if (const auto* failure = std::get_if<t::Failure>(&authenticated)) {
      return error_status(*failure, {}, {});
    }
    caller = std::move(std::get<t::ModelCaller>(authenticated));
    caller.method = std::string{method};
    return grpc::Status::OK;
  }
  grpc::Status lifecycle(grpc::ServerContext& context, const auto& request, bool enable,
                         std::string_view method, v1::ModelMetadata* model) {
    t::ModelCaller caller;
    auto status = caller_for(context, request.context(), method, caller);
    if (!status.ok()) {
      return status;
    }
    t::ModelCallContext call_context;
    if (!request.has_context() || !context_from_wire(request.context(), true, call_context) ||
        !request.has_expected_version()) {
      return invalid_status();
    }
    auto reply = application_.set_model_lifecycle({caller,
                                                   call_context,
                                                   {request.model_id()},
                                                   t::ResourceVersion{request.expected_version()},
                                                   enable});
    if (const auto* failure = std::get_if<t::Failure>(&reply)) {
      return error_status(*failure, call_context.request_id, call_context.trace_id);
    }
    const auto& value = std::get<t::ModelMutationReply>(reply);
    if (value.metadata) {
      metadata_wire(*value.metadata, model);
    }
    return grpc::Status::OK;
  }
  t::ModelGatewayApplication& application_;
  ModelPeerAuthenticator authenticate_;
};

} // namespace

target::Outcome<std::unique_ptr<v1::ModelService::Service>>
make_model_rpc(target::ModelGatewayApplication& application, ModelPeerAuthenticator authenticate) {
  try {
    return std::unique_ptr<v1::ModelService::Service>(
        std::make_unique<ModelRpcImpl>(application, std::move(authenticate)));
  } catch (...) {
    return transport_failure(t::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
