#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/invariants.hpp"
#include "kiarabot/core/domain/target/runtime_records.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"

namespace kiarabot::core::target {

inline Failure invalid_runtime_input() {
  return {ErrorCode::InvalidArgument, Acceptance::NotAccepted, Recovery::CorrectInput, {}, {}};
}
inline Failure runtime_conflict() {
  return {ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::ReadCurrent, {}, {}};
}
inline Failure runtime_not_found() {
  return {ErrorCode::NotFound, Acceptance::NotAccepted, Recovery::ReadCurrent, {}, {}};
}

// Canonical lowercase UUID text. The trusted boundary validates the same rule before dispatch;
// domain repeats it so a direct application caller cannot bypass id validation.
inline bool canonical_id(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
inline bool bounded_text(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char character) { return character < 32 || character == 127; });
}
inline bool platform_text(std::string_view value) {
  if (!bounded_text(value, 1, kMaximumPlatformText)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-' || character == ':';
  });
}
inline bool platform_account_text(std::string_view value) {
  if (!bounded_text(value, 1, kMaximumPlatformAccountId)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == ':' || character == '@' || character == '-';
  });
}
inline bool positive(ResourceVersion version) {
  return version.value > 0;
}

// Account create: the boundary supplies the owner-allocated id; the domain rejects malformed
// platform references before the create-only repository call.
inline Status validate_account_create(const ResourceId& id, std::string_view platform,
                                      std::string_view platform_account_id,
                                      std::string_view display_name) {
  if (!canonical_id(id.value) || !platform_text(platform) ||
      !platform_account_text(platform_account_id) ||
      !bounded_text(display_name, 1, kMaximumDisplayName)) {
    return invalid_runtime_input();
  }
  return std::monostate{};
}

