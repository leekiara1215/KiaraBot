#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/core/ports/target/model_ports.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// Request context repeated on every Model command/query. RPC validation owns wire/mTLS mapping;
// the application only trusts these already-validated values plus the authorization decision.
struct ModelCallContext {
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  std::string idempotency_key;
};

struct ModelMetadataView {
  ModelConfiguration config;
  ResourceVersion secret_version{0};
};

struct ModelConfigurationView {
  ModelConfiguration config;
  ResourceVersion secret_version{0};
  bool secret_configured = false;
  TimePoint observed_at;
};

struct ModelChatCommand {
  ModelCaller caller;
  ModelCallContext context;
  std::string logical_call_id;
  std::string model;
  std::vector<ModelMessage> messages;
};

struct ModelChatReply {
  ModelChatDisposition disposition = ModelChatDisposition::Unspecified;
  ModelIntent call;
  // Present only for a fresh provider success; never persisted and never replayed.
  std::optional<std::string> result;
};

struct ModelGetCallStatusQuery {
  ModelCaller caller;
  ModelCallContext context;
  std::string logical_call_id;
  std::optional<ResourceId> model_scope;
};

struct ModelMetadataQuery {
  ModelCaller caller;
  ModelCallContext context;
  ResourceId model_id;
};

struct ModelListMetadataQuery {
  ModelCaller caller;
  ModelCallContext context;
  std::vector<ResourceId> scope;
};

struct ModelListMetadataReply {
  std::vector<ModelMetadataView> models;
};

struct ModelUpdateCommand {
  ModelCaller caller;
  ModelCallContext context;
  ModelConfiguration config;
  std::optional<ResourceVersion> expected_version;
};

struct ModelRotateSecretCommand {
  ModelCaller caller;
  ModelCallContext context;
  ResourceId model_id;
  std::vector<std::byte> secret;
  std::optional<ResourceVersion> expected_version;
};

struct ModelLifecycleCommand {
  ModelCaller caller;
  ModelCallContext context;
  ResourceId model_id;
  std::optional<ResourceVersion> expected_version;
  bool enable = false;
};

struct ModelConfigurationQuery {
  ModelCaller caller;
  ModelCallContext context;
  ResourceId model_id;
};

struct ModelResolveCallCommand {
  ModelCaller caller;
  ModelCallContext context;
  std::string logical_call_id;
  std::optional<ResourceVersion> expected_version;
  ModelCallDisposition disposition = ModelCallDisposition::Unspecified;
};

struct ModelOperationQuery {
  ModelCaller caller;
  ModelCallContext context;
  OperationRef reference;
};

// Management replies carry the durable operation reference; a replay of the same idempotency key
// returns the original resource version and creates no second operation.
struct ModelMutationReply {
  std::optional<ModelMetadataView> metadata;
  std::optional<ResourceVersion> version;
  std::optional<OperationRef> operation;
};

struct ModelApplicationDependencies {
  ModelUnitOfWorkFactory& transactions;
  ModelProviderPort& provider;
  ModelSecretCryptoPort& crypto;
  ModelDigestPort& digest;
  ModelIdPort& ids;
  ModelAuthorizationPort& authorization;
  std::uint64_t root_key_version = 0;
};

// Pure P11 Model application. No SQL, protobuf, HTTP or logging dependency; all side effects are
// the injected ports plus the Model-owned typed UoW.
class ModelGatewayApplication {
public:
  explicit ModelGatewayApplication(ModelApplicationDependencies dependencies);

  Outcome<ModelChatReply> chat(const ModelChatCommand& command);
  Outcome<ModelIntent> get_call_status(const ModelGetCallStatusQuery& query);
  Outcome<ModelMetadataView> get_model_metadata(const ModelMetadataQuery& query);
  Outcome<ModelListMetadataReply> list_model_metadata(const ModelListMetadataQuery& query);
  Outcome<ModelMutationReply> update_model(const ModelUpdateCommand& command);
  Outcome<ModelMutationReply> rotate_provider_secret(const ModelRotateSecretCommand& command);
  Outcome<ModelMutationReply> set_model_lifecycle(const ModelLifecycleCommand& command);
  Outcome<ModelConfigurationView> get_configuration(const ModelConfigurationQuery& query);
  Outcome<ModelIntent> resolve_call(const ModelResolveCallCommand& command);
  Outcome<Operation> get_operation(const ModelOperationQuery& query);
  // Bounded maintenance: abandon UNKNOWN rows past the 30-day window (rule-based, conservative
  // billing) and delete terminal rows past retention. Transient conflicts are retried by caller.
  Status maintenance();

private:
  Outcome<ModelAuthorizationDecision> authorize(const ModelCaller& caller, Permission permission,
                                                const ResourceRef& resource,
                                                const ModelCallContext& context);
  Outcome<ModelMetadataView> load_metadata(ModelUnitOfWork& work, const ResourceId& model_id);
  Outcome<ModelMutationReply> finish_mutation(ModelUnitOfWork& work, const ModelCaller& caller,
                                              const ModelCallContext& context,
                                              std::string_view method, const ResourceRef& resource,
                                              const Sha256Digest& fingerprint,
                                              const ResourceVersion& new_version,
                                              const ResourceVersion& authorization_version,
                                              std::optional<ModelMetadataView> metadata);
  Outcome<std::optional<ReplayResult>>
  find_receipt(ModelUnitOfWork& work, const ModelCaller& caller, std::string_view method,
               const ResourceRef& resource, const ModelCallContext& context,
               const Sha256Digest& fingerprint);
  Outcome<Sha256Digest> command_fingerprint(std::string_view method,
                                            const std::vector<std::byte>& payload);

  ModelApplicationDependencies dependencies_;
};

std::unique_ptr<ModelGatewayApplication>
make_model_gateway_application(ModelApplicationDependencies dependencies);

} // namespace kiarabot::core::target
