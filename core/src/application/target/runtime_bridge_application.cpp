#include "kiarabot/core/application/target/runtime_bridge_application.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/runtime_rules.hpp"

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
        supported_features_(std::move(dependencies.supported_features)) {}

  Outcome<BridgeRegistrationResult>
  register_adapter(const BridgeAdapterRegistration& request) override {
    if (request.request_id.empty() || request.trace_id.empty() || request.adapter.value.empty() ||
        !request.bearer_credential.size()) {
      return invalid_runtime_input();
    }
    try {
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
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = current_.find(bridge_key(kind, current.fence.instance));
    if (found == current_.end() || found->second.fence != current.fence ||
        found->second.fence.connection_generation != current.fence.connection_generation ||
        !bridge_secret_equal(found->second.proof, current.proof)) {
      return unauthenticated();
    }
    return std::monostate{};
  }

  Status disconnect(BridgePeerKind kind, const InstanceId& instance,
                    ConnectionGeneration connection_generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = bridge_key(kind, instance);
    const auto found = current_.find(key);
    if (found != current_.end() &&
        found->second.fence.connection_generation == connection_generation) {
      current_.erase(found);
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

private:
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

  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  RuntimeResourceApplication& resources_;
  std::vector<std::string> supported_features_;
  mutable std::mutex mutex_;
  std::map<std::string, BridgeAuthorization> current_;
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