// Account update only changes the display name; a disabled/deleting account is not editable and
// an omitted expected version is rejected because update requires a conditional version.
inline Status validate_account_update(const BotAccount& current, std::string_view display_name,
                                      std::optional<ResourceVersion> expected) {
  if (current.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (!bounded_text(display_name, 1, kMaximumDisplayName) || !expected || !positive(*expected)) {
    return invalid_runtime_input();
  }
  const auto next = next_version(current.version, *expected);
  if (const auto* failure = std::get_if<Failure>(&next)) {
    return *failure;
  }
  return std::monostate{};
}

// Disable is terminal for the first P05 scope: the account stays addressable as a tombstone, active
// bindings must be removed first, and a missing expected version is rejected.
inline Status validate_account_disable(const BotAccount& current,
                                       std::optional<ResourceVersion> expected,
                                       bool has_active_binding) {
  if (current.state != LifecycleState::Active || has_active_binding) {
    return runtime_conflict();
  }
  if (!expected || !positive(*expected)) {
    return invalid_runtime_input();
  }
  const auto next = next_version(current.version, *expected);
  if (const auto* failure = std::get_if<Failure>(&next)) {
    return *failure;
  }
  return std::monostate{};
}

// Bind an active account to the currently registered adapter instance. Declared capabilities never
// authorize this operation; state and current fence do.
inline Status validate_bind(const BotAccount& account, const AdapterRecord& adapter,
                            std::optional<AdapterBindingRecord> existing) {
  if (account.state != LifecycleState::Active || adapter.state != LifecycleState::Active ||
      !valid_adapter_fence(adapter.current)) {
    return runtime_conflict();
  }
  if (existing && existing->state == LifecycleState::Active) {
    return runtime_conflict();
  }
  return std::monostate{};
}

inline Status validate_unbind(const AdapterBindingRecord& binding,
                              std::optional<ResourceVersion> expected) {
  if (binding.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (!expected || !positive(*expected)) {
    return invalid_runtime_input();
  }
  const auto next = next_version(binding.version, *expected);
  if (const auto* failure = std::get_if<Failure>(&next)) {
    return *failure;
  }
  return std::monostate{};
}

// Closed adapter capability vocabulary. Unknown names are CAPABILITY_NOT_SUPPORTED and unknown
// versions are INVALID_ARGUMENT; the negotiated result keeps declaration order and de-duplicates
// exact name/version pairs. Effective capabilities are negotiation facts only, never permissions.
inline constexpr std::array<std::string_view, 6> kAdapterCapabilityVocabulary{
    "action.send_message.text.v1", "action.react_message.v1", "action.delete_message.v1",
    "action.mute_member.v1",       "action.kick_member.v1",   "action.approve_request.v1"};

inline bool known_adapter_capability(std::string_view name) {
  return std::find(kAdapterCapabilityVocabulary.begin(), kAdapterCapabilityVocabulary.end(),
                   name) != kAdapterCapabilityVocabulary.end();
}

inline Status validate_declared_capabilities(const std::vector<Capability>& declared) {
  if (declared.size() > kMaximumCapabilityCount) {
    return invalid_runtime_input();
  }
  std::vector<Capability> seen;
  for (const auto& capability : declared) {
    if (!bounded_text(capability.name, 1, 48) || !bounded_text(capability.version, 1, 32)) {
      return invalid_runtime_input();
    }
    if (!known_adapter_capability(capability.name)) {
      return Failure{
          ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
    }
    if (std::find(seen.begin(), seen.end(), capability) != seen.end()) {
      return invalid_runtime_input();
    }
    seen.push_back(capability);
  }
  return std::monostate{};
}

// Negotiation intersects the declared set with the trusted supported set configured by the
// composition root. Contract-vocabulary names are validated separately; a known capability this
// Runtime build does not support is simply absent from the effective set.
inline Outcome<std::vector<Capability>>
negotiate_capabilities(const std::vector<Capability>& declared,
                       std::span<const std::string> supported) {
  const auto declared_status = validate_declared_capabilities(declared);
  if (const auto* failure = std::get_if<Failure>(&declared_status)) {
    return *failure;
  }
  std::vector<Capability> effective;
  for (const auto& capability : declared) {
    if (std::find(supported.begin(), supported.end(), capability.name) != supported.end()) {
      effective.push_back(capability);
    }
  }
  return effective;
}

struct AdapterRegistrationRequest {
  ResourceId adapter;
  AdapterRegistrationMetadata metadata;
  std::optional<InstanceId> presented_instance;
  RuntimeEpoch runtime_epoch;
  InstanceId allocated_instance;
  ConnectionId allocated_connection;
  TimePoint now;
};

inline bool valid_registration_metadata(const AdapterRegistrationMetadata& metadata) {
  return bounded_text(metadata.implementation, 1, kMaximumImplementationText) &&
         bounded_text(metadata.implementation_version, 1, kMaximumImplementationText) &&
         metadata.protocol_version == kRuntimeProtocolVersion;
}

// Accept a registration and advance the current accepted protocol connection. A reconnect that
// presents the current instance keeps the logical instance identity and bumps both generations; an
// absent presented id is only valid for the first accepted registration of the adapter. A presented
// stale/other instance is rejected instead of being silently replaced.
inline Outcome<AdapterRecord>
accept_adapter_registration(std::optional<AdapterRecord> current,
                            const AdapterRegistrationRequest& request) {
  if (!canonical_id(request.adapter.value) || !valid_registration_metadata(request.metadata) ||
      request.runtime_epoch.value.empty() || request.allocated_connection.value.empty() ||
      request.allocated_instance.value.empty() || request.now.time_since_epoch().count() <= 0) {
    return invalid_runtime_input();
  }
  const auto declared_status =
      validate_declared_capabilities(request.metadata.declared_capabilities);
  if (const auto* failure = std::get_if<Failure>(&declared_status)) {
    return *failure;
  }
  if (current && current->state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (current && !valid_adapter_fence(current->current)) {
    return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
  }
  if (current) {
    if (current->adapter != request.adapter) {
      return runtime_conflict();
    }
    if (!request.presented_instance || *request.presented_instance != current->current.instance) {
      return runtime_conflict();
    }
  } else if (request.presented_instance) {
    return runtime_not_found();
  }
  AdapterRecord next;
  next.adapter = request.adapter;
  next.registration = request.metadata;
  next.current.instance = current ? current->current.instance : request.allocated_instance;
  next.current.runtime_epoch = request.runtime_epoch;
  next.current.connection = request.allocated_connection;
  // Only the connection generation advances on an accepted (re)registration; an adapter has no
  // trusted process execution identity in P05, so the adapter Fence carries no execution value.
  next.current.connection_generation =
      ConnectionGeneration{current ? current->current.connection_generation.value + 1 : 1};
  next.version = ResourceVersion{current ? current->version.value + 1 : 1};
  next.last_registered_at = request.now;
  next.last_heartbeat_at = request.now;
  next.online_until = request.now + std::chrono::seconds{90};
  if (!valid_adapter_fence(next.current)) {
    return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
  }
  return next;
}

// Heartbeat authority is the authenticated connection plus the current fence; an offline window
// never extends itself and a stale generation never writes to the new connection.
inline Status validate_heartbeat(const AdapterRecord& current, const AdapterFenceClaim& supplied,
                                 TimePoint now) {
  if (current.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  // The wire claim has no connection id. Matching the stable instance, runtime epoch and the
  // positive connection generation binds the frame to the current accepted connection; a stale
  // generation or a pre-restart epoch is rejected instead of being mapped to the new connection.
  if (!valid_adapter_fence(current.current)) {
    return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
  }
  if (supplied.instance != current.current.instance ||
      supplied.runtime_epoch != current.current.runtime_epoch ||
      supplied.connection_generation != current.current.connection_generation) {
    if (supplied.instance != current.current.instance ||
        supplied.runtime_epoch != current.current.runtime_epoch) {
      return Failure{
          ErrorCode::Unauthenticated, Acceptance::NotAccepted, Recovery::Authenticate, {}, {}};
    }
    return runtime_conflict();
  }
  if (now < current.last_registered_at) {
    return invalid_runtime_input();
  }
  return std::monostate{};
}

inline Outcome<AdapterRecord> accept_heartbeat(AdapterRecord current, TimePoint now) {
  if (!valid_adapter_fence(current.current) || now < current.last_heartbeat_at) {
    return Failure{ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
  }
  current.last_heartbeat_at = now;
  current.online_until = now + std::chrono::seconds{90};
  current.version.value += 1;
  return current;
}

inline bool adapter_online(const AdapterRecord& record, TimePoint now) {
  return record.state == LifecycleState::Active && record.online_until > now;
}

// A plugin instance under one installation; installation state gates creation of new instances.
inline Status validate_instance_create(const InstanceId& id, const PluginInstallation& installation,
                                       std::optional<ResourceId> host, bool externally_managed) {
  if (installation.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (!canonical_id(id.value)) {
    return invalid_runtime_input();
  }
  if (externally_managed && host) {
    return invalid_runtime_input();
  }
  if (!externally_managed && (!host || !canonical_id(host->value))) {
    return invalid_runtime_input();
  }
  return std::monostate{};
}

// Desired state is management intent; it is accepted with a conditional version and never claims
// the process actually started or stopped.
inline Outcome<PluginInstance> advance_desired_state(PluginInstance current, DesiredState desired,
                                                     std::optional<ResourceVersion> expected) {
  if (current.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (!expected || !positive(*expected)) {
    return invalid_runtime_input();
  }
  const auto next = next_version(current.version, *expected);
  if (const auto* failure = std::get_if<Failure>(&next)) {
    return *failure;
  }
  current.desired = desired;
  current.version = std::get<ResourceVersion>(next);
  return current;
}

// Config save only advances the desired config version; applied state is a later plugin ACK fact.
inline Outcome<PluginInstance> save_desired_config(PluginInstance current,
                                                   std::optional<ResourceVersion> expected) {
  if (current.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (!expected || !positive(*expected)) {
    return invalid_runtime_input();
  }
  const auto next = next_version(current.version, *expected);
  if (const auto* failure = std::get_if<Failure>(&next)) {
    return *failure;
  }
  current.desired_version.value += 1;
  current.version = std::get<ResourceVersion>(next);
  return current;
}

inline Status validate_config_update(const PluginInstance& instance, std::size_t values_size) {
  if (instance.state != LifecycleState::Active) {
    return runtime_conflict();
  }
  if (values_size == 0 || values_size > 64 * 1024) {
    return invalid_runtime_input();
  }
  return std::monostate{};
}

} // namespace kiarabot::core::target
