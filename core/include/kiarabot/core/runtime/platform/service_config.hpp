#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::core::platform {

// Intersection of the actual OpenSSL-selected verified leaf/intermediate/anchor chain.
// Inclusive not-before, exclusive not-after; never a relative or inferred lifetime.
struct TlsValidity {
  target::TimePoint not_before;
  target::TimePoint not_after;
};
inline bool tls_valid_at(const TlsValidity& validity, target::TimePoint now) noexcept {
  return validity.not_before < validity.not_after && now >= validity.not_before &&
         now < validity.not_after;
}
struct TlsMaterial {
  std::string certificate;
  std::string private_key;
  std::string ca;
  std::optional<TlsValidity> validity = std::nullopt;
};
// Outer composition input only. Password is loaded from an owner-only file, never JSON/argv.
// Connection policy (TLS verification, channel binding, timeouts) remains in the PG adapter.
struct DatabaseMaterial {
  std::string host;
  std::uint16_t port;
  std::string name;
  std::string migration_role;
  std::string runtime_role;
  std::string password;
  std::string ca_file;
};
struct ControlApiMaterial {
  std::string listen;
  std::string identity_endpoint;
  // P12c: optional owner mTLS endpoints used by Control REST aggregation/forwarding. Absent
  // endpoints keep the corresponding owner-backed routes stable-failing (never fabricated data).
  std::optional<std::string> runtime_endpoint;
  std::optional<std::string> scheduler_endpoint;
  std::optional<std::string> model_endpoint;
  std::optional<std::string> host_endpoint;
};
// Trusted file references only; mature crypto loaders own secret allocation and key validation.
struct IdentitySymmetricKeyFile {
  std::string id;
  std::string file;
};
struct IdentityDelegationKeyFiles {
  std::string id;
  std::string public_key_file;
  std::string private_key_file;
};
struct IdentityClockDeclaration {
  bool trusted = false;
  std::uint32_t maximum_known_skew_ms = 0;
};
struct IdentityVerificationDeclaration {
  std::string catalog_id;
  std::uint64_t version;
  std::uint64_t key_version;
  std::int64_t valid_until_epoch_seconds;
};
struct IdentitySecurityMaterial {
  IdentitySymmetricKeyFile token_key;
  IdentitySymmetricKeyFile request_key;
  IdentitySymmetricKeyFile receipt_key;
  IdentityDelegationKeyFiles delegation_key;
  std::string issuer;
  IdentityClockDeclaration clock;
  IdentityVerificationDeclaration verification;
};
// Model provider-secret AEAD root key. Deployment-injected file + monotonic key version; the
// material is read by the composition root and never stored in PostgreSQL or logs.
struct ModelSecurityMaterial {
  IdentitySymmetricKeyFile root_key;
  std::uint64_t key_version = 0;
};
// Host deployment material (P08). Only file references/allowlists; secret values are read from
// owner-only deployment files by the composition root and never appear in JSON/argv.
struct HostPackageMaterial {
  std::string plugin_type_id;
  std::string root;
  std::string sha256;
  std::string launch_profile;
  std::vector<std::string> allowed_argument_names;
};
struct HostLaunchProfileMaterial {
  std::string name;
  std::string executable;
  std::vector<std::string> fixed_arguments;
  std::vector<std::string> allowed_environment_names;
  std::string credential_environment;
};
// Runtime-only deployment allowlist for HostBridge internal-mTLS peers: exactly one
// lowercase SHA-256 of the deployment-issued client certificate maps to one host_id.
struct HostPeerMaterial {
  std::string host_id;
  std::string certificate_sha256;
};
// P13h-1 Scheduler-only deployment authorization for the Runtime platform peer. Identity
// platform_peers remains the registration/rotation source; this explicit instance-level allowlist
// is required before ListTasks/CancelTask are exposed to the Runtime peer.
struct SchedulerRuntimePeerMaterial {
  std::string certificate_sha256;
  std::string instance_id;
  std::string installation_id;
  std::string account_id;
  std::vector<std::string> permissions;
};
struct HostMaterial {
  std::string host_id;
  std::string sqlite_path;
  std::string runtime_endpoint;
  std::string credential_directory;
  std::vector<HostPackageMaterial> packages;
  std::vector<HostLaunchProfileMaterial> profiles;
  std::vector<std::pair<std::string, std::string>> environment;
};
struct ProcessConfig {
  target::Service service;
  std::string probe_listen;
  std::string grpc_listen;
  TlsMaterial tls;
  std::optional<DatabaseMaterial> database;
  std::optional<ControlApiMaterial> control_api = std::nullopt;
  std::optional<IdentitySecurityMaterial> identity_security = std::nullopt;
  std::optional<ModelSecurityMaterial> model_security = std::nullopt;
  // Runtime/Scheduler: Identity owner gRPC endpoint for online authorization and delegation checks.
  // The validated service TLS material is reused as the outbound mTLS client identity.
  std::optional<std::string> identity_endpoint = std::nullopt;
  // Runtime-only: Model owner mTLS endpoint used for plugin host_request.model_chat ingress.
  std::optional<std::string> model_endpoint = std::nullopt;
  // Runtime-only P10k/P13h-2: Scheduler owner mTLS endpoint for plugin task frames and the
  // lifecycle participant's platform-identity ListTasks/CancelTask. Absent keeps both paths
  // fail-closed (CapabilityNotSupported/UPSTREAM_UNAVAILABLE, never fabricated results).
  std::optional<std::string> scheduler_endpoint = std::nullopt;
  // P05b-5/B1: Runtime-only sealed short-lived instance-parent delegation file. The Control/
  // deployment refresher owns writes; Runtime validates 0600 regular-file/JSON/expiry/instance/
  // permission and exchanges the token, never caches it beyond expiry. Absent keeps fail-closed.
  std::optional<std::string> instance_delegation_sealed_file = std::nullopt;
  // Scheduler-only: Runtime internal mTLS endpoint used to deliver TimerTriggered handoffs.
  std::optional<std::string> runtime_endpoint = std::nullopt;
  // Host-only: deployment identity, package/profile allowlists and local journal path.
  std::optional<HostMaterial> host = std::nullopt;
  // Runtime-only, optional: external Adapter/Plugin bridge listener. Server-verified TLS only (no
  // client certificate request), same certificate material as grpc_listen. Absent means no external
  // bridge listener; disabling the listener never falls back to the internal mTLS owner listener.
  std::optional<std::string> bridge_listen = std::nullopt;
  // Runtime-only P08 HostBridge certificate allowlist. Empty means no Host may register
  // (fail closed); entries are deployment configuration, never wire input.
  std::vector<HostPeerMaterial> hosts;
  // Scheduler-only: Runtime platform-peer instance grants for ListTasks/CancelTask. Empty keeps
  // the platform path fail-closed; Control/user delegation callers are unaffected.
  std::vector<SchedulerRuntimePeerMaterial> runtime_peers;
};
struct ConfigFailure {
  int exit_code;
  // Static reason only; never exception/config/PEM text.
  std::string_view reason;
};
using ConfigResult = std::variant<ProcessConfig, ConfigFailure>;
std::string_view service_name(target::Service service) noexcept;
// Strict schema v1. Regular files only, <=1MiB each; private key requires owner-only permissions.
// Reads and validates TLS chain/time/key before any listener. Failure never contains file content.
// Validation time is injectable for exact certificate-boundary tests; production uses current UTC.
ConfigResult load_service_config(const std::string& path, target::Service service,
                                 target::TimePoint now = std::chrono::system_clock::now());

} // namespace kiarabot::core::platform
