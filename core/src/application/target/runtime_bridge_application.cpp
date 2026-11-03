#include "kiarabot/core/application/target/runtime_bridge_application.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/runtime_rules.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"

namespace kiarabot::core::target {
namespace {

Failure bridge_failure(ErrorCode code, Recovery recovery) {
  return {code, Acceptance::NotAccepted, recovery, {}, {}};
}
Failure unauthenticated() {
  return bridge_failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
}
Failure permission_denied() {
  return bridge_failure(ErrorCode::PermissionDenied, Recovery::Authenticate);
}
Failure unavailable() {
  return bridge_failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
Failure internal_failure() {
  return bridge_failure(ErrorCode::Internal, Recovery::Diagnose);
}
Failure capability_not_supported() {
  return bridge_failure(ErrorCode::CapabilityNotSupported, Recovery::CorrectInput);
}
Failure runtime_conflict_failure() {
  return bridge_failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}

// Frozen plugin permission vocabulary from doc/03-plugin-sdk.md. It is a declaration vocabulary,
// not an authorization decision; every Action/model/storage call is still authorized online.
constexpr std::array<std::string_view, 11> kPluginPermissionVocabulary{
    "message.read",  "message.send", "member.read",       "member.mute",
    "member.kick",   "request.read", "request.approve",   "adapter.read",
    "task.schedule", "model.chat",   "storage.namespace",
};
constexpr std::string_view kPluginApiVersion = "kiarabot.plugin.v1";
constexpr std::string_view kAdapterProtocolVersion = "kiarabot.v1";

bool known_plugin_permission(std::string_view value) {
  return std::find(kPluginPermissionVocabulary.begin(), kPluginPermissionVocabulary.end(), value) !=
         kPluginPermissionVocabulary.end();
}
bool identifier(std::string_view value) {
  return value.size() >= 1 && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '.' ||
                  character == '-';
         });
}
bool entrypoint(std::string_view value) {
  if (value.empty() || value.size() > 256) {
    return false;
  }
  const auto separator = value.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size()) {
    return false;
  }
  const auto module = value.substr(0, separator);
  const auto symbol = value.substr(separator + 1);
  if (!((module.front() >= 'A' && module.front() <= 'Z') ||
        (module.front() >= 'a' && module.front() <= 'z') || module.front() == '_')) {
    return false;
  }
  for (const auto character : module) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '_' || character == '.')) {
      return false;
    }
  }
  if (!((symbol.front() >= 'A' && symbol.front() <= 'Z') ||
        (symbol.front() >= 'a' && symbol.front() <= 'z') || symbol.front() == '_')) {
    return false;
  }
  for (const auto character : symbol) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '_')) {
      return false;
    }
  }
  return true;
}
bool unique_identifiers(const std::vector<std::string>& values) {
  if (values.size() > 128) {
    return false;
  }
  std::vector<std::string> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end() &&
         std::all_of(values.begin(), values.end(), identifier);
}
// manifest validation mirrors the shipped SDK pre-flight; the server remains authoritative for
// what it accepts (unknown capability -> CapabilityNotSupported, missing permission ->
// PermissionDenied, malformed field -> InvalidArgument).
Outcome<std::monostate> validate_plugin_manifest(const BridgePluginManifest& manifest) {
  if (!identifier(manifest.id) || !identifier(manifest.version) ||
      !bounded_text(manifest.name, 1, 128) || manifest.api_version != kPluginApiVersion ||
      !entrypoint(manifest.entrypoint) || !unique_identifiers(manifest.capabilities) ||
      !unique_identifiers(manifest.permissions)) {
    return invalid_runtime_input();
  }
  if (manifest.runtime != "python-rpc" ||
      (!manifest.isolation.empty() && manifest.isolation != "process") ||
      !manifest.dependencies.empty() ||
      (!manifest.config_schema.empty() && manifest.config_schema != "{}")) {
    return capability_not_supported();
  }
  for (const auto& permission : manifest.permissions) {
    if (!known_plugin_permission(permission)) {
      return capability_not_supported();
    }
  }
  for (const auto& capability : manifest.capabilities) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6>
        kCapabilityPermissions{{{"event.message.received", "message.read"},
                                {"event.message.sent", "message.read"},
                                {"event.group.member.changed", "member.read"},
                                {"event.friend.request", "request.read"},
                                {"event.adapter.status.changed", "adapter.read"},
                                {"event.timer.triggered", "task.schedule"}}};
    const auto required =
        std::find_if(kCapabilityPermissions.begin(), kCapabilityPermissions.end(),
                     [&](const auto& entry) { return entry.first == capability; });
    if (required == kCapabilityPermissions.end()) {
      return capability_not_supported();
    }
    if (std::find(manifest.permissions.begin(), manifest.permissions.end(), required->second) ==
        manifest.permissions.end()) {
      return permission_denied();
    }
  }
  return std::monostate{};
}

struct ResolvedBootstrap {
  BridgeCredentialRecord credential;
  InstanceId resource;
  TimePoint now;
};

std::string bridge_key(BridgePeerKind kind, const InstanceId& instance) {
  return std::string{bridge_peer_name(kind)} + ':' + instance.value;
}

class RuntimeBridgeApplicationImpl final : public RuntimeBridgeApplication {
public:
  explicit RuntimeBridgeApplicationImpl(RuntimeBridgeDependencies dependencies)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        secrets_(dependencies.secrets), resources_(dependencies.resources),
        actions_(dependencies.actions),
        supported_features_(std::move(dependencies.supported_features)) {}

