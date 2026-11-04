#include "kiarabot/core/application/target/runtime_resource_application.hpp"

#include "kiarabot/core/application/target/runtime_readiness.hpp"
#include "runtime_resources/components.hpp"
#include "runtime_resources/snapshot_pages.hpp"

namespace kiarabot::core::target {
namespace {
namespace detail = runtime_resources_detail;
class RuntimeResourceApplicationImpl final : public RuntimeResourceApplication {
public:
  explicit RuntimeResourceApplicationImpl(const RuntimeResourceDependencies& dependencies)
      : pages_(dependencies.ids, dependencies.authorization),
        account_adapter_(detail::make_account_adapter(dependencies, pages_)),
        instances_(detail::make_instances(dependencies, pages_)),
        lifecycle_(detail::make_lifecycle(dependencies)),
        host_commands_(detail::make_host_commands(dependencies)),
        timer_(detail::make_timer(dependencies)) {}
  Service service() const noexcept override {
    return Service::Runtime;
  }
  ServiceReadiness readiness() const override {
    return evaluate_runtime_readiness(RuntimeReadinessObservation{
        DependencyState::Available, DependencyState::Unverified, DependencyState::Unverified});
  }

  Outcome<BotAccount> create_account(const CreateAccountRequest& request) override {
    return account_adapter_->create_account(request);
  }

  Outcome<AccountMutation> update_account(const UpdateAccountRequest& request) override {
    return account_adapter_->update_account(request);
  }

  Outcome<AccountMutation> disable_account(const DisableAccountRequest& request) override {
    return account_adapter_->disable_account(request);
  }

  Outcome<BotAccount> get_account(const AccountQuery& request) override {
    return account_adapter_->get_account(request);
  }

  Outcome<ListResult<BotAccount>> list_accounts(const AccountListQuery& request) override {
    return account_adapter_->list_accounts(request);
  }

  Outcome<BindingMutation> bind_adapter(const BindAdapterRequest& request) override {
    return account_adapter_->bind_adapter(request);
  }

  Outcome<BindingMutation> unbind_adapter(const UnbindAdapterRequest& request) override {
    return account_adapter_->unbind_adapter(request);
  }

  Outcome<AdapterRegistrationResult>
  register_adapter(const AdapterRegistrationCommand& request) override {
    return account_adapter_->register_adapter(request);
  }

  Outcome<HeartbeatResult> heartbeat(const HeartbeatCommand& request) override {
    return account_adapter_->heartbeat(request);
  }

  Outcome<AdapterRecord> get_adapter(const AdapterQuery& request) override {
    return account_adapter_->get_adapter(request);
  }

  Outcome<ListResult<AdapterRecord>> list_adapters(const AdapterListQuery& request) override {
    return account_adapter_->list_adapters(request);
  }

  Outcome<ManifestValidation> validate_manifest(const ValidateManifestCommand& request) override {
    return instances_->validate_manifest(request);
  }

  Outcome<PluginInstallation>
  create_installation(const CreateInstallationRequest& request) override {
    return instances_->create_installation(request);
  }

  Outcome<PluginInstallation> get_installation(const InstallationQuery& request) override {
    return instances_->get_installation(request);
  }

  Outcome<ListResult<PluginInstallation>>
  list_installations(const InstallationListQuery& request) override {
    return instances_->list_installations(request);
  }

  Outcome<ListResult<PluginInstance>> list_instances(const InstanceListQuery& request) override {
    return instances_->list_instances(request);
  }

  Outcome<InstanceCreation> create_instance(const CreateInstanceRequest& request) override {
    return instances_->create_instance(request);
  }

  Outcome<PluginInstance> get_instance(const InstanceQuery& request) override {
    return instances_->get_instance(request);
  }

  Outcome<InstanceConfiguration> get_instance_config(const InstanceConfigQuery& request) override {
    return instances_->get_instance_config(request);
  }

  Outcome<InstanceMutation> apply_desired_state(const SetDesiredStateRequest& request) override {
    return instances_->apply_desired_state(request);
  }

  Outcome<ConfigMutation> update_config(const UpdateConfigRequest& request) override {
    return instances_->update_config(request);
  }

  Outcome<ResourceState> get_resource_state(const ResourceStateQuery& request) override {
    return lifecycle_->get_resource_state(request);
  }

  Outcome<Operation> get_operation(const OperationQuery& request) override {
    return lifecycle_->get_operation(request);
  }

  Outcome<Operation> begin_delete(const BeginDeleteRequest& request) override {
    return lifecycle_->begin_delete(request);
  }

  Outcome<Operation> restore_resource(const RestoreResourceRequest& request) override {
    return lifecycle_->restore_resource(request);
  }

  Outcome<LifecycleMaintenance> lifecycle_maintenance() override {
    return lifecycle_->lifecycle_maintenance();
  }

  Outcome<std::vector<HostCommandOutboxRecord>>
  host_pending_commands(const std::string& host_id, std::uint64_t connection_generation,
                        std::size_t limit) override {
    return host_commands_->host_pending_commands(host_id, connection_generation, limit);
  }

  Status host_command_dispatched(const std::string& command_id, std::uint64_t connection_generation,
                                 std::string_view runtime_epoch) override {
    return host_commands_->host_command_dispatched(command_id, connection_generation,
                                                   runtime_epoch);
  }

  Status host_command_receipt(const std::string& command_id, std::uint64_t connection_generation,
                              bool persisted, std::optional<ExecutionOutcome> outcome,
                              std::optional<std::string> process_marker) override {
    return host_commands_->host_command_receipt(command_id, connection_generation, persisted,
                                                outcome, std::move(process_marker));
  }

  Outcome<bool> host_observation(const HostObservationRecord& observation) override {
    return host_commands_->host_observation(observation);
  }

  Outcome<HostCommandMaintenance> host_command_maintenance() override {
    return host_commands_->host_command_maintenance();
  }

  Outcome<TimerTriggerAcceptance>
  accept_timer_trigger(const AcceptTimerTriggerRequest& request) override {
    return timer_->accept_timer_trigger(request);
  }

private:
  detail::SnapshotPages pages_;
  std::unique_ptr<RuntimeAccountAdapterApplication> account_adapter_;
  std::unique_ptr<RuntimeInstanceApplication> instances_;
  std::unique_ptr<RuntimeLifecycleApplication> lifecycle_;
  std::unique_ptr<RuntimeHostCommandApplication> host_commands_;
  std::unique_ptr<RuntimeTimerApplication> timer_;
};
} // namespace

Outcome<std::unique_ptr<RuntimeResourceApplication>>
make_runtime_resource_application(RuntimeResourceDependencies dependencies) {
  try {
    return std::unique_ptr<RuntimeResourceApplication>(
        std::make_unique<RuntimeResourceApplicationImpl>(std::move(dependencies)));
  } catch (const std::bad_alloc&) {
    return detail::unavailable();
  } catch (...) {
    return detail::internal_failure();
  }
}

} // namespace kiarabot::core::target
