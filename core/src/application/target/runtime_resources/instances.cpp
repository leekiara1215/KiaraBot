#include <algorithm>
#include <set>

#include "common.hpp"
#include "components.hpp"
#include "kiarabot/core/application/target/runtime_resources/bootstrap.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/host_command_records.hpp"
#include "mutation.hpp"
#include "snapshot_pages.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
namespace {
class RuntimeInstanceApplicationImpl final : public RuntimeInstanceApplication {
public:
  RuntimeInstanceApplicationImpl(const RuntimeResourceDependencies& dependencies,
                                 SnapshotPages& pages)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        secrets_(dependencies.secrets), authorization_(dependencies.authorization), pages_(pages),
        resource_catalogs_(dependencies.resource_catalogs) {}

  Outcome<ManifestValidation> validate_manifest(const ValidateManifestCommand& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Installation, request.catalog, Permission::PluginManage);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    // The boundary has already parsed the protobuf manifest; this increment only bounds the opaque
    // canonical serialization. Full manifest schema rules arrive with the plugin-host phase.
    ManifestValidation validation;
    if (request.manifest.empty() || request.manifest.size() > 64 * 1024) {
      validation.violations.push_back(invalid_runtime_input());
      return validation;
    }
    validation.valid = true;
    return validation;
  }

