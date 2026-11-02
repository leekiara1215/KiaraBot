#include "kiarabot/core/application/target/model_gateway_application.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kiarabot::core::target {
namespace {

Failure fail(ErrorCode code, Recovery recovery = Recovery::Diagnose,
             Acceptance acceptance = Acceptance::NotAccepted) {
  return {code, acceptance, recovery, {}, {}};
}
Failure invalid() {
  return fail(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure conflict() {
  return fail(ErrorCode::Conflict, Recovery::ReadCurrent);
}
Failure not_found() {
  return fail(ErrorCode::NotFound, Recovery::ReadCurrent);
}
Failure unavailable() {
  return fail(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
Failure unknown_result(ErrorCode code) {
  return fail(code, Recovery::QuerySameIdentity, Acceptance::Unknown);
}
Failure accepted_failure(ErrorCode code,
                         std::optional<std::chrono::milliseconds> retry_after = std::nullopt) {
  auto failure = fail(code, Recovery::Diagnose, Acceptance::Accepted);
  failure.retry_after = retry_after;
  return failure;
}
Failure internal() {
  return fail(ErrorCode::Internal);
}
Failure permission_denied() {
  return fail(ErrorCode::PermissionDenied, Recovery::Authenticate);
}
Failure unauthenticated() {
  return fail(ErrorCode::Unauthenticated, Recovery::Authenticate);
}
Failure capability() {
  return fail(ErrorCode::CapabilityNotSupported, Recovery::CorrectInput);
}
bool bounded_text(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum &&
         value.find('\0') == std::string_view::npos;
}
bool valid_context(const ModelCallContext& context) {
  return bounded_text(context.request_id, 1, 128) && bounded_text(context.trace_id, 1, 128) &&
         context.deadline != TimePoint{};
}
TimePoint wall_now() {
  return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
class ByteWriter {
public:
  void text(std::string_view value) {
    u64(value.size());
    for (const char character : value) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
  }
  void u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      bytes.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  }
  void boolean(bool value) {
    bytes.push_back(static_cast<std::byte>(value ? 1 : 0));
  }
  void raw(std::span<const std::byte> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  std::vector<std::byte> bytes;
};
std::vector<std::byte> configuration_payload(const ModelConfiguration& config,
                                             std::optional<ResourceVersion> expected) {
  ByteWriter writer;
  writer.u64(expected ? expected->value : 0);
  writer.boolean(expected.has_value());
  writer.text(config.model_id.value);
  writer.text(config.alias);
  writer.text(std::string{model_provider_kind_name(config.provider_kind)});
  writer.text(config.endpoint);
  writer.text(config.provider_model);
  writer.u64(config.timeout_ms);
  writer.u64(config.max_concurrency);
  writer.u64(static_cast<std::uint64_t>(config.state));
  return std::move(writer.bytes);
}
std::vector<std::byte> rotate_payload(const ResourceId& model_id, std::span<const std::byte> secret,
                                      std::optional<ResourceVersion> expected) {
  ByteWriter writer;
  writer.u64(expected ? expected->value : 0);
  writer.boolean(expected.has_value());
  writer.text(model_id.value);
  writer.u64(secret.size());
  writer.raw(secret);
  return std::move(writer.bytes);
}
std::vector<std::byte> lifecycle_payload(const ResourceId& model_id, bool enable,
                                         std::optional<ResourceVersion> expected) {
  ByteWriter writer;
  writer.u64(expected ? expected->value : 0);
  writer.boolean(expected.has_value());
  writer.text(model_id.value);
  writer.boolean(enable);
  return std::move(writer.bytes);
}
std::vector<std::byte> resolve_payload(std::string_view logical_call_id,
                                       ModelCallDisposition disposition,
                                       std::optional<ResourceVersion> expected) {
  ByteWriter writer;
  writer.u64(expected ? expected->value : 0);
  writer.boolean(expected.has_value());
  writer.text(logical_call_id);
  writer.u64(static_cast<std::uint64_t>(disposition));
  return std::move(writer.bytes);
}
ResourceRef model_resource(const ResourceId& model_id) {
  return {ResourceKind::Model, model_id};
}
ModelChatDisposition replay_disposition(ModelCallState state) {
  return state == ModelCallState::Started ? ModelChatDisposition::ReplayInProgress
                                          : ModelChatDisposition::ReplayTerminal;
}
bool valid_configuration(const ModelConfiguration& config) {
  return valid_model_identifier(config.model_id.value) && valid_model_alias(config.alias) &&
         valid_model_endpoint(config.endpoint) && valid_provider_model(config.provider_model) &&
         valid_model_timeout(config.timeout_ms) &&
         valid_model_concurrency(config.max_concurrency) && config.version.value > 0;
}

} // namespace

ModelGatewayApplication::ModelGatewayApplication(ModelApplicationDependencies dependencies)
    : dependencies_(dependencies) {}

Outcome<ModelAuthorizationDecision>
ModelGatewayApplication::authorize(const ModelCaller& caller, Permission permission,
                                   const ResourceRef& resource, const ModelCallContext& context) {
  if (!caller.subject && !caller.owner) {
    return unauthenticated();
  }
  auto budget = std::chrono::milliseconds{1000};
  if (context.deadline != TimePoint{}) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline - wall_now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      return fail(ErrorCode::Timeout, Recovery::Backoff);
    }
    budget = std::min(budget, remaining);
  }
  try {
    return dependencies_.authorization.authorize(caller, permission, resource, budget);
  } catch (...) {
    return unavailable();
  }
}

Outcome<Sha256Digest>
ModelGatewayApplication::command_fingerprint(std::string_view method,
                                             const std::vector<std::byte>& payload) {
  ByteWriter writer;
  writer.text(method);
  writer.u64(payload.size());
  writer.raw(payload);
  try {
    return dependencies_.digest.sha256(writer.bytes);
  } catch (...) {
    return internal();
  }
}

Outcome<std::optional<ReplayResult>> ModelGatewayApplication::find_receipt(
    ModelUnitOfWork& work, const ModelCaller& caller, std::string_view method,
    const ResourceRef& resource, const ModelCallContext& context, const Sha256Digest& fingerprint) {
  if (!caller.subject) {
    return unauthenticated();
  }
  if (!bounded_text(context.idempotency_key, 1, 128)) {
    return invalid();
  }
  const ReceiptKey key{*caller.subject, std::string{method}, resource, context.idempotency_key};
  auto found = work.receipts().find(key);
  if (const auto* rejected = std::get_if<Failure>(&found)) {
    return *rejected;
  }
  const auto& receipt = std::get<std::optional<CommandReceipt>>(found);
  if (!receipt) {
    return std::optional<ReplayResult>{};
  }
  if (receipt->fingerprint != fingerprint) {
    return conflict();
  }
  return std::optional<ReplayResult>{receipt->result};
}

Outcome<ModelMetadataView> ModelGatewayApplication::load_metadata(ModelUnitOfWork& work,
                                                                  const ResourceId& model_id) {
  auto config = work.model_configs().find(model_id);
  if (const auto* rejected = std::get_if<Failure>(&config)) {
    return *rejected;
  }
  const auto& stored = std::get<std::optional<ModelConfiguration>>(config);
  if (!stored) {
    return not_found();
  }
  auto secret = work.provider_secrets().current(model_id);
  if (const auto* rejected = std::get_if<Failure>(&secret)) {
    return *rejected;
  }
  const auto& current = std::get<std::optional<ProviderSecretRecord>>(secret);
  return ModelMetadataView{*stored, current ? current->secret_version : ResourceVersion{0}};
}

Outcome<ModelChatReply> ModelGatewayApplication::chat(const ModelChatCommand& command) {
  if (!command.caller.owner || !valid_context(command.context) ||
      !valid_model_identifier(command.logical_call_id) || command.model.empty() ||
      command.model.size() > 256 || command.messages.empty() ||
      command.messages.size() > kModelMaximumMessages) {
    return invalid();
  }
  for (const auto& message : command.messages) {
    if (!valid_model_message_role(message.role) || message.text.find('\0') != std::string::npos) {
      return invalid();
    }
  }
  const auto canonical = model_request_canonical(command.model, command.messages);
  if (canonical.size() > kModelMaximumRequestBytes) {
    return invalid();
  }
  auto hash = dependencies_.digest.sha256(canonical);
  if (const auto* rejected = std::get_if<Failure>(&hash)) {
    return *rejected;
  }
  const auto request_hash = std::get<Sha256Digest>(hash);
  const ModelCallKey call_key{*command.caller.owner, command.logical_call_id};

  // Read-only precheck: an existing intent is a replay, never a second provider invocation.
  {
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return internal();
    }
    auto existing = work->model_calls().find(call_key);
    if (const auto* rejected = std::get_if<Failure>(&existing)) {
      work->rollback();
      return *rejected;
    }
    const auto& stored = std::get<std::optional<ModelIntent>>(existing);
    if (stored) {
      const auto call = *stored;
      work->rollback();
      if (call.request_hash != request_hash) {
        return conflict();
      }
      auto decision = authorize(command.caller, Permission::ModelChat, model_resource(call.model),
                                command.context);
      if (const auto* rejected = std::get_if<Failure>(&decision)) {
        return *rejected;
      }
      return ModelChatReply{replay_disposition(call.state), call, std::nullopt};
    }
    work->rollback();
  }

  // Resolve the current configuration once for authorization and reuse the canonical identity.
  ResourceId model_id;
  {
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return internal();
    }
    auto resolved = valid_model_identifier(command.model)
                        ? work->model_configs().find(ResourceId{command.model})
                    : valid_model_alias(command.model)
                        ? work->model_configs().find_by_alias(command.model)
                        : Outcome<std::optional<ModelConfiguration>>{invalid()};
    if (const auto* rejected = std::get_if<Failure>(&resolved)) {
      work->rollback();
      return *rejected;
    }
    const auto& config = std::get<std::optional<ModelConfiguration>>(resolved);
    if (!config) {
      work->rollback();
      return not_found();
    }
    model_id = config->model_id;
    work->rollback();
  }
  auto decision =
      authorize(command.caller, Permission::ModelChat, model_resource(model_id), command.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }

  ProviderSecretRecord pinned_secret;
  ModelConfiguration pinned_config;
  {
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return internal();
    }
    const auto now = work->current_time();
    if (const auto* rejected = std::get_if<Failure>(&now)) {
      work->rollback();
      return *rejected;
    }
    auto existing = work->model_calls().find(call_key);
    if (const auto* rejected = std::get_if<Failure>(&existing)) {
      work->rollback();
      return *rejected;
    }
    if (const auto& stored = std::get<std::optional<ModelIntent>>(existing)) {
      const auto call = *stored;
      work->rollback();
      if (call.request_hash != request_hash) {
        return conflict();
      }
      return ModelChatReply{replay_disposition(call.state), call, std::nullopt};
    }
    auto resolved = work->model_configs().find(model_id);
    if (const auto* rejected = std::get_if<Failure>(&resolved)) {
      work->rollback();
      return *rejected;
    }
    const auto& config = std::get<std::optional<ModelConfiguration>>(resolved);
    if (!config || config->state != LifecycleState::Active) {
      work->rollback();
      return capability();
    }
    auto secret = work->provider_secrets().current(model_id);
    if (const auto* rejected = std::get_if<Failure>(&secret)) {
      work->rollback();
      return *rejected;
    }
    const auto& current_secret = std::get<std::optional<ProviderSecretRecord>>(secret);
    if (!current_secret) {
      work->rollback();
      return unavailable();
    }
    pinned_config = *config;
    pinned_secret = *current_secret;
    ModelIntent intent;
    intent.owner = call_key.owner;
    intent.logical_call_id = call_key.logical_call_id;
    intent.model = model_id;
    intent.config_version = config->version;
    intent.request_hash = request_hash;
    intent.state = ModelCallState::Started;
    intent.version = {1};
    intent.created_at = std::get<TimePoint>(now);
    intent.observed_at = intent.created_at;
    intent.secret_version = current_secret->secret_version;
    auto inserted = work->model_calls().insert(intent);
    if (const auto* rejected = std::get_if<Failure>(&inserted)) {
      work->rollback();
      return *rejected;
    }
    switch (work->commit()) {
    case CommitResult::Committed:
      break;
    case CommitResult::RolledBack:
      return unavailable();
    case CommitResult::Unknown:
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
  }

  // Provider call happens outside every transaction. The borrowed plaintext secret is destroyed
  // with the returned buffer before the second transaction begins.
  ModelCallMutation mutation;
  std::optional<std::string> result_text;
  {
    auto aad = model_secret_aad(pinned_secret.model_id, pinned_secret.secret_version,
                                pinned_secret.root_key_version);
    auto plaintext =
        dependencies_.crypto.open(pinned_secret.sealed_secret, aad, pinned_secret.nonce);
    if (const auto* rejected = std::get_if<Failure>(&plaintext)) {
      mutation = model_definite_failure_mutation(ErrorCode::UpstreamUnavailable);
      mutation.may_have_billed = false;
    } else {
      const auto secret = std::get<OwnedSecret>(plaintext).get();
      ModelProviderRequest request;
      request.endpoint = pinned_config.endpoint;
      request.provider_model = pinned_config.provider_model;
      const auto timeout_ms =
          model_timeout_budget(pinned_config.timeout_ms, command.context.deadline, wall_now());
      if (timeout_ms < kModelMinimumTimeoutMs) {
        mutation = model_definite_failure_mutation(ErrorCode::Timeout);
        mutation.may_have_billed = false;
      } else {
        request.timeout_ms = timeout_ms;
        request.messages = command.messages;
        request.secret = secret ? secret->bytes() : SecretView{};
        ModelProviderResult provider;
        try {
          provider = dependencies_.provider.chat(request);
        } catch (...) {
          provider.outcome = ModelProviderOutcome::Unknown;
          provider.failure = unavailable();
        }
        switch (provider.outcome) {
        case ModelProviderOutcome::Succeeded:
          if (provider.reply.text.size() > kModelMaximumResultBytes ||
              !valid_provider_request_id(provider.reply.provider_request_id)) {
            mutation = model_unknown_mutation(ErrorCode::UpstreamUnavailable);
          } else {
            mutation = model_success_mutation(provider.reply);
            result_text = provider.reply.text;
          }
          break;
        case ModelProviderOutcome::DefiniteFailure:
          mutation = model_definite_failure_mutation(provider.failure.code);
          mutation.retry_after = provider.failure.retry_after;
          break;
        case ModelProviderOutcome::Unknown:
        default:
          mutation = model_unknown_mutation(provider.failure.code);
          break;
        }
      }
    }
  }

  // Second transaction: persist known success/failure or UNKNOWN. Never roll a call back to
  // "provider not invoked" after the provider call started.
  ModelIntent committed_call;
  {
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
    const auto now = work->current_time();
    if (const auto* rejected = std::get_if<Failure>(&now)) {
      work->rollback();
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
    auto existing = work->model_calls().find(call_key);
    if (const auto* rejected = std::get_if<Failure>(&existing)) {
      work->rollback();
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
    const auto& stored = std::get<std::optional<ModelIntent>>(existing);
    if (!stored) {
      work->rollback();
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
    ModelIntent updated = *stored;
    updated.state = mutation.state;
    updated.observed_at = std::get<TimePoint>(now);
    updated.secret_version = pinned_secret.secret_version;
    updated.usage_known = mutation.usage_known;
    updated.input_tokens = mutation.input_tokens;
    updated.output_tokens = mutation.output_tokens;
    updated.may_have_billed = mutation.may_have_billed;
    updated.provider_request_id = mutation.provider_request_id;
    updated.latency_ms = mutation.latency_ms;
    updated.error = mutation.error;
    updated.version.value += 1;
    auto saved = work->model_calls().save(updated, stored->version);
    if (const auto* rejected = std::get_if<Failure>(&saved)) {
      work->rollback();
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
    switch (work->commit()) {
    case CommitResult::Committed:
      committed_call = updated;
      break;
    case CommitResult::RolledBack:
    case CommitResult::Unknown:
      return unknown_result(ErrorCode::UpstreamUnavailable);
    }
  }
  if (mutation.state == ModelCallState::Unknown) {
    return unknown_result(mutation.error.value_or(ErrorCode::UpstreamUnavailable));
  }
  if (mutation.state == ModelCallState::Failed) {
    return accepted_failure(mutation.error.value_or(ErrorCode::Internal), mutation.retry_after);
  }
  return ModelChatReply{ModelChatDisposition::FreshResult, committed_call, std::move(result_text)};
}

Outcome<ModelIntent>
ModelGatewayApplication::get_call_status(const ModelGetCallStatusQuery& query) {
  if (!valid_context(query.context) || !valid_model_identifier(query.logical_call_id)) {
    return invalid();
  }
  const auto load = [&](ModelUnitOfWork& work) -> Outcome<ModelIntent> {
    if (query.caller.owner) {
      auto found = work.model_calls().find({*query.caller.owner, query.logical_call_id});
      if (const auto* rejected = std::get_if<Failure>(&found)) {
        return *rejected;
      }
      const auto& call = std::get<std::optional<ModelIntent>>(found);
      return call ? Outcome<ModelIntent>{*call} : Outcome<ModelIntent>{not_found()};
    }
    if (!query.model_scope) {
      return invalid();
    }
    auto found = work.model_calls().find_by_call_id(query.logical_call_id);
    if (const auto* rejected = std::get_if<Failure>(&found)) {
      return *rejected;
    }
    const auto& call = std::get<std::optional<ModelIntent>>(found);
    if (!call || call->model.value != query.model_scope->value) {
      return not_found();
    }
    return *call;
  };
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  auto call = load(*work);
  work->rollback();
  if (const auto* rejected = std::get_if<Failure>(&call)) {
    return *rejected;
  }
  const auto& value = std::get<ModelIntent>(call);
  auto decision = authorize(query.caller, Permission::ModelCallInspect, model_resource(value.model),
                            query.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  return value;
}

Outcome<ModelMetadataView>
ModelGatewayApplication::get_model_metadata(const ModelMetadataQuery& query) {
  if (!valid_context(query.context) || !valid_model_identifier(query.model_id.value)) {
    return invalid();
  }
  auto decision =
      authorize(query.caller, Permission::ModelRead, model_resource(query.model_id), query.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  auto metadata = load_metadata(*work, query.model_id);
  work->rollback();
  return metadata;
}

Outcome<ModelListMetadataReply>
ModelGatewayApplication::list_model_metadata(const ModelListMetadataQuery& query) {
  if (!valid_context(query.context) || query.scope.empty() || query.scope.size() > 100) {
    return invalid();
  }
  std::vector<ResourceId> seen;
  ModelListMetadataReply reply;
  for (const auto& model_id : query.scope) {
    if (!valid_model_identifier(model_id.value)) {
      return invalid();
    }
    auto decision =
        authorize(query.caller, Permission::ModelRead, model_resource(model_id), query.context);
    if (const auto* rejected = std::get_if<Failure>(&decision)) {
      return *rejected;
    }
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return internal();
    }
    auto metadata = load_metadata(*work, model_id);
    work->rollback();
    if (const auto* rejected = std::get_if<Failure>(&metadata)) {
      if (rejected->code == ErrorCode::NotFound) {
        continue;
      }
      return *rejected;
    }
    reply.models.push_back(std::get<ModelMetadataView>(metadata));
  }
  return reply;
}

Outcome<ModelMutationReply> ModelGatewayApplication::finish_mutation(
    ModelUnitOfWork& work, const ModelCaller& caller, const ModelCallContext& context,
    std::string_view method, const ResourceRef& resource, const Sha256Digest& fingerprint,
    const ResourceVersion& new_version, const ResourceVersion& authorization_version,
    std::optional<ModelMetadataView> metadata) {
  if (!caller.subject) {
    return unauthenticated();
  }
  auto id = dependencies_.ids.uuid();
  if (const auto* rejected = std::get_if<Failure>(&id)) {
    return *rejected;
  }
  Operation operation;
  operation.ref = {Service::Model, OperationId{std::get<std::string>(id)}, resource};
  operation.version = {1};
  operation.state = OperationState::Succeeded;
  operation.execution_outcome = ExecutionOutcome::KnownSucceeded;
  auto saved_operation = work.operations().save(operation, std::nullopt);
  if (const auto* rejected = std::get_if<Failure>(&saved_operation)) {
    return *rejected;
  }
  const auto now = work.current_time();
  if (const auto* rejected = std::get_if<Failure>(&now)) {
    return *rejected;
  }
  auto audited = work.journal().append_audit(AuditEntry{
      *caller.subject, resource, context.request_id, context.trace_id, std::string{method},
      std::nullopt, authorization_version, std::get<TimePoint>(now)});
  if (const auto* rejected = std::get_if<Failure>(&audited)) {
    return *rejected;
  }
  const ReceiptKey key{*caller.subject, std::string{method}, resource, context.idempotency_key};
  auto inserted = work.receipts().insert(
      CommandReceipt{key, fingerprint, ResourceMutationResult{resource, new_version},
                     std::get<TimePoint>(now), std::get<TimePoint>(now) + kMinimumReceiptRetention},
      ReceiptCapacity{});
  if (const auto* rejected = std::get_if<Failure>(&inserted)) {
    return *rejected;
  }
  switch (work.commit()) {
  case CommitResult::Committed:
    break;
  case CommitResult::RolledBack:
    return unavailable();
  case CommitResult::Unknown:
    return unknown_result(ErrorCode::UpstreamUnavailable);
  }
  return ModelMutationReply{std::move(metadata), new_version, operation.ref};
}

Outcome<ModelMutationReply>
ModelGatewayApplication::update_model(const ModelUpdateCommand& command) {
  ModelConfiguration config = command.config;
  if (!valid_context(command.context) || !valid_model_identifier(config.model_id.value) ||
      !valid_model_alias(config.alias) || !valid_model_endpoint(config.endpoint) ||
      !valid_provider_model(config.provider_model) || !valid_model_timeout(config.timeout_ms) ||
      !valid_model_concurrency(config.max_concurrency)) {
    return invalid();
  }
  const auto resource = model_resource(config.model_id);
  auto decision = authorize(command.caller, Permission::ModelManage, resource, command.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  auto payload = configuration_payload(config, command.expected_version);
  auto fingerprint = command_fingerprint("ModelService.UpdateModel", payload);
  if (const auto* rejected = std::get_if<Failure>(&fingerprint)) {
    return *rejected;
  }
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  {
    auto replay = find_receipt(*work, command.caller, "ModelService.UpdateModel", resource,
                               command.context, std::get<Sha256Digest>(fingerprint));
    if (const auto* rejected = std::get_if<Failure>(&replay)) {
      work->rollback();
      return *rejected;
    }
    if (const auto& receipt = std::get<std::optional<ReplayResult>>(replay)) {
      if (const auto* result = std::get_if<ResourceMutationResult>(&*receipt)) {
        auto existing = work->model_configs().find(config.model_id);
        if (const auto* rejected = std::get_if<Failure>(&existing)) {
          work->rollback();
          return *rejected;
        }
        const auto& stored = std::get<std::optional<ModelConfiguration>>(existing);
        work->rollback();
        if (!stored) {
          return not_found();
        }
        auto secret = dependencies_.transactions.begin();
        if (const auto* rejected = std::get_if<Failure>(&secret)) {
          return *rejected;
        }
        auto secret_work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(secret));
        if (!secret_work) {
          return internal();
        }
        auto current = secret_work->provider_secrets().current(config.model_id);
        const auto version =
            std::get_if<Failure>(&current)
                ? ResourceVersion{0}
                : (std::get<std::optional<ProviderSecretRecord>>(current)
                       ? std::get<std::optional<ProviderSecretRecord>>(current)->secret_version
                       : ResourceVersion{0});
        secret_work->rollback();
        return ModelMutationReply{ModelMetadataView{*stored, version}, result->version,
                                  std::nullopt};
      }
      work->rollback();
      return conflict();
    }
  }
  auto existing = work->model_configs().find(config.model_id);
  if (const auto* rejected = std::get_if<Failure>(&existing)) {
    work->rollback();
    return *rejected;
  }
  const auto& stored = std::get<std::optional<ModelConfiguration>>(existing);
  const auto now = work->current_time();
  if (const auto* rejected = std::get_if<Failure>(&now)) {
    work->rollback();
    return *rejected;
  }
  if (stored) {
    if (!command.expected_version || command.expected_version->value != stored->version.value) {
      work->rollback();
      return conflict();
    }
    config.created_at = stored->created_at;
    config.state = stored->state;
    config.version.value = stored->version.value + 1;
    config.observed_at = std::get<TimePoint>(now);
    auto saved = work->model_configs().save(config, stored->version);
    if (const auto* rejected = std::get_if<Failure>(&saved)) {
      work->rollback();
      return *rejected;
    }
  } else {
    if (command.expected_version) {
      work->rollback();
      return conflict();
    }
    // Creation always starts disabled; EnableModel is the explicit activation command.
    config.state = LifecycleState::Disabled;
    config.version = {1};
    config.created_at = std::get<TimePoint>(now);
    config.observed_at = config.created_at;
    auto saved = work->model_configs().save(config, std::nullopt);
    if (const auto* rejected = std::get_if<Failure>(&saved)) {
      work->rollback();
      return *rejected;
    }
  }
  auto metadata = load_metadata(*work, config.model_id);
  if (const auto* rejected = std::get_if<Failure>(&metadata)) {
    work->rollback();
    return *rejected;
  }
  return finish_mutation(*work, command.caller, command.context, "ModelService.UpdateModel",
                         resource, std::get<Sha256Digest>(fingerprint), config.version,
                         std::get<ModelAuthorizationDecision>(decision).authorization_version,
                         std::get<ModelMetadataView>(metadata));
}

Outcome<ModelMutationReply>
ModelGatewayApplication::rotate_provider_secret(const ModelRotateSecretCommand& command) {
  if (!valid_context(command.context) || !valid_model_identifier(command.model_id.value) ||
      !valid_model_secret(command.secret)) {
    return invalid();
  }
  const auto resource = model_resource(command.model_id);
  auto decision = authorize(command.caller, Permission::ModelManage, resource, command.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  auto payload = rotate_payload(command.model_id, command.secret, command.expected_version);
  auto fingerprint = command_fingerprint("ModelService.RotateProviderSecret", payload);
  if (const auto* rejected = std::get_if<Failure>(&fingerprint)) {
    return *rejected;
  }
  // Read current versions, close the transaction, seal outside any UoW, then CAS-insert.
  ResourceVersion next_version{1};
  {
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return internal();
    }
    auto replay = find_receipt(*work, command.caller, "ModelService.RotateProviderSecret", resource,
                               command.context, std::get<Sha256Digest>(fingerprint));
    if (const auto* rejected = std::get_if<Failure>(&replay)) {
      work->rollback();
      return *rejected;
    }
    if (const auto& receipt = std::get<std::optional<ReplayResult>>(replay)) {
      if (const auto* result = std::get_if<ResourceMutationResult>(&*receipt)) {
        work->rollback();
        auto metadata = ModelMetadataView{};
        return ModelMutationReply{std::nullopt, result->version, std::nullopt};
      }
      work->rollback();
      return conflict();
    }
    auto config = work->model_configs().find(command.model_id);
    if (const auto* rejected = std::get_if<Failure>(&config)) {
      work->rollback();
      return *rejected;
    }
    if (!std::get<std::optional<ModelConfiguration>>(config)) {
      work->rollback();
      return not_found();
    }
    auto current = work->provider_secrets().current(command.model_id);
    if (const auto* rejected = std::get_if<Failure>(&current)) {
      work->rollback();
      return *rejected;
    }
    const auto& secret = std::get<std::optional<ProviderSecretRecord>>(current);
    if (command.expected_version) {
      const auto expected = secret ? secret->secret_version : ResourceVersion{0};
      if (command.expected_version->value != expected.value) {
        work->rollback();
        return conflict();
      }
    }
    next_version = ResourceVersion{(secret ? secret->secret_version.value : 0) + 1};
    work->rollback();
  }
  ProviderSecretRecord sealed;
  sealed.model_id = command.model_id;
  sealed.secret_version = next_version;
  sealed.root_key_version = dependencies_.root_key_version;
  auto aad = model_secret_aad(sealed.model_id, sealed.secret_version, sealed.root_key_version);
  auto ciphertext = dependencies_.crypto.seal(command.secret, aad, sealed.nonce);
  if (const auto* rejected = std::get_if<Failure>(&ciphertext)) {
    return *rejected;
  }
  sealed.sealed_secret = std::get<std::vector<std::byte>>(ciphertext);
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  const auto now = work->current_time();
  if (const auto* rejected = std::get_if<Failure>(&now)) {
    work->rollback();
    return *rejected;
  }
  sealed.created_at = std::get<TimePoint>(now);
  // A concurrent rotation that already took this secret version wins; the caller must re-read.
  auto current = work->provider_secrets().current(command.model_id);
  if (const auto* rejected = std::get_if<Failure>(&current)) {
    work->rollback();
    return *rejected;
  }
  const auto& current_secret = std::get<std::optional<ProviderSecretRecord>>(current);
  const auto expected_next =
      ResourceVersion{(current_secret ? current_secret->secret_version.value : 0) + 1};
  if (expected_next.value != sealed.secret_version.value) {
    work->rollback();
    return conflict();
  }
  auto inserted = work->provider_secrets().insert(sealed);
  if (const auto* rejected = std::get_if<Failure>(&inserted)) {
    work->rollback();
    return *rejected;
  }
  return finish_mutation(
      *work, command.caller, command.context, "ModelService.RotateProviderSecret", resource,
      std::get<Sha256Digest>(fingerprint), sealed.secret_version,
      std::get<ModelAuthorizationDecision>(decision).authorization_version, std::nullopt);
}

Outcome<ModelMutationReply>
ModelGatewayApplication::set_model_lifecycle(const ModelLifecycleCommand& command) {
  if (!valid_context(command.context) || !valid_model_identifier(command.model_id.value) ||
      !command.expected_version) {
    return invalid();
  }
  const auto resource = model_resource(command.model_id);
  auto decision = authorize(command.caller, Permission::ModelManage, resource, command.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  auto payload = lifecycle_payload(command.model_id, command.enable, command.expected_version);
  const auto method = command.enable ? "ModelService.EnableModel" : "ModelService.DisableModel";
  auto fingerprint = command_fingerprint(method, payload);
  if (const auto* rejected = std::get_if<Failure>(&fingerprint)) {
    return *rejected;
  }
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  auto replay = find_receipt(*work, command.caller, method, resource, command.context,
                             std::get<Sha256Digest>(fingerprint));
  if (const auto* rejected = std::get_if<Failure>(&replay)) {
    work->rollback();
    return *rejected;
  }
  if (const auto& receipt = std::get<std::optional<ReplayResult>>(replay)) {
    if (const auto* result = std::get_if<ResourceMutationResult>(&*receipt)) {
      auto existing = work->model_configs().find(command.model_id);
      if (const auto* rejected = std::get_if<Failure>(&existing)) {
        work->rollback();
        return *rejected;
      }
      const auto& stored = std::get<std::optional<ModelConfiguration>>(existing);
      work->rollback();
      return ModelMutationReply{
          stored ? std::optional<ModelMetadataView>{ModelMetadataView{*stored, ResourceVersion{0}}}
                 : std::nullopt,
          result->version, std::nullopt};
    }
    work->rollback();
    return conflict();
  }
  auto existing = work->model_configs().find(command.model_id);
  if (const auto* rejected = std::get_if<Failure>(&existing)) {
    work->rollback();
    return *rejected;
  }
  const auto& stored = std::get<std::optional<ModelConfiguration>>(existing);
  if (!stored) {
    work->rollback();
    return not_found();
  }
  if (stored->version.value != command.expected_version->value) {
    work->rollback();
    return conflict();
  }
  auto updated = *stored;
  updated.state = command.enable ? LifecycleState::Active : LifecycleState::Disabled;
  updated.version.value += 1;
  const auto now = work->current_time();
  if (const auto* rejected = std::get_if<Failure>(&now)) {
    work->rollback();
    return *rejected;
  }
  updated.observed_at = std::get<TimePoint>(now);
  auto saved = work->model_configs().save(updated, stored->version);
  if (const auto* rejected = std::get_if<Failure>(&saved)) {
    work->rollback();
    return *rejected;
  }
  auto metadata = load_metadata(*work, command.model_id);
  if (const auto* rejected = std::get_if<Failure>(&metadata)) {
    work->rollback();
    return *rejected;
  }
  return finish_mutation(*work, command.caller, command.context, method, resource,
                         std::get<Sha256Digest>(fingerprint), updated.version,
                         std::get<ModelAuthorizationDecision>(decision).authorization_version,
                         std::get<ModelMetadataView>(metadata));
}

Outcome<ModelConfigurationView>
ModelGatewayApplication::get_configuration(const ModelConfigurationQuery& query) {
  if (!valid_context(query.context) || !valid_model_identifier(query.model_id.value)) {
    return invalid();
  }
  auto decision = authorize(query.caller, Permission::ModelManage, model_resource(query.model_id),
                            query.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  auto config = work->model_configs().find(query.model_id);
  if (const auto* rejected = std::get_if<Failure>(&config)) {
    work->rollback();
    return *rejected;
  }
  const auto& stored = std::get<std::optional<ModelConfiguration>>(config);
  if (!stored) {
    work->rollback();
    return not_found();
  }
  auto secret = work->provider_secrets().current(query.model_id);
  if (const auto* rejected = std::get_if<Failure>(&secret)) {
    work->rollback();
    return *rejected;
  }
  const auto& current = std::get<std::optional<ProviderSecretRecord>>(secret);
  const auto observed_at = stored->observed_at;
  work->rollback();
  return ModelConfigurationView{*stored, current ? current->secret_version : ResourceVersion{0},
                                static_cast<bool>(current), observed_at};
}

Outcome<ModelIntent> ModelGatewayApplication::resolve_call(const ModelResolveCallCommand& command) {
  if (!valid_context(command.context) || !valid_model_identifier(command.logical_call_id) ||
      !valid_call_disposition(command.disposition) || !command.expected_version) {
    return invalid();
  }
  auto payload =
      resolve_payload(command.logical_call_id, command.disposition, command.expected_version);
  auto fingerprint = command_fingerprint("ModelService.ResolveModelCall", payload);
  if (const auto* rejected = std::get_if<Failure>(&fingerprint)) {
    return *rejected;
  }
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  auto found = work->model_calls().find_by_call_id(command.logical_call_id);
  if (const auto* rejected = std::get_if<Failure>(&found)) {
    work->rollback();
    return *rejected;
  }
  const auto& call = std::get<std::optional<ModelIntent>>(found);
  if (!call) {
    work->rollback();
    return not_found();
  }
  if (command.caller.owner) {
    work->rollback();
    return permission_denied();
  }
  auto decision = authorize(command.caller, Permission::ModelManage, model_resource(call->model),
                            command.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    work->rollback();
    return *rejected;
  }
  auto replay = find_receipt(*work, command.caller, "ModelService.ResolveModelCall",
                             model_resource(call->model), command.context,
                             std::get<Sha256Digest>(fingerprint));
  if (const auto* rejected = std::get_if<Failure>(&replay)) {
    work->rollback();
    return *rejected;
  }
  if (std::get<std::optional<ReplayResult>>(replay)) {
    const auto snapshot = *call;
    work->rollback();
    return snapshot;
  }
  if (call->state != ModelCallState::Unknown ||
      call->disposition != ModelCallDisposition::Unspecified) {
    work->rollback();
    return conflict();
  }
  if (call->version.value != command.expected_version->value) {
    work->rollback();
    return conflict();
  }
  const auto now = work->current_time();
  if (const auto* rejected = std::get_if<Failure>(&now)) {
    work->rollback();
    return *rejected;
  }
  auto resolved =
      model_resolve_unknown(*call, command.disposition, std::get<TimePoint>(now),
                            command.caller.subject ? command.caller.subject->value : "control");
  auto saved = work->model_calls().save(resolved, call->version);
  if (const auto* rejected = std::get_if<Failure>(&saved)) {
    work->rollback();
    return *rejected;
  }
  const ReceiptKey key{*command.caller.subject, "ModelService.ResolveModelCall",
                       model_resource(call->model), command.context.idempotency_key};
  auto receipt = work->receipts().insert(
      CommandReceipt{key, std::get<Sha256Digest>(fingerprint),
                     ResourceMutationResult{model_resource(call->model), resolved.version},
                     std::get<TimePoint>(now), std::get<TimePoint>(now) + kMinimumReceiptRetention},
      ReceiptCapacity{});
  if (const auto* rejected = std::get_if<Failure>(&receipt)) {
    work->rollback();
    return *rejected;
  }
  auto audited = work->journal().append_audit(AuditEntry{
      command.caller.subject.value_or(SubjectId{"control"}), model_resource(call->model),
      command.context.request_id, command.context.trace_id, "ModelService.ResolveModelCall",
      std::nullopt, std::get<ModelAuthorizationDecision>(decision).authorization_version,
      std::get<TimePoint>(now)});
  if (const auto* rejected = std::get_if<Failure>(&audited)) {
    work->rollback();
    return *rejected;
  }
  switch (work->commit()) {
  case CommitResult::Committed:
    return resolved;
  case CommitResult::RolledBack:
    return unavailable();
  case CommitResult::Unknown:
    return unknown_result(ErrorCode::UpstreamUnavailable);
  }
  return internal();
}

Outcome<Operation> ModelGatewayApplication::get_operation(const ModelOperationQuery& query) {
  if (!valid_context(query.context) || query.reference.owner != Service::Model ||
      query.reference.id.value.empty()) {
    return invalid();
  }
  auto decision =
      authorize(query.caller, Permission::ResourceRead, query.reference.resource, query.context);
  if (const auto* rejected = std::get_if<Failure>(&decision)) {
    return *rejected;
  }
  auto begun = dependencies_.transactions.begin();
  if (const auto* rejected = std::get_if<Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
  if (!work) {
    return internal();
  }
  auto found = work->operations().find(query.reference.id);
  work->rollback();
  if (const auto* rejected = std::get_if<Failure>(&found)) {
    return *rejected;
  }
  const auto& operation = std::get<std::optional<Operation>>(found);
  if (!operation || operation->ref != query.reference) {
    return not_found();
  }
  return *operation;
}

Status ModelGatewayApplication::maintenance() {
  try {
    auto begun = dependencies_.transactions.begin();
    if (const auto* rejected = std::get_if<Failure>(&begun)) {
      if (rejected->code == ErrorCode::Conflict ||
          rejected->code == ErrorCode::UpstreamUnavailable ||
          rejected->code == ErrorCode::Timeout) {
        return std::monostate{};
      }
      return *rejected;
    }
    auto work = std::move(std::get<std::unique_ptr<ModelUnitOfWork>>(begun));
    if (!work) {
      return unavailable();
    }
    const auto now = work->current_time();
    if (const auto* rejected = std::get_if<Failure>(&now)) {
      work->rollback();
      return *rejected;
    }
    const auto timestamp = std::get<TimePoint>(now);
    auto unresolved = work->model_calls().list_unresolved_unknown(timestamp, 32);
    if (const auto* rejected = std::get_if<Failure>(&unresolved)) {
      work->rollback();
      return *rejected;
    }
    for (const auto& call : std::get<std::vector<ModelIntent>>(unresolved)) {
      auto abandoned = model_abandon_unknown(call, timestamp, "model-maintenance");
      auto saved = work->model_calls().save(abandoned, call.version);
      if (const auto* rejected = std::get_if<Failure>(&saved)) {
        work->rollback();
        return *rejected;
      }
      auto audited = work->journal().append_audit(AuditEntry{SubjectId{"model-maintenance"},
                                                             model_resource(call.model),
                                                             {},
                                                             {},
                                                             "ModelService.AbandonUnknown",
                                                             std::nullopt,
                                                             {1},
                                                             timestamp});
      if (const auto* rejected = std::get_if<Failure>(&audited)) {
        work->rollback();
        return *rejected;
      }
    }
    auto pruned = work->model_calls().prune_terminal(timestamp - kModelCallRetention, 64);
    if (const auto* rejected = std::get_if<Failure>(&pruned)) {
      work->rollback();
      return *rejected;
    }
    switch (work->commit()) {
    case CommitResult::Committed:
      return std::monostate{};
    case CommitResult::RolledBack:
      return unavailable();
    case CommitResult::Unknown:
      return unavailable();
    }
  } catch (...) {
    return unavailable();
  }
  return unavailable();
}

std::unique_ptr<ModelGatewayApplication>
make_model_gateway_application(ModelApplicationDependencies dependencies) {
  return std::make_unique<ModelGatewayApplication>(dependencies);
}

} // namespace kiarabot::core::target
