#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "kiarabot/v1/model.grpc.pb.h"

namespace {
namespace wire = kiarabot::v1;
constexpr char kModel[] = "11111111-1111-4111-8111-111111111111";
constexpr std::int64_t kObserved = 1700000000;

std::string read(const char* path) {
  std::ifstream input(path);
  std::ostringstream result;
  result << input.rdbuf();
  return result.str();
}
grpc::Status error(wire::ErrorCode code, grpc::StatusCode grpc_code,
                   const wire::RequestContext& context) {
  wire::RpcFailure failure;
  failure.mutable_context()->set_acceptance(wire::NOT_ACCEPTED);
  failure.mutable_context()->set_recovery(wire::DO_NOT_RETRY);
  failure.mutable_error()->set_code(code);
  failure.mutable_error()->set_request_id(context.request_id());
  failure.mutable_error()->set_trace_id(context.trace_id());
  failure.mutable_error()->set_message("never-print-upstream-secret");
  return {grpc_code, "never-print-upstream-secret", failure.SerializeAsString()};
}
bool delegated(const wire::RequestContext& context) {
  return !context.delegation().empty() && !context.request_id().empty() &&
         !context.trace_id().empty() && context.has_deadline();
}
void metadata(wire::ModelMetadata* output, wire::LifecycleState state = wire::LIFECYCLE_ACTIVE) {
  output->set_model_id(kModel);
  output->set_alias("default");
  output->set_config_version(3);
  output->set_state(state);
  output->set_provider_kind("openai_chat_completions");
}
grpc::Status reject(const wire::RequestContext& context) {
  return error(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED, context);
}
class ModelFixture final : public wire::ModelService::Service {
  grpc::Status GetModelMetadata(grpc::ServerContext*,
                                const wire::ModelGetModelMetadataRequest* input,
                                wire::ModelGetModelMetadataResponse* output) override {
    if (!delegated(input->context())) {
      return reject(input->context());
    }
    if (input->model_id() != kModel) {
      return error(wire::NOT_FOUND, grpc::StatusCode::NOT_FOUND, input->context());
    }
    metadata(output->mutable_model());
    return grpc::Status::OK;
  }
  grpc::Status ListModelMetadata(grpc::ServerContext*,
                                 const wire::ModelListModelMetadataRequest* input,
                                 wire::ModelListModelMetadataResponse* output) override {
    if (!delegated(input->context()) || input->scope().resources_size() == 0) {
      return reject(input->context());
    }
    metadata(output->add_models());
    if (input->has_page()) {
      output->mutable_page()->set_page_size(input->page().page_size());
    }
    return grpc::Status::OK;
  }
  grpc::Status UpdateModel(grpc::ServerContext*, const wire::ModelUpdateModelRequest* input,
                           wire::ModelUpdateModelResponse* output) override {
    if (!delegated(input->context()) || !input->has_config() ||
        input->config().model_id() != kModel ||
        input->context().idempotency_key() != "model-update-key-1") {
      return reject(input->context());
    }
    metadata(output->mutable_model());
    return grpc::Status::OK;
  }
  grpc::Status DisableModel(grpc::ServerContext*, const wire::ModelDisableModelRequest* input,
                            wire::ModelDisableModelResponse* output) override {
    if (!delegated(input->context()) || input->model_id() != kModel ||
        !input->has_expected_version()) {
      return reject(input->context());
    }
    metadata(output->mutable_model(), wire::LIFECYCLE_DISABLED);
    return grpc::Status::OK;
  }
  grpc::Status EnableModel(grpc::ServerContext*, const wire::ModelEnableModelRequest* input,
                           wire::ModelEnableModelResponse* output) override {
    if (!delegated(input->context()) || input->model_id() != kModel ||
        !input->has_expected_version()) {
      return reject(input->context());
    }
    metadata(output->mutable_model());
    return grpc::Status::OK;
  }
  grpc::Status RotateProviderSecret(grpc::ServerContext*,
                                    const wire::ModelRotateProviderSecretRequest* input,
                                    wire::ModelRotateProviderSecretResponse* output) override {
    if (!delegated(input->context()) || input->model_id() != kModel ||
        input->new_secret().empty() || input->context().idempotency_key() != "model-secret-key-1") {
      return reject(input->context());
    }
    output->set_version(9);
    return grpc::Status::OK;
  }
  grpc::Status GetModelConfiguration(grpc::ServerContext*,
                                     const wire::ModelGetModelConfigurationRequest* input,
                                     wire::ModelGetModelConfigurationResponse* output) override {
    if (!delegated(input->context()) || input->model_id() != kModel) {
      return reject(input->context());
    }
    auto* config = output->mutable_config();
    config->set_model_id(kModel);
    config->set_alias("default");
    config->set_provider_kind("openai_chat_completions");
    config->set_endpoint("https://api.example.invalid/v1");
    config->set_provider_model("fixture-model");
    config->set_timeout_ms(30000);
    config->set_version(3);
    config->set_state(wire::LIFECYCLE_ACTIVE);
    config->set_max_concurrency(4);
    output->set_secret_configured(true);
    output->set_secret_version(9);
    output->mutable_observed_at()->set_seconds(kObserved);
    return grpc::Status::OK;
  }
  grpc::Status GetCallStatus(grpc::ServerContext*, const wire::ModelGetCallStatusRequest* input,
                             wire::ModelGetCallStatusResponse* output) override {
    if (!delegated(input->context()) || input->logical_call_id().empty()) {
      return reject(input->context());
    }
    auto* call = output->mutable_call();
    call->set_logical_call_id(input->logical_call_id());
    call->set_owner_instance_id("22222222-2222-4222-8222-222222222222");
    call->set_model_id(kModel);
    call->set_config_version(3);
    call->set_state(wire::OPERATION_BLOCKED);
    call->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    call->set_usage_known(false);
    call->set_may_have_billed(true);
    call->set_disposition(wire::MODEL_CALL_DISPOSITION_UNSPECIFIED);
    call->mutable_observed_at()->set_seconds(kObserved);
    return grpc::Status::OK;
  }
  grpc::Status ResolveModelCall(grpc::ServerContext*,
                                const wire::ModelResolveModelCallRequest* input,
                                wire::ModelResolveModelCallResponse* output) override {
    if (!delegated(input->context()) || input->logical_call_id().empty() ||
        input->disposition() == wire::MODEL_CALL_DISPOSITION_UNSPECIFIED) {
      return reject(input->context());
    }
    auto* call = output->mutable_call();
    call->set_logical_call_id(input->logical_call_id());
    call->set_model_id(kModel);
    call->set_config_version(3);
    call->set_state(wire::OPERATION_SUCCEEDED);
    call->set_execution_outcome(wire::EXECUTION_UNKNOWN);
    call->set_disposition(input->disposition());
    call->mutable_observed_at()->set_seconds(kObserved);
    return grpc::Status::OK;
  }
  grpc::Status GetOperation(grpc::ServerContext*, const wire::ModelGetOperationRequest* input,
                            wire::ModelGetOperationResponse* output) override {
    if (!delegated(input->context()) || !input->has_operation()) {
      return reject(input->context());
    }
    auto* operation = output->mutable_operation();
    *operation->mutable_ref() = input->operation();
    operation->set_version(1);
    operation->set_state(wire::OPERATION_SUCCEEDED);
    operation->set_execution_outcome(wire::EXECUTION_KNOWN_SUCCEEDED);
    operation->mutable_observed_at()->set_seconds(kObserved);
    return grpc::Status::OK;
  }
};
} // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    return 2;
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = read(argv[1]);
  options.pem_key_cert_pairs.push_back({read(argv[3]), read(argv[2])});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  ModelFixture fixture;
  builder.RegisterService(&fixture);
  auto server = builder.BuildAndStart();
  if (!server) {
    return 1;
  }
  std::cout << port << '\n' << std::flush;
  server->Wait();
}
