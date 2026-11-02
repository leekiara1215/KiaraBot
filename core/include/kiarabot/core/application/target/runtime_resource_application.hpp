#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/domain/target/runtime_records.hpp"
#include "kiarabot/core/domain/target/runtime_rules.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/service_readiness.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

inline constexpr std::uint32_t kDefaultPageSize = 20;
inline constexpr std::uint32_t kMaximumPageSize = 100;
inline constexpr auto kSnapshotLifetime = std::chrono::minutes{10};

struct PageRequest {
  std::uint32_t page_size = kDefaultPageSize;
  std::string page_token;
};
struct PageInfo {
  std::uint32_t page_size = 0;
  std::string next_page_token;
};
template <typename Value> struct ListResult {
  std::vector<Value> items;
  PageInfo page;
  SnapshotWindow snapshot;
};
struct OperationAccepted {
  OperationRef operation;
};
struct AccountMutation {
  BotAccount account;
  std::optional<OperationAccepted> operation;
};
struct BindingMutation {
  AdapterBindingRecord binding;
  std::optional<OperationAccepted> operation;
};
struct AdapterRegistrationResult {
  AdapterRecord adapter;
  std::vector<Capability> effective_capabilities;
  AdapterFence fence;
  std::uint32_t heartbeat_interval_seconds = 30;
};
struct HeartbeatResult {
  AdapterFence fence;
  TimePoint received_at;
};
struct InstanceMutation {
  PluginInstance instance;
  std::optional<OperationAccepted> operation;
};
struct ConfigMutation {
  InstanceConfiguration config;
  std::optional<OperationAccepted> operation;
};
struct ManifestValidation {
  bool valid = false;
  std::vector<Failure> violations;
};
// Neutral typed resource state projection; the boundary renders the wire VersionedResourceState.
struct ResourceState {
  ResourceRef resource;
  ResourceVersion version;
  LifecycleState lifecycle = LifecycleState::Active;
  DesiredState desired = DesiredState::Running;
  std::optional<Fence> fence;
  // Adapter resources carry a connection-only fence (no trusted process execution in P05).
  std::optional<AdapterFence> adapter_fence;
  bool registered = false;
  ResourceVersion desired_config_version;
  ResourceVersion applied_config_version;
  ExecutionOutcome process_outcome = ExecutionOutcome::NotStarted;
  TimePoint observed_at;
  Service source_service = Service::Runtime;
};

struct CreateAccountRequest {
  CommandContext context;
  std::string platform;
  std::string platform_account_id;
  std::string display_name;
};
struct UpdateAccountRequest {
  CommandContext context;
  ResourceId account;
  std::string display_name;
};
struct DisableAccountRequest {
  CommandContext context;
  ResourceId account;
};
struct AccountQuery {
  CommandContext context;
  ResourceId account;
};
struct AccountListQuery {
  CommandContext context;
  PageRequest page;
};
struct BindAdapterRequest {
  CommandContext context;
  ResourceId account;
  InstanceId adapter_instance;
};
struct UnbindAdapterRequest {
  CommandContext context;
  ResourceId account;
};
struct AdapterRegistrationCommand {
  // Direct adapter peer authentication is boundary-owned; the command carries the trusted result.
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  ResourceId adapter;
  AdapterRegistrationMetadata metadata;
  std::optional<InstanceId> presented_instance;
  std::vector<ResourceId> declared_accounts;
};
struct HeartbeatCommand {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  TimePoint deadline;
  // Wire Fence values only: instance, runtime epoch and the positive connection generation. The
  // internal connection id is resolved by the application from the current authoritative record.
  AdapterFenceClaim fence;
};
struct AdapterQuery {
  CommandContext context;
  ResourceId adapter;
};
struct AdapterListQuery {
  CommandContext context;
  PageRequest page;
};
struct ValidateManifestCommand {
  CommandContext context;
  std::vector<std::byte> manifest;
};
struct CreateInstallationRequest {
  CommandContext context;
  std::string plugin_type_id;
  std::vector<std::byte> manifest;
};
struct InstallationQuery {
  CommandContext context;
  InstallationId installation;
};
struct InstallationListQuery {
  CommandContext context;
  PageRequest page;
};
struct CreateInstanceRequest {
  CommandContext context;
  InstallationId installation;
  std::optional<ResourceId> host;
  bool externally_managed = false;
};
struct InstanceQuery {
  CommandContext context;
  InstanceId instance;
};
struct InstanceListQuery {
  CommandContext context;
  PageRequest page;
};
struct InstanceConfigQuery {
  CommandContext context;
  InstanceId instance;
};
struct SetDesiredStateRequest {
  CommandContext context;
  InstanceId instance;
  DesiredState desired;
};
struct UpdateConfigRequest {
  CommandContext context;
  InstanceId instance;
  std::vector<std::byte> values;
};
struct ResourceStateQuery {
  CommandContext context;
  ResourceRef resource;
};
struct OperationQuery {
  CommandContext context;
  OperationRef operation;
};

