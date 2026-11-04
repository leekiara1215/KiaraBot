#pragma once

#include "kiarabot/core/application/target/runtime_resources/types.hpp"

namespace kiarabot::core::target {

// Narrow Runtime resource use cases. Each call owns its local UoW; borrowed ports outlive calls.
class RuntimeInstanceApplication {
public:
  virtual ~RuntimeInstanceApplication() = default;

  virtual Outcome<ManifestValidation> validate_manifest(const ValidateManifestCommand& request) = 0;
  virtual Outcome<PluginInstallation>
  create_installation(const CreateInstallationRequest& request) = 0;
  virtual Outcome<PluginInstallation> get_installation(const InstallationQuery& request) = 0;
  virtual Outcome<ListResult<PluginInstallation>>
  list_installations(const InstallationListQuery& request) = 0;
  virtual Outcome<ListResult<PluginInstance>> list_instances(const InstanceListQuery& request) = 0;
  virtual Outcome<InstanceCreation> create_instance(const CreateInstanceRequest& request) = 0;
  virtual Outcome<PluginInstance> get_instance(const InstanceQuery& request) = 0;
  virtual Outcome<InstanceConfiguration>
  get_instance_config(const InstanceConfigQuery& request) = 0;
  virtual Outcome<InstanceMutation> apply_desired_state(const SetDesiredStateRequest& request) = 0;
  virtual Outcome<ConfigMutation> update_config(const UpdateConfigRequest& request) = 0;
};

} // namespace kiarabot::core::target