  Outcome<PluginInstallation>
  create_installation(const CreateInstallationRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (request.plugin_type_id.empty() || request.manifest.empty() ||
        request.manifest.size() > 64 * 1024) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Installation, request.catalog, Permission::PluginManage);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto allocated = ids_.installation_id();
    if (const auto* failure = std::get_if<Failure>(&allocated)) {
      return *failure;
    }
    const auto id = std::get<InstallationId>(allocated);
    const ResourceRef receipt_scope{ResourceKind::Catalog, {"runtime-create-installation"}};
    auto result = mutate<PluginInstallation>(
        transactions_, request.context, receipt_scope, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<PluginInstallation> {
          PluginInstallation installation{id,
                                          ResourceId{request.plugin_type_id},
                                          {1},
                                          LifecycleState::Active,
                                          request.manifest};
          const auto saved = work.installations().save(installation, std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          (void)now;
          return installation;
        },
        [](const PluginInstallation& value) {
          return ResourceRef{ResourceKind::Installation, {value.id.value}};
        },
        [](const PluginInstallation& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    return std::move(std::get<Mutation<PluginInstallation>>(result).value);
  }

  Outcome<PluginInstallation> get_installation(const InstallationQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Installation, {request.installation.value}};
    auto checked = authorize(authorization_, request.context.authentication, Permission::PluginRead,
                             request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<PluginInstallation>(
        transactions_, request.context, [&](RuntimeUnitOfWork& work, TimePoint) {
          return require_found(work.installations().find(request.installation));
        });
  }

  Outcome<ListResult<PluginInstallation>>
  list_installations(const InstallationListQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.page.page_size == 0 || request.page.page_size > kMaximumPageSize) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Installation, request.catalog, Permission::PluginRead);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<PluginInstallation>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<PluginInstallation>> {
          return pages_.list_page<PluginInstallation>(
              work, request.context, request.page, "installations|" + request.catalog.id.value,
              authorization_version, now, Permission::PluginRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_installations(limit, after);
              },
              [](const PluginInstallation& value) {
                return ResourceRef{ResourceKind::Installation, {value.id.value}};
              },
              [](const PluginInstallation& value) -> const std::string& { return value.id.value; });
        },
        true);
  }

  Outcome<ListResult<PluginInstance>> list_instances(const InstanceListQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.page.page_size == 0 || request.page.page_size > kMaximumPageSize) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Instance, request.catalog, Permission::PluginRead);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<PluginInstance>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<PluginInstance>> {
          return pages_.list_page<PluginInstance>(
              work, request.context, request.page, "instances|" + request.catalog.id.value,
              authorization_version, now, Permission::PluginRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_instances(limit, after);
              },
              [](const PluginInstance& value) {
                return ResourceRef{ResourceKind::Instance, {value.id.value}};
              },
              [](const PluginInstance& value) -> const std::string& { return value.id.value; });
        },
        true);
  }

  Outcome<InstanceCreation> create_instance(const CreateInstanceRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (!canonical(request.installation.value)) {
      return invalid_runtime_input();
    }
    const ResourceRef parent{ResourceKind::Installation, {request.installation.value}};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::PluginManage,
                  request.context.method, {parent}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto allocated = ids_.instance_id();
    if (const auto* failure = std::get_if<Failure>(&allocated)) {
      return *failure;
    }
    const auto id = std::get<InstanceId>(allocated);
    const ResourceRef receipt_scope{ResourceKind::Catalog, {"runtime-create-instance"}};
    std::optional<std::string> bootstrap_credential;
    std::optional<BootstrapDeliveryRecord> delivery;
    auto result = mutate<PluginInstance>(
        transactions_, request.context, receipt_scope, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<PluginInstance> {
          auto installation = work.installations().find(request.installation);
          if (const auto* failure = std::get_if<Failure>(&installation)) {
            return *failure;
          }
          const auto value = std::move(std::get<std::optional<PluginInstallation>>(installation));
          if (!value) {
            return not_found();
          }
          if (const auto failure = status_failure(
                  validate_instance_create(id, *value, request.host, request.externally_managed))) {
            return *failure;
          }
          PluginInstance instance{id,
                                  request.installation,
                                  value->plugin_type,
                                  ResourceVersion{1},
                                  ResourceVersion{0},
                                  std::nullopt,
                                  ResourceVersion{1},
                                  LifecycleState::Active,
                                  request.host,
                                  DesiredState::Running};
          const auto saved = work.instances().save(instance, std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          const ResourceRef resource{ResourceKind::Instance, {id.value}};
          if (!request.bootstrap_recipient_id.empty()) {
            // Explicit opt-in: first issuance persists digest, real monotonic version, recipient
            // binding and no-secret delivery intent with a Runtime-owned operation. The plaintext
            // stays only in this response object.
            auto operation = accepted_operation(work, request.context, resource, now);
            if (const auto* failure = std::get_if<Failure>(&operation)) {
              return *failure;
            }
            auto issued = issue_bootstrap_delivery(
                work, ids_, secrets_, resource, BootstrapPeerFamily::Plugin, id.value,
                request.bootstrap_recipient_id, std::get<Operation>(operation).ref, now);
            if (const auto* failure = std::get_if<Failure>(&issued)) {
              return *failure;
            }
            auto issue = std::move(std::get<std::optional<BootstrapDeliveryIssue>>(issued));
            if (!issue) {
              return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
            }
            bootstrap_credential = std::move(issue->plaintext);
            delivery = std::move(issue->delivery);
          } else if (request.externally_managed || request.host) {
            // Managed instances also receive a one-time deployment bootstrap; the Host operator
            // places it in the Host credential directory (0600) before starting the instance. The
            // externally-managed P06 bridge path uses the same stored digest and response field.
            auto issued = issue_bootstrap_credential(work, ids_, secrets_, id.value, now);
            if (const auto* failure = std::get_if<Failure>(&issued)) {
              return *failure;
            }
            bootstrap_credential = std::move(std::get<std::optional<std::string>>(issued));
          }
          const auto enqueued = enqueue_host_command(work, secrets_, instance, now);
          if (const auto* failure = std::get_if<Failure>(&enqueued)) {
            return *failure;
          }
          return instance;
        },
        [](const PluginInstance& value) {
          return ResourceRef{ResourceKind::Instance, {value.id.value}};
        },
        [](const PluginInstance& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<PluginInstance>>(result));
    if (!delivery && !request.bootstrap_recipient_id.empty()) {
      // Idempotent replay restores the current no-secret delivery metadata (never the plaintext).
      const ResourceRef instance_resource{ResourceKind::Instance, {mutation.value.id.value}};
      auto current = current_delivery_metadata(transactions_, request.context, instance_resource,
                                               request.bootstrap_recipient_id);
      if (const auto* rejected = std::get_if<Failure>(&current)) {
        return *rejected;
      }
      delivery = std::move(std::get<std::optional<BootstrapDeliveryRecord>>(current));
    }
    return InstanceCreation{std::move(mutation.value), std::move(bootstrap_credential),
                            std::move(delivery)};
  }

  Outcome<PluginInstance> get_instance(const InstanceQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Instance, {request.instance.value}};
    auto checked = authorize(authorization_, request.context.authentication, Permission::PluginRead,
                             request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<PluginInstance>(
        transactions_, request.context, [&](RuntimeUnitOfWork& work, TimePoint) {
          return require_found(work.instances().find(request.instance));
        });
  }

  Outcome<InstanceConfiguration> get_instance_config(const InstanceConfigQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Instance, {request.instance.value}};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::PluginConfigRead,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<InstanceConfiguration>(
        transactions_, request.context, [&](RuntimeUnitOfWork& work, TimePoint) {
          return require_found(work.instance_configs().find(request.instance));
        });
  }

  Outcome<InstanceMutation> apply_desired_state(const SetDesiredStateRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Instance, {request.instance.value}};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::PluginManage,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<PluginInstance>(
        transactions_, request.context, resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<PluginInstance> {
          auto found = work.instances().find(request.instance);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          auto current = std::move(std::get<std::optional<PluginInstance>>(found));
          if (!current) {
            return not_found();
          }
          auto changed = ::kiarabot::core::target::advance_desired_state(
              *current, request.desired, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&changed)) {
            return *failure;
          }
          const auto& updated = std::get<PluginInstance>(changed);
          const auto saved = work.instances().save(updated, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto operation = accepted_operation(work, request.context, resource, now);
          if (const auto* failure = std::get_if<Failure>(&operation)) {
            return *failure;
          }
          const auto enqueued = enqueue_host_command(work, secrets_, updated, now);
          if (const auto* failure = std::get_if<Failure>(&enqueued)) {
            return *failure;
          }
          return updated;
        },
        [](const PluginInstance& value) {
          return ResourceRef{ResourceKind::Instance, {value.id.value}};
        },
        [](const PluginInstance& value) { return value.version; }, true);
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<PluginInstance>>(result));
    return InstanceMutation{std::move(mutation.value), accepted(std::move(mutation.operation)),
                            std::move(mutation.committed_state)};
  }

  Outcome<ConfigMutation> update_config(const UpdateConfigRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Instance, {request.instance.value}};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::PluginManage,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<InstanceConfiguration>(
        transactions_, request.context, resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<InstanceConfiguration> {
          auto found = work.instances().find(request.instance);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          auto current = std::move(std::get<std::optional<PluginInstance>>(found));
          if (!current) {
            return not_found();
          }
          if (const auto failure =
                  status_failure(validate_config_update(*current, request.values.size()))) {
            return *failure;
          }
          auto changed = save_desired_config(*current, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&changed)) {
            return *failure;
          }
          const auto& updated = std::get<PluginInstance>(changed);
          const auto saved = work.instances().save(updated, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto existing = work.instance_configs().find(request.instance);
          if (const auto* failure = std::get_if<Failure>(&existing)) {
            return *failure;
          }
          const auto previous = std::move(std::get<std::optional<InstanceConfiguration>>(existing));
          InstanceConfiguration config{request.instance,
                                       updated.desired_version,
                                       updated.applied_config_version,
                                       ConfigApplyState::Pending,
                                       std::string{},
                                       updated.current,
                                       std::nullopt,
                                       now,
                                       request.values,
                                       ResourceVersion{previous ? previous->version.value + 1 : 1}};
          const auto saved_config = work.instance_configs().save(
              config, previous ? std::optional<ResourceVersion>{previous->version} : std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&saved_config)) {
            return *failure;
          }
          auto operation = accepted_operation(work, request.context, resource, now);
          if (const auto* failure = std::get_if<Failure>(&operation)) {
            return *failure;
          }
          return config;
        },
        [](const InstanceConfiguration& value) {
          return ResourceRef{ResourceKind::Instance, {value.instance.value}};
        },
        [](const InstanceConfiguration& value) { return value.version; }, true);
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<InstanceConfiguration>>(result));
    return ConfigMutation{std::move(mutation.value), accepted(std::move(mutation.operation)),
                          std::move(mutation.committed_state)};
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  RuntimeAuthorizationPort& authorization_;
  SnapshotPages& pages_;
  const std::map<ResourceKind, ResourceId> resource_catalogs_;
};
} // namespace

std::unique_ptr<RuntimeInstanceApplication>
make_instances(const RuntimeResourceDependencies& dependencies, SnapshotPages& pages) {
  return std::make_unique<RuntimeInstanceApplicationImpl>(dependencies, pages);
}

} // namespace kiarabot::core::target::runtime_resources_detail