// Runtime resource application boundary. Every method owns its UoWs, enforces current online
// authorization before effects, commits receipts/audit/operation in the same local transaction and
// never performs network I/O inside a UoW. Boundary adapters only convert wire values and map
// stable failures.
class RuntimeResourceApplication : public ServiceReadinessPort {
public:
  ~RuntimeResourceApplication() override = default;

  virtual Outcome<BotAccount> create_account(const CreateAccountRequest& request) = 0;
  virtual Outcome<AccountMutation> update_account(const UpdateAccountRequest& request) = 0;
  virtual Outcome<AccountMutation> disable_account(const DisableAccountRequest& request) = 0;
  virtual Outcome<BotAccount> get_account(const AccountQuery& request) = 0;
  virtual Outcome<ListResult<BotAccount>> list_accounts(const AccountListQuery& request) = 0;

  virtual Outcome<BindingMutation> bind_adapter(const BindAdapterRequest& request) = 0;
  virtual Outcome<BindingMutation> unbind_adapter(const UnbindAdapterRequest& request) = 0;

  virtual Outcome<AdapterRegistrationResult>
  register_adapter(const AdapterRegistrationCommand& request) = 0;
  virtual Outcome<HeartbeatResult> heartbeat(const HeartbeatCommand& request) = 0;
  virtual Outcome<AdapterRecord> get_adapter(const AdapterQuery& request) = 0;
  virtual Outcome<ListResult<AdapterRecord>> list_adapters(const AdapterListQuery& request) = 0;

  virtual Outcome<ManifestValidation> validate_manifest(const ValidateManifestCommand& request) = 0;
  virtual Outcome<PluginInstallation>
  create_installation(const CreateInstallationRequest& request) = 0;
  virtual Outcome<PluginInstallation> get_installation(const InstallationQuery& request) = 0;
  virtual Outcome<ListResult<PluginInstallation>>
  list_installations(const InstallationListQuery& request) = 0;

  virtual Outcome<PluginInstance> create_instance(const CreateInstanceRequest& request) = 0;
  virtual Outcome<PluginInstance> get_instance(const InstanceQuery& request) = 0;
  virtual Outcome<ListResult<PluginInstance>> list_instances(const InstanceListQuery& request) = 0;
  virtual Outcome<InstanceConfiguration>
  get_instance_config(const InstanceConfigQuery& request) = 0;
  virtual Outcome<InstanceMutation> apply_desired_state(const SetDesiredStateRequest& request) = 0;
  virtual Outcome<ConfigMutation> update_config(const UpdateConfigRequest& request) = 0;

  virtual Outcome<ResourceState> get_resource_state(const ResourceStateQuery& request) = 0;
  virtual Outcome<Operation> get_operation(const OperationQuery& request) = 0;
};

struct RuntimeResourceDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  // Fail-closed composition adapter until the Identity current-authorization client is wired.
  RuntimeAuthorizationPort& authorization;
  // Trusted capability catalog this build actually supports; never request-controlled.
  std::vector<std::string> supported_capabilities;
};

// Borrowed dependencies outlive every call; each call owns separate short UoWs.
Outcome<std::unique_ptr<RuntimeResourceApplication>>
make_runtime_resource_application(RuntimeResourceDependencies dependencies);

} // namespace kiarabot::core::target