  Outcome<BridgeRegistrationResult>
  register_adapter(const BridgeAdapterRegistration& request) override {
    if (request.request_id.empty() || request.trace_id.empty() || request.adapter.value.empty() ||
        !request.bearer_credential.size()) {
      return invalid_runtime_input();
    }
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_.size() >= kMaximumBridgeConnections) {
          return bridge_failure(ErrorCode::RateLimited, Recovery::Backoff);
        }
      }
      auto resolved = resolve_bootstrap(BridgePeerKind::Adapter, request.bearer_credential);
      if (const auto* failure = std::get_if<Failure>(&resolved)) {
        return *failure;
      }
      const auto bootstrap = std::get<ResolvedBootstrap>(resolved);
      if (request.presented_instance &&
          request.presented_instance->value != bootstrap.resource.value) {
        return permission_denied();
      }
      auto epoch = process_epoch();
      if (const auto* failure = std::get_if<Failure>(&epoch)) {
        return *failure;
      }
      AdapterRegistrationCommand command;
      command.authentication.audience = Service::Runtime;
      command.authentication.subject = SubjectId{bootstrap.resource.value};
      command.authentication.direct_peer =
          AdapterPeer{request.adapter, SubjectId{bootstrap.resource.value}};
      command.authentication.authorization_version = ResourceVersion{1};
      command.authentication.verified_at = bootstrap.now;
      command.authentication.expires_at = bootstrap.now + kBridgeAuthorizationLifetime;
      command.request_id = request.request_id;
      command.trace_id = request.trace_id;
      command.deadline = request.deadline;
      command.adapter = request.adapter;
      command.metadata = {request.implementation,
                          request.implementation_version,
                          request.protocol_version.empty() ? std::string{kAdapterProtocolVersion}
                                                           : request.protocol_version,
                          request.declared_capabilities,
                          {}};
      command.presented_instance = request.presented_instance;
      command.declared_accounts = request.declared_accounts;
      command.assigned_instance = bootstrap.resource;
      command.bootstrap_authenticated = true;
      command.runtime_epoch = std::get<RuntimeEpoch>(epoch);
      auto registered = resources_.register_adapter(command);
      if (const auto* failure = std::get_if<Failure>(&registered)) {
        return *failure;
      }
      const auto& result = std::get<AdapterRegistrationResult>(registered);
      BridgeRegistrationResult bridge;
      bridge.assigned_instance_id = result.adapter.current.instance;
      bridge.heartbeat_interval_seconds = result.heartbeat_interval_seconds;
      bridge.effective_capabilities = result.effective_capabilities;
      bridge.required_protocol_version = std::string{kAdapterProtocolVersion};
      auto issued = issue_authorization(
          BridgePeerKind::Adapter, result.adapter.current.instance,
          BridgeFence{result.adapter.current.instance, result.adapter.current.runtime_epoch,
                      ExecutionGeneration{1}, result.adapter.current.connection_generation},
          result.adapter.last_registered_at, result.adapter.version.value);
      if (const auto* failure = std::get_if<Failure>(&issued)) {
        return *failure;
      }
      bridge.authorization = std::move(std::get<BridgeAuthorization>(issued));
      return bridge;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<BridgeRegistrationResult>
  register_plugin(const BridgePluginRegistration& request) override {
    if (request.request_id.empty() || request.instance_id.empty() ||
        request.bearer_credential.empty()) {
      return invalid_runtime_input();
    }
    auto manifest = validate_plugin_manifest(request.manifest);
    if (const auto* failure = std::get_if<Failure>(&manifest)) {
      return *failure;
    }
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_.size() >= kMaximumBridgeConnections) {
          return bridge_failure(ErrorCode::RateLimited, Recovery::Backoff);
        }
      }
      auto resolved = resolve_bootstrap(BridgePeerKind::Plugin, request.bearer_credential);
      if (const auto* failure = std::get_if<Failure>(&resolved)) {
        return *failure;
      }
      const auto bootstrap = std::get<ResolvedBootstrap>(resolved);
      const InstanceId instance{bootstrap.resource};
      if (request.instance_id != instance.value) {
        return permission_denied();
      }
      auto epoch = process_epoch();
      if (const auto* failure = std::get_if<Failure>(&epoch)) {
        return *failure;
      }
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&sampled)) {
        return *failure;
      }
      const auto now = std::get<TimePoint>(sampled);
      auto found = work->instances().find(instance);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      auto current = std::move(std::get<std::optional<PluginInstance>>(found));
      if (!current) {
        return runtime_not_found();
      }
      if (current->state != LifecycleState::Active || current->host.has_value()) {
        return permission_denied();
      }
      if (request.installation_id &&
          current->installation.value != request.installation_id->value) {
        return permission_denied();
      }
      // B1 wire field: the client may only repeat the already allocated external execution
      // generation (the first registration allocates 1); a different value never changes it.
      if (request.execution_generation) {
        const auto expected =
            current->current ? current->current->execution_generation.value : std::uint64_t{1};
        if (*request.execution_generation != expected) {
          return runtime_conflict_failure();
        }
      }
      auto expected_version = std::optional<ResourceVersion>{current->version};
      Fence fence;
      fence.instance = instance;
      fence.runtime_epoch = std::get<RuntimeEpoch>(epoch);
      if (current->current) {
        fence.execution = current->current->execution;
        fence.execution_generation = current->current->execution_generation;
        fence.connection_generation =
            ConnectionGeneration{current->current->connection_generation.value + 1};
      } else {
        auto execution = ids_.connection_id();
        if (const auto* failure = std::get_if<Failure>(&execution)) {
          return *failure;
        }
        fence.execution = ExecutionId{std::get<ConnectionId>(execution).value};
        fence.execution_generation = ExecutionGeneration{1};
        fence.connection_generation = ConnectionGeneration{1};
      }
      auto connection = ids_.connection_id();
      if (const auto* failure = std::get_if<Failure>(&connection)) {
        return *failure;
      }
      fence.connection = std::get<ConnectionId>(connection);
      current->current = fence;
      current->version.value += 1;
      const auto saved = work->instances().save(*current, expected_version);
      if (const auto* failure = std::get_if<Failure>(&saved)) {
        return *failure;
      }
      if (work->commit() != CommitResult::Committed) {
        return runtime_conflict_failure();
      }
      BridgeRegistrationResult bridge;
      bridge.assigned_instance_id = instance;
      bridge.heartbeat_interval_seconds = 30;
      bridge.required_api_version = std::string{kPluginApiVersion};
      bridge.desired_config_version = current->desired_version.value;
      bridge.granted_permissions = request.manifest.permissions;
      for (const auto& feature : request.supported_features) {
        if (std::find(supported_features_.begin(), supported_features_.end(), feature) !=
            supported_features_.end()) {
          bridge.enabled_features.push_back(feature);
        }
      }
      auto issued =
          issue_authorization(BridgePeerKind::Plugin, instance,
                              BridgeFence{instance, fence.runtime_epoch, fence.execution_generation,
                                          fence.connection_generation},
                              now, current->version.value);
      if (const auto* failure = std::get_if<Failure>(&issued)) {
        return *failure;
      }
      bridge.authorization = std::move(std::get<BridgeAuthorization>(issued));
      {
        std::lock_guard<std::mutex> lock(mutex_);
        PluginProfile profile;
        profile.ordered = request.manifest.ordered;
        // The authenticated bootstrap already resolved the instance; its installation is the
        // authoritative owner installation. The registration frame may omit installation_id.
        profile.installation = current->installation;
        profile.permissions = request.manifest.permissions;
        profile.enabled_features = bridge.enabled_features;
        for (const auto& capability : request.manifest.capabilities) {
          for (const auto payload :
               {EventPayloadKind::MessageReceived, EventPayloadKind::MessageSent,
                EventPayloadKind::GroupMemberChanged, EventPayloadKind::FriendRequest,
                EventPayloadKind::AdapterStatusChanged}) {
            if (capability == event_subscription(payload)) {
              profile.subscriptions.insert(std::string{event_subscription(payload)});
            }
          }
        }
        plugin_profiles_[instance.value] = std::move(profile);
      }
      return bridge;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<BridgeAuthorization> refresh(BridgePeerKind kind, const InstanceId& instance,
                                       const BridgeAuthorization& current,
                                       const BridgeRefreshRequest& request) override {
    const auto key = bridge_key(kind, instance);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = current_.find(key);
      if (found == current_.end() || !bridge_secret_equal(found->second.proof, request.proof) ||
          !bridge_secret_equal(found->second.refresh_credential, request.refresh_credential) ||
          found->second.refresh_generation != request.refresh_generation ||
          found->second.fence != request.fence || found->second.fence != current.fence ||
          !bridge_secret_equal(found->second.proof, current.proof)) {
        return unauthenticated();
      }
    }
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&sampled)) {
        return *failure;
      }
      const auto now = std::get<TimePoint>(sampled);
      if (now >= current.expires_at) {
        return unauthenticated();
      }
      auto active = work->bridge_credentials().active(instance.value);
      if (const auto* failure = std::get_if<Failure>(&active)) {
        return *failure;
      }
      if (!std::get<bool>(active)) {
        return unauthenticated();
      }
      std::uint64_t authorization_version = current.authorization_version;
      if (kind == BridgePeerKind::Adapter) {
        auto adapter = work->adapters().by_instance(instance);
        if (const auto* failure = std::get_if<Failure>(&adapter)) {
          return *failure;
        }
        const auto record = std::get<std::optional<AdapterRecord>>(adapter);
        if (!record || record->state != LifecycleState::Active) {
          return unauthenticated();
        }
        authorization_version = std::max(authorization_version, record->version.value);
      } else {
        auto instance_record = work->instances().find(instance);
        if (const auto* failure = std::get_if<Failure>(&instance_record)) {
          return *failure;
        }
        const auto record = std::get<std::optional<PluginInstance>>(instance_record);
        if (!record || record->state != LifecycleState::Active || record->host.has_value()) {
          return unauthenticated();
        }
        authorization_version = std::max(authorization_version, record->version.value);
      }
      auto proof = secrets_.opaque_secret();
      if (const auto* failure = std::get_if<Failure>(&proof)) {
        return *failure;
      }
      auto refresh_material = secrets_.opaque_secret();
      if (const auto* failure = std::get_if<Failure>(&refresh_material)) {
        return *failure;
      }
      BridgeAuthorization renewed;
      renewed.fence = current.fence;
      renewed.proof = std::get<std::array<std::byte, 32>>(proof);
      renewed.refresh_credential = std::get<std::array<std::byte, 32>>(refresh_material);
      renewed.refresh_generation = current.refresh_generation + 1;
      renewed.credential_version = current.credential_version;
      renewed.authorization_version = authorization_version;
      renewed.issued_at = now;
      renewed.expires_at = now + kBridgeAuthorizationLifetime;
      if (!valid_bridge_authorization(renewed, now)) {
        return internal_failure();
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        current_[key] = renewed;
      }
      return renewed;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Status authorize_frame(BridgePeerKind kind, const BridgeAuthorization& current,
                         const BridgeFence& fence, const std::array<std::byte, 32>& proof,
                         TimePoint now) override {
    if (!valid_bridge_authorization(current, now)) {
      return unauthenticated();
    }
    if (!bridge_frame_fence_matches(current, fence) || !bridge_secret_equal(current.proof, proof)) {
      return unauthenticated();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = current_.find(bridge_key(kind, current.fence.instance));
      if (found == current_.end() || found->second.fence != current.fence ||
          found->second.fence.connection_generation != current.fence.connection_generation ||
          !bridge_secret_equal(found->second.proof, current.proof)) {
        return unauthenticated();
      }
    }
    // Bounded local authority refresh: revocation/disable/invalidation becomes effective within
    // kBridgeAuthorizationGateInterval (<=1s) even when the peer keeps sending valid proofs.
    return authorization_gate(kind, current.fence.instance, now);
  }

  Status disconnect(BridgePeerKind kind, const InstanceId& instance,
                    ConnectionGeneration connection_generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = bridge_key(kind, instance);
    const auto found = current_.find(key);
    if (found != current_.end() &&
        found->second.fence.connection_generation == connection_generation) {
      current_.erase(found);
      gates_.erase(key);
    }
    const auto action_sink = action_sinks_.find(instance.value);
    if (action_sink != action_sinks_.end() &&
        action_sink->second.connection_generation.value == connection_generation.value) {
      action_sinks_.erase(action_sink);
    }
    return std::monostate{};
  }

  Outcome<TimePoint> current_time() override {
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      work->rollback();
      return sampled;
    } catch (...) {
      return unavailable();
    }
  }

  bool available() const override {
    return true;
  }

  Outcome<BridgeEventAck> ingest_event(const InstanceId& adapter_instance,
                                       const BridgeEventIngress& ingress) override {
    if (!canonical_id(ingress.account_id) || !canonical_id(ingress.adapter_id) ||
        ingress.event_id.empty()) {
      return invalid_runtime_input();
    }
    auto fingerprint = secrets_.sha256(ingress.serialized_envelope);
    if (const auto* failure = std::get_if<Failure>(&fingerprint)) {
      return *failure;
    }
    EventRecord event;
    event.key = EventKey{ResourceId{ingress.adapter_id}, ingress.event_id};
    event.fingerprint = std::get<Sha256Digest>(fingerprint);
    event.conversation =
        ConversationKey{ResourceId{ingress.adapter_id}, ResourceId{ingress.account_id},
                        ingress.conversation_id, ingress.conversation_type};
    event.payload = ingress.payload;
    event.has_conversation = ingress.has_conversation;
    event.has_sequence = ingress.sequence.has_value() && *ingress.sequence != 0;
    if (event.has_sequence) {
      event.sequence = ingress.sequence;
    }
    event.serialized_envelope = ingress.serialized_envelope;
    event.occurred_at = ingress.occurred_at;
    event.retain_until = ingress.occurred_at + std::chrono::hours{24 * 7};
    const auto subscription = event_subscription(ingress.payload);
    std::vector<DeliveryRecord> targets;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& [instance_value, profile] : plugin_profiles_) {
        if (subscription.empty() || profile.subscriptions.count(std::string{subscription}) == 0) {
          continue;
        }
        targets.push_back(DeliveryRecord{
            DeliveryKey{event.key, InstanceId{instance_value}}, profile.ordered,
            ingress.trace_id.empty() ? ingress.event_id : ingress.trace_id, 0, std::nullopt});
      }
    }
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      // Audit the envelope account against the authoritative A1 binding and registration: a stream
      // authenticated for one instance must never inject events for an unbound account.
      auto binding = work->adapter_bindings().find(ResourceId{ingress.account_id});
      if (const auto* failure = std::get_if<Failure>(&binding)) {
        return *failure;
      }
      const auto bound = std::get<std::optional<AdapterBindingRecord>>(binding);
      if (!bound || bound->state != LifecycleState::Active ||
          bound->adapter_instance != adapter_instance) {
        work->rollback();
        return permission_denied();
      }
      auto adapter = work->adapters().by_instance(adapter_instance);
      if (const auto* failure = std::get_if<Failure>(&adapter)) {
        return *failure;
      }
      const auto registered = std::get<std::optional<AdapterRecord>>(adapter);
      if (!registered || registered->state != LifecycleState::Active ||
          registered->current.instance != adapter_instance) {
        work->rollback();
        return permission_denied();
      }
      auto accepted = work->events().accept(event, targets, EventCapacityLimits{});
      if (const auto* rejected = std::get_if<Failure>(&accepted)) {
        work->rollback();
        BridgeEventAck ack;
        ack.status = BridgeAckStatus::Rejected;
        ack.failure = *rejected;
        if (rejected->code == ErrorCode::RateLimited ||
            rejected->code == ErrorCode::UpstreamUnavailable) {
          ack.retryable = true;
          ack.retry_after_ms = 1000;
          ack.close_stream = true;
        }
        return ack;
      }
      if (work->commit() != CommitResult::Committed) {
        return runtime_conflict_failure();
      }
      const auto& result = std::get<EventAcceptanceResult>(accepted);
      BridgeEventAck ack;
      if (result.duplicate) {
        ack.status = BridgeAckStatus::Duplicate;
      } else if (result.sequence.verdict == SequenceVerdict::OutOfOrder) {
        ack.status = BridgeAckStatus::Rejected;
        ack.failure = bridge_failure(ErrorCode::Conflict, Recovery::ReadCurrent);
        ack.retryable = true;
        ack.retry_after_ms = 1000;
        ack.close_stream = true;
      } else {
        ack.status = BridgeAckStatus::Persisted;
      }
      (void)adapter_instance;
      return ack;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Status attach_plugin_delivery(const InstanceId& instance,
                                ConnectionGeneration connection_generation,
                                const DeliverySink& sink) override {
    if (!sink || connection_generation.value == 0) {
      return invalid_runtime_input();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_[instance.value] = SinkEntry{sink, {}, connection_generation};
    return std::monostate{};
  }

  void detach_plugin_delivery(const InstanceId& instance,
                              ConnectionGeneration connection_generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = sinks_.find(instance.value);
    if (entry != sinks_.end() &&
        entry->second.connection_generation.value == connection_generation.value) {
      sinks_.erase(entry);
    }
    const auto key = bridge_key(BridgePeerKind::Plugin, instance);
    const auto refreshed = current_.find(key);
    if (refreshed != current_.end() &&
        refreshed->second.fence.connection_generation == connection_generation) {
      current_.erase(refreshed);
      plugin_profiles_.erase(instance.value);
      gates_.erase(key);
    }
  }

  Outcome<BridgePluginProfile> plugin_profile(const InstanceId& instance) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto profile = plugin_profiles_.find(instance.value);
    if (profile == plugin_profiles_.end()) {
      return unauthenticated();
    }
    BridgePluginProfile snapshot;
    snapshot.permissions = profile->second.permissions;
    snapshot.installation = profile->second.installation;
    snapshot.features = profile->second.enabled_features;
    snapshot.subscriptions.assign(profile->second.subscriptions.begin(),
                                  profile->second.subscriptions.end());
    return snapshot;
  }

  Outcome<ActionRecord> submit_plugin_action(const InstanceId& instance,
                                             const BridgeActionSubmit& request) override {
    if (!actions_) {
      return capability_not_supported();
    }
    std::vector<std::string> permissions;
    std::vector<std::string> features;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto profile = plugin_profiles_.find(instance.value);
      if (profile == plugin_profiles_.end()) {
        return unauthenticated();
      }
      permissions = profile->second.permissions;
      features = profile->second.enabled_features;
    }
    ActionSubmitRequest application_request;
    application_request.owner_instance = instance;
    application_request.request_id = request.request_id;
    application_request.trace_id = request.trace_id;
    application_request.owner_permissions = std::move(permissions);
    application_request.enabled_features = std::move(features);
    application_request.action_id = request.action_id;
    application_request.account_id = request.account_id;
    application_request.adapter_id = request.adapter_id;
    application_request.kind = request.kind;
    application_request.idempotency_key = request.idempotency_key;
    application_request.serialized_envelope = request.serialized_envelope;
    application_request.target_conversation = request.target_conversation;
    application_request.target_conversation_kind = request.target_conversation_kind;
    application_request.deadline = request.deadline;
    auto submitted = actions_->submit_action(application_request);
    if (const auto* failure = std::get_if<Failure>(&submitted)) {
      return *failure;
    }
    return std::get<ActionSubmitOutcome>(std::move(submitted)).record;
  }

  Outcome<ActionRecord> query_plugin_action(const InstanceId& instance,
                                            std::optional<ActionId> action_id,
                                            std::optional<std::string> idempotency_key) override {
    if (!actions_) {
      return capability_not_supported();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (plugin_profiles_.find(instance.value) == plugin_profiles_.end()) {
        return unauthenticated();
      }
    }
    ActionOwnerQuery query;
    query.owner_instance = instance;
    query.action_id = std::move(action_id);
    query.idempotency_key = std::move(idempotency_key);
    return actions_->query_owner_action(query);
  }

  Outcome<ActionRecord> cancel_plugin_action(const InstanceId& instance,
                                             const ActionId& action_id) override {
    if (!actions_) {
      return capability_not_supported();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (plugin_profiles_.find(instance.value) == plugin_profiles_.end()) {
        return unauthenticated();
      }
    }
    ActionCancelRequest request;
    request.owner_instance = instance;
    request.action_id = action_id;
    return actions_->cancel_action(request);
  }

  Outcome<ActionReportReceipt> record_adapter_result(const InstanceId& adapter_instance,
                                                     const BridgeActionReport& report) override {
    if (!actions_) {
      return capability_not_supported();
    }
    ActionResultReport application_report;
    application_report.adapter_instance = adapter_instance;
    application_report.action_id = report.action_id;
    application_report.dispatch_attempt_id = report.dispatch_attempt_id;
    application_report.result = report.result;
    application_report.trace_id = report.trace_id;
    return actions_->record_result(application_report);
  }

  Outcome<ActionReportReceipt>
  record_adapter_evidence(const InstanceId& adapter_instance,
                          const BridgeEvidenceReport& report) override {
    if (!actions_) {
      return capability_not_supported();
    }
    ActionAdapterEvidence application_report;
    application_report.adapter_instance = adapter_instance;
    application_report.action_id = report.action_id;
    application_report.dispatch_attempt_id = report.dispatch_attempt_id;
    application_report.execution_outcome = report.execution_outcome;
    application_report.error_code = report.error_code;
    application_report.platform_reference = report.platform_reference;
    application_report.observed_at = report.observed_at;
    application_report.trace_id = report.trace_id;
    return actions_->record_evidence(application_report);
  }

  Status attach_adapter_action_sink(const InstanceId& instance,
                                    ConnectionGeneration connection_generation,
                                    const ActionDispatchSink& sink) override {
    if (!sink || connection_generation.value == 0) {
      return invalid_runtime_input();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    action_sinks_[instance.value] = ActionSinkEntry{sink, connection_generation};
    return std::monostate{};
  }

  void detach_adapter_action_sink(const InstanceId& instance,
                                  ConnectionGeneration connection_generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = action_sinks_.find(instance.value);
    if (found != action_sinks_.end() &&
        found->second.connection_generation.value == connection_generation.value) {
      action_sinks_.erase(found);
    }
  }

  Status maintenance() override {
    struct Pending {
      InstanceId instance;
      std::vector<DeliveryClaim> claims;
    };
    try {
      std::vector<Pending> pending;
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return transient(*failure);
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&sampled)) {
        work->rollback();
        return transient(*failure);
      }
      const auto now = std::get<TimePoint>(sampled);
      auto entropy = secrets_.opaque_secret();
      if (const auto* failure = std::get_if<Failure>(&entropy)) {
        work->rollback();
        return transient(*failure);
      }
      const auto& bytes = std::get<std::array<std::byte, 32>>(entropy);
      const auto jitter = static_cast<double>(static_cast<unsigned char>(bytes[0])) / 255.0;
      const bool recover_now = !recovered_;
      const bool prune_now = (++maintenance_round_ % 60U) == 0;
      if (recover_now) {
        auto recovered = work->events().recover(now, RetryPolicy{}, jitter);
        if (const auto* failure = std::get_if<Failure>(&recovered)) {
          work->rollback();
          return transient(*failure);
        }
      }
      if (prune_now) {
        auto pruned = work->events().prune_expired(now, 100);
        if (const auto* failure = std::get_if<Failure>(&pruned)) {
          work->rollback();
          return transient(*failure);
        }
      }
      auto swept = work->events().sweep(now, RetryPolicy{}, jitter);
      if (const auto* failure = std::get_if<Failure>(&swept)) {
        work->rollback();
        return transient(*failure);
      }
      std::vector<InstanceId> connected;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [instance_value, entry] : sinks_) {
          (void)entry;
          connected.push_back(InstanceId{instance_value});
        }
      }
      for (const auto& instance : connected) {
        auto found = work->instances().find(instance);
        if (const auto* failure = std::get_if<Failure>(&found)) {
          work->rollback();
          return transient(*failure);
        }
        const auto record = std::get<std::optional<PluginInstance>>(found);
        if (!record || !record->current || record->state != LifecycleState::Active) {
          continue;
        }
        DeliveryClaimRequest request;
        request.instance = instance;
        request.fence = *record->current;
        request.limit = 4;
        request.lease = kDeliveryLease;
        request.now = now;
        for (std::size_t index = 0; index < request.limit; ++index) {
          auto id = ids_.snapshot_id();
          if (const auto* failure = std::get_if<Failure>(&id)) {
            work->rollback();
            return transient(*failure);
          }
          request.attempt_ids.push_back(AttemptId{std::get<std::string>(id)});
        }
        auto claimed = work->events().claim(request);
        if (const auto* failure = std::get_if<Failure>(&claimed)) {
          work->rollback();
          return transient(*failure);
        }
        auto claims = std::move(std::get<std::vector<DeliveryClaim>>(claimed));
        if (!claims.empty()) {
          pending.push_back(Pending{instance, std::move(claims)});
        }
      }
      if (work->commit() != CommitResult::Committed) {
        return transient(runtime_conflict_failure());
      }
      if (recover_now) {
        recovered_ = true;
      }
      for (auto& group : pending) {
        DeliverySink sink;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          const auto entry = sinks_.find(group.instance.value);
          if (entry == sinks_.end()) {
            continue;
          }
          if (entry->second.connection_generation.value !=
              group.claims.front().attempt.fence.connection_generation.value) {
            continue;
          }
          for (const auto& claim : group.claims) {
            sinks_[group.instance.value].attempts[claim.attempt.attempt.value] =
                claim.attempt.delivery;
          }
          sink = entry->second.sink;
        }
        for (const auto& claim : group.claims) {
          (void)sink(claim.event, claim.attempt, claim.trace_id);
        }
      }
      if (actions_ == nullptr) {
        return std::monostate{};
      }
      std::vector<std::pair<InstanceId, ConnectionGeneration>> adapter_instances;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        adapter_instances.reserve(action_sinks_.size());
        for (const auto& [instance_value, entry] : action_sinks_) {
          adapter_instances.emplace_back(InstanceId{instance_value}, entry.connection_generation);
        }
      }
      for (const auto& [instance, generation] : adapter_instances) {
        auto claimed = actions_->claim_due(instance, 4, now);
        if (const auto* failure = std::get_if<Failure>(&claimed)) {
          return transient(*failure);
        }
        ActionDispatchSink sink;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          const auto entry = action_sinks_.find(instance.value);
          if (entry == action_sinks_.end() ||
              entry->second.connection_generation.value != generation.value) {
            continue;
          }
          sink = entry->second.sink;
        }
        for (const auto& frame : std::get<ActionDispatchClaim>(claimed).frames) {
          auto pushed = sink(frame);
        }
      }
      return std::monostate{};
    } catch (const std::bad_alloc&) {
      return transient(unavailable());
    } catch (...) {
      return internal_failure();
    }
  }

  Status attach_shutdown_sink(BridgePeerKind kind, const InstanceId& instance,
                              ConnectionGeneration connection_generation,
                              const ShutdownSink& sink) override {
    if (!sink || connection_generation.value == 0) {
      return invalid_runtime_input();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_sinks_[bridge_key(kind, instance)] = ShutdownEntry{connection_generation, sink};
    return std::monostate{};
  }

  void detach_shutdown_sink(BridgePeerKind kind, const InstanceId& instance,
                            ConnectionGeneration connection_generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = bridge_key(kind, instance);
    const auto entry = shutdown_sinks_.find(key);
    if (entry != shutdown_sinks_.end() &&
        entry->second.connection_generation.value == connection_generation.value) {
      shutdown_sinks_.erase(entry);
    }
  }

  Status shutdown_notice() override {
    std::vector<ShutdownSink> sinks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sinks.reserve(shutdown_sinks_.size());
      for (const auto& [key, entry] : shutdown_sinks_) {
        (void)key;
        sinks.push_back(entry.sink);
      }
    }
    Status result = std::monostate{};
    for (const auto& sink : sinks) {
      const auto sent = sink();
      if (const auto* failure = std::get_if<Failure>(&sent)) {
        result = *failure;
      }
    }
    return result;
  }

  Outcome<DeliveryReceipt> acknowledge_delivery(const InstanceId& instance,
                                                const BridgeAttemptRef& supplied, bool accepted,
                                                std::optional<Failure> failure,
                                                bool retryable) override {
    DeliveryAcknowledgement acknowledgement;
    acknowledgement.accepted = accepted;
    acknowledgement.failure = std::move(failure);
    acknowledgement.retryable = retryable;
    return settle_delivery(instance, supplied, acknowledgement);
  }

  Outcome<DeliveryReceipt> renew_delivery(const InstanceId& instance,
                                          const BridgeAttemptRef& supplied) override {
    return settle_delivery(instance, supplied, std::nullopt);
  }

private:
  Status authorization_gate(BridgePeerKind kind, const InstanceId& instance, TimePoint now) {
    const auto key = bridge_key(kind, instance);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto cached = gates_.find(key);
      if (cached != gates_.end() && now >= cached->second.checked &&
          now - cached->second.checked < kBridgeAuthorizationGateInterval) {
        return cached->second.allowed ? Status{std::monostate{}} : permission_denied();
      }
    }
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      bool allowed = false;
      auto active = work->bridge_credentials().active(instance.value);
      if (const auto* failure = std::get_if<Failure>(&active)) {
        return *failure;
      }
      if (std::get<bool>(active)) {
        if (kind == BridgePeerKind::Adapter) {
          auto adapter = work->adapters().by_instance(instance);
          if (const auto* failure = std::get_if<Failure>(&adapter)) {
            return *failure;
          }
          const auto record = std::get<std::optional<AdapterRecord>>(adapter);
          auto bound = work->active_bindings(instance);
          if (const auto* failure = std::get_if<Failure>(&bound)) {
            return *failure;
          }
          allowed = record && record->state == LifecycleState::Active &&
                    record->current.instance == instance && std::get<std::size_t>(bound) > 0;
        } else {
          auto found = work->instances().find(instance);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          const auto record = std::get<std::optional<PluginInstance>>(found);
          allowed = record && record->state == LifecycleState::Active && !record->host.has_value();
        }
      }
      work->rollback();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        gates_[key] = GateEntry{now, allowed};
      }
      return allowed ? Status{std::monostate{}} : permission_denied();
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  static Status transient(const Failure& failure) {
    if (failure.code == ErrorCode::Conflict || failure.code == ErrorCode::UpstreamUnavailable ||
        failure.code == ErrorCode::Timeout) {
      return std::monostate{};
    }
    return failure;
  }

  DeliveryReceipt stale_receipt(const BridgeAttemptRef& supplied) {
    DeliveryReceipt receipt;
    receipt.attempt.attempt = supplied.attempt;
    receipt.attempt.fence.instance = supplied.fence.instance;
    receipt.attempt.fence.runtime_epoch = supplied.fence.runtime_epoch;
    receipt.attempt.fence.execution_generation = supplied.fence.execution_generation;
    receipt.attempt.fence.connection_generation = supplied.fence.connection_generation;
    receipt.attempt.started_at = TimePoint{};
    receipt.attempt.lease_until = TimePoint{};
    receipt.disposition = DeliveryDisposition::Stale;
    receipt.failure = bridge_failure(ErrorCode::Conflict, Recovery::ReadCurrent);
    receipt.observed_at = TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
    return receipt;
  }

  Outcome<DeliveryReceipt> settle_delivery(const InstanceId& instance,
                                           const BridgeAttemptRef& supplied,
                                           std::optional<DeliveryAcknowledgement> acknowledgement) {
    DeliveryKey delivery;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto entry = sinks_.find(instance.value);
      if (entry == sinks_.end()) {
        return stale_receipt(supplied);
      }
      const auto attempt = entry->second.attempts.find(supplied.attempt.value);
      if (attempt == entry->second.attempts.end()) {
        return stale_receipt(supplied);
      }
      delivery = attempt->second;
    }
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&sampled)) {
        return *failure;
      }
      const auto now = std::get<TimePoint>(sampled);
      auto found = work->instances().find(instance);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      const auto record = std::get<std::optional<PluginInstance>>(found);
      if (!record || !record->current || record->state != LifecycleState::Active ||
          record->current->runtime_epoch.value != supplied.fence.runtime_epoch.value ||
          record->current->execution_generation.value !=
              supplied.fence.execution_generation.value ||
          record->current->connection_generation.value !=
              supplied.fence.connection_generation.value) {
        return stale_receipt(supplied);
      }
      DeliveryAttempt attempt;
      attempt.delivery = delivery;
      attempt.attempt = supplied.attempt;
      attempt.fence = *record->current;
      attempt.started_at = now;
      attempt.lease_until = now;
      Outcome<DeliveryReceipt> receipt = stale_receipt(supplied);
      if (acknowledgement) {
        acknowledgement->attempt = attempt;
        receipt = work->events().acknowledge(*acknowledgement, now);
      } else {
        receipt = work->events().renew(attempt, now);
      }
      if (const auto* failure = std::get_if<Failure>(&receipt)) {
        work->rollback();
        return *failure;
      }
      if (work->commit() != CommitResult::Committed) {
        return runtime_conflict_failure();
      }
      if (acknowledgement) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = sinks_.find(instance.value);
        if (entry != sinks_.end()) {
          entry->second.attempts.erase(supplied.attempt.value);
        }
      }
      return receipt;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ResolvedBootstrap> resolve_bootstrap(BridgePeerKind kind, std::string_view bearer) {
    if (bearer.size() < 32 || bearer.size() > 4096 ||
        !std::all_of(bearer.begin(), bearer.end(),
                     [](unsigned char character) { return character >= 33 && character <= 126; })) {
      return unauthenticated();
    }
    auto digest = secrets_.sha256(bearer);
    if (const auto* failure = std::get_if<Failure>(&digest)) {
      return *failure;
    }
    try {
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&sampled)) {
        return *failure;
      }
      const auto now = std::get<TimePoint>(sampled);
      auto record = work->bridge_credentials().by_digest(std::get<Sha256Digest>(digest));
      if (const auto* failure = std::get_if<Failure>(&record)) {
        return *failure;
      }
      const auto credential = std::move(std::get<std::optional<BridgeCredentialRecord>>(record));
      if (!credential || credential->revoked || credential->resource_id.empty()) {
        return unauthenticated();
      }
      if (kind == BridgePeerKind::Adapter) {
        auto bound = work->active_bindings(InstanceId{credential->resource_id});
        if (const auto* failure = std::get_if<Failure>(&bound)) {
          return *failure;
        }
        if (std::get<std::size_t>(bound) == 0) {
          return permission_denied();
        }
      } else {
        auto instance_record = work->instances().find(InstanceId{credential->resource_id});
        if (const auto* failure = std::get_if<Failure>(&instance_record)) {
          return *failure;
        }
        const auto record_instance = std::get<std::optional<PluginInstance>>(instance_record);
        if (!record_instance || record_instance->state != LifecycleState::Active ||
            record_instance->host.has_value()) {
          return permission_denied();
        }
      }
      work->rollback();
      return ResolvedBootstrap{*credential, InstanceId{credential->resource_id}, now};
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<BridgeAuthorization> issue_authorization(BridgePeerKind kind, const InstanceId& instance,
                                                   BridgeFence fence, TimePoint issued_at,
                                                   std::uint64_t authorization_version) {
    auto proof = secrets_.opaque_secret();
    if (const auto* failure = std::get_if<Failure>(&proof)) {
      return *failure;
    }
    auto refresh = secrets_.opaque_secret();
    if (const auto* failure = std::get_if<Failure>(&refresh)) {
      return *failure;
    }
    BridgeAuthorization authorization;
    authorization.fence = fence;
    authorization.proof = std::get<std::array<std::byte, 32>>(proof);
    authorization.refresh_credential = std::get<std::array<std::byte, 32>>(refresh);
    authorization.refresh_generation = 1;
    authorization.credential_version = kBridgeCredentialVersion;
    authorization.authorization_version = std::max<std::uint64_t>(1, authorization_version);
    authorization.issued_at = issued_at;
    authorization.expires_at = issued_at + kBridgeAuthorizationLifetime;
    if (!valid_bridge_authorization(authorization, issued_at)) {
      return internal_failure();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_[bridge_key(kind, instance)] = authorization;
    }
    return authorization;
  }

  Outcome<RuntimeEpoch> process_epoch() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!epoch_) {
      auto sampled = ids_.runtime_epoch();
      if (const auto* failure = std::get_if<Failure>(&sampled)) {
        return *failure;
      }
      epoch_ = std::get<RuntimeEpoch>(sampled);
    }
    return *epoch_;
  }

  struct PluginProfile {
    bool ordered = false;
    std::vector<std::string> permissions;
    std::vector<std::string> enabled_features;
    std::set<std::string> subscriptions;
    std::optional<InstallationId> installation;
  };
  struct ActionSinkEntry {
    ActionDispatchSink sink;
    ConnectionGeneration connection_generation;
  };
  struct SinkEntry {
    DeliverySink sink;
    std::map<std::string, DeliveryKey> attempts;
    ConnectionGeneration connection_generation;
  };
  struct ShutdownEntry {
    ConnectionGeneration connection_generation;
    ShutdownSink sink;
  };
  struct GateEntry {
    TimePoint checked;
    bool allowed = false;
  };

  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  RuntimeResourceApplication& resources_;
  RuntimeActionApplication* actions_;
  std::vector<std::string> supported_features_;
  mutable std::mutex mutex_;
  std::map<std::string, BridgeAuthorization> current_;
  std::map<std::string, PluginProfile> plugin_profiles_;
  std::map<std::string, ShutdownEntry> shutdown_sinks_;
  std::map<std::string, ActionSinkEntry> action_sinks_;
  std::map<std::string, SinkEntry> sinks_;
  std::map<std::string, GateEntry> gates_;
  bool recovered_ = false;
  std::uint64_t maintenance_round_ = 0;
  std::optional<RuntimeEpoch> epoch_;
};

} // namespace

Outcome<std::unique_ptr<RuntimeBridgeApplication>>
make_runtime_bridge_application(RuntimeBridgeDependencies dependencies) {
  try {
    return std::unique_ptr<RuntimeBridgeApplication>(
        std::make_unique<RuntimeBridgeApplicationImpl>(std::move(dependencies)));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace kiarabot::core::target
