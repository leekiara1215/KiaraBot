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
class RuntimeAccountAdapterApplicationImpl final : public RuntimeAccountAdapterApplication {
public:
  RuntimeAccountAdapterApplicationImpl(const RuntimeResourceDependencies& dependencies,
                                       SnapshotPages& pages)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        secrets_(dependencies.secrets), authorization_(dependencies.authorization), pages_(pages),
        resource_catalogs_(dependencies.resource_catalogs),
        supported_capabilities_(dependencies.supported_capabilities) {}

  Outcome<BotAccount> create_account(const CreateAccountRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Account, request.catalog, Permission::AccountManage);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto allocated = ids_.account_id();
    if (const auto* failure = std::get_if<Failure>(&allocated)) {
      return *failure;
    }
    const auto id = std::get<ResourceId>(allocated);
    const auto validation = validate_account_create(
        id, request.platform, request.platform_account_id, request.display_name);
    if (const auto* failure = std::get_if<Failure>(&validation)) {
      return *failure;
    }
    // Create has no pre-existing resource identity, so the idempotency/receipt scope uses a stable
    // kind token instead of the freshly allocated id; the created resource id is not part of the
    // replay key and a client retry with the same key must not allocate a second account.
    const ResourceRef receipt_scope{ResourceKind::Catalog, {"runtime-create-account"}};
    auto result = mutate<BotAccount>(
        transactions_, request.context, receipt_scope, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<BotAccount> {
          const auto existing = work.accounts().find(id);
          if (const auto* failure = std::get_if<Failure>(&existing)) {
            return *failure;
          }
          if (std::get<std::optional<BotAccount>>(existing)) {
            return stale_version();
          }
          BotAccount account{id,
                             request.platform,
                             request.platform_account_id,
                             request.display_name,
                             ResourceVersion{1},
                             LifecycleState::Active};
          const auto saved = work.accounts().save(account, std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          if (now <= TimePoint{}) {
            return internal_failure();
          }
          return account;
        },
        [](const BotAccount& value) {
          return ResourceRef{ResourceKind::Account, value.id};
        },
        [](const BotAccount& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    return std::move(std::get<Mutation<BotAccount>>(result).value);
  }

  Outcome<AccountMutation> update_account(const UpdateAccountRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (!canonical(request.account.value)) {
      return invalid_runtime_input();
    }
    const ResourceRef resource{ResourceKind::Account, request.account};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AccountManage,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<BotAccount>(
        transactions_, request.context, resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<BotAccount> {
          auto found = work.accounts().find(request.account);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          auto current = std::move(std::get<std::optional<BotAccount>>(found));
          if (!current) {
            return not_found();
          }
          if (const auto failure = status_failure(validate_account_update(
                  *current, request.display_name, request.context.expected_version))) {
            return *failure;
          }
          current->display_name = request.display_name;
          current->version.value += 1;
          const auto saved = work.accounts().save(*current, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          return *current;
        },
        [](const BotAccount& value) {
          return ResourceRef{ResourceKind::Account, value.id};
        },
        [](const BotAccount& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<BotAccount>>(result));
    return AccountMutation{std::move(mutation.value), accepted(std::move(mutation.operation))};
  }

  Outcome<AccountMutation> disable_account(const DisableAccountRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Account, request.account};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AccountManage,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<BotAccount>(
        transactions_, request.context, resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<BotAccount> {
          auto found = work.accounts().find(request.account);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          auto current = std::move(std::get<std::optional<BotAccount>>(found));
          if (!current) {
            return not_found();
          }
          auto binding = work.adapter_bindings().find(request.account);
          if (const auto* failure = std::get_if<Failure>(&binding)) {
            return *failure;
          }
          const auto bound = std::get<std::optional<AdapterBindingRecord>>(binding);
          const bool active = bound && bound->state == LifecycleState::Active;
          if (const auto failure = status_failure(
                  validate_account_disable(*current, request.context.expected_version, active))) {
            return *failure;
          }
          current->state = LifecycleState::Disabled;
          current->version.value += 1;
          const auto saved = work.accounts().save(*current, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto operation = accepted_operation(work, request.context, resource, now);
          if (const auto* failure = std::get_if<Failure>(&operation)) {
            return *failure;
          }
          return *current;
        },
        [](const BotAccount& value) {
          return ResourceRef{ResourceKind::Account, value.id};
        },
        [](const BotAccount& value) { return value.version; }, true);
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<BotAccount>>(result));
    return AccountMutation{std::move(mutation.value), accepted(std::move(mutation.operation))};
  }

  Outcome<BotAccount> get_account(const AccountQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Account, request.account};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AccountRead,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<BotAccount>(transactions_, request.context,
                                 [&](RuntimeUnitOfWork& work, TimePoint) {
                                   return require_found(work.accounts().find(request.account));
                                 });
  }

  Outcome<ListResult<BotAccount>> list_accounts(const AccountListQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.page.page_size == 0 || request.page.page_size > kMaximumPageSize) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Account, request.catalog, Permission::AccountRead);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<BotAccount>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<BotAccount>> {
          return pages_.list_page<BotAccount>(
              work, request.context, request.page, "accounts|" + request.catalog.id.value,
              authorization_version, now, Permission::AccountRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_accounts(limit, after);
              },
              [](const BotAccount& value) {
                return ResourceRef{ResourceKind::Account, value.id};
              },
              [](const BotAccount& value) -> const std::string& { return value.id.value; });
        },
        true);
  }

  Outcome<BindingMutation> bind_adapter(const BindAdapterRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (!canonical(request.account.value) || request.adapter_instance.value.empty()) {
      return invalid_runtime_input();
    }
    const ResourceRef resource{ResourceKind::Account, request.account};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AccountManage,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    std::optional<std::string> bootstrap_credential;
    std::optional<BootstrapDeliveryRecord> delivery;
    auto result = mutate<AdapterBindingRecord>(
        transactions_, request.context, resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<AdapterBindingRecord> {
          auto account = work.accounts().find(request.account);
          if (const auto* failure = std::get_if<Failure>(&account)) {
            return *failure;
          }
          const auto account_value = std::get<std::optional<BotAccount>>(account);
          if (!account_value) {
            return not_found();
          }
          auto adapter = work.adapters().by_instance(request.adapter_instance);
          if (const auto* failure = std::get_if<Failure>(&adapter)) {
            return *failure;
          }
          const auto adapter_value = std::get<std::optional<AdapterRecord>>(adapter);
          auto existing = work.adapter_bindings().find(request.account);
          if (const auto* failure = std::get_if<Failure>(&existing)) {
            return *failure;
          }
          const auto binding_value = std::get<std::optional<AdapterBindingRecord>>(existing);
          if (request.context.expected_version &&
              (!binding_value || binding_value->version != *request.context.expected_version)) {
            return runtime_conflict();
          }
          // A1: an external self-managed adapter may be bound before its first bridge
          // registration creates the AdapterRecord. The bootstrap credential issued below is the
          // identity that the registration must present; a registered adapter still validates its
          // current fence and lifecycle.
          const auto validation = adapter_value
                                      ? validate_bind(*account_value, *adapter_value, binding_value)
                                      : validate_bind_unregistered(*account_value, binding_value);
          if (const auto failure = status_failure(validation)) {
            return *failure;
          }
          AdapterBindingRecord binding{
              request.account, request.adapter_instance,
              ResourceVersion{binding_value ? binding_value->version.value + 1 : 1},
              LifecycleState::Active};
          const auto saved = work.adapter_bindings().save(
              binding, binding_value ? std::optional<ResourceVersion>{binding_value->version}
                                     : std::nullopt);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto operation = accepted_operation(work, request.context, resource, now);
          if (const auto* failure = std::get_if<Failure>(&operation)) {
            return *failure;
          }
          if (!request.bootstrap_recipient_id.empty()) {
            // Explicit opt-in: the first issuance persists the digest, the real monotonic version,
            // the recipient binding and the no-secret delivery intent in this same UoW. An
            // already-active credential is never re-exposed here; an explicit authorized reissue
            // is the only way to obtain a new plaintext.
            auto issued = issue_bootstrap_delivery(
                work, ids_, secrets_, resource, BootstrapPeerFamily::Adapter,
                request.adapter_instance.value, request.bootstrap_recipient_id,
                std::get<Operation>(operation).ref, now);
            if (const auto* failure = std::get_if<Failure>(&issued)) {
              return *failure;
            }
            auto issue = std::move(std::get<std::optional<BootstrapDeliveryIssue>>(issued));
            if (!issue) {
              return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
            }
            bootstrap_credential = std::move(issue->plaintext);
            delivery = std::move(issue->delivery);
            return binding;
          }
          auto issued =
              issue_bootstrap_credential(work, ids_, secrets_, request.adapter_instance.value, now);
          if (const auto* failure = std::get_if<Failure>(&issued)) {
            return *failure;
          }
          bootstrap_credential = std::move(std::get<std::optional<std::string>>(issued));
          return binding;
        },
        [](const AdapterBindingRecord& value) {
          return ResourceRef{ResourceKind::Account, value.account};
        },
        [](const AdapterBindingRecord& value) { return value.version; }, true);
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<AdapterBindingRecord>>(result));
    if (!delivery && !request.bootstrap_recipient_id.empty()) {
      // Idempotent replay restores the current no-secret delivery metadata (never the plaintext) so
      // Control can recover the original delivery_id after a lost response.
      auto current = current_delivery_metadata(transactions_, request.context, resource,
                                               request.bootstrap_recipient_id);
      if (const auto* rejected = std::get_if<Failure>(&current)) {
        return *rejected;
      }
      delivery = std::move(std::get<std::optional<BootstrapDeliveryRecord>>(current));
    }
    return BindingMutation{std::move(mutation.value), accepted(std::move(mutation.operation)),
                           std::move(bootstrap_credential), std::move(delivery)};
  }

  Outcome<BindingMutation> unbind_adapter(const UnbindAdapterRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Account, request.account};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AccountManage,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    auto result = mutate<AdapterBindingRecord>(
        transactions_, request.context, resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<AdapterBindingRecord> {
          auto found = work.adapter_bindings().find(request.account);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          auto binding = std::move(std::get<std::optional<AdapterBindingRecord>>(found));
          if (!binding) {
            return not_found();
          }
          if (request.adapter_instance && *request.adapter_instance != binding->adapter_instance) {
            return runtime_conflict();
          }
          if (const auto failure =
                  status_failure(validate_unbind(*binding, request.context.expected_version))) {
            return *failure;
          }
          binding->state = LifecycleState::Disabled;
          binding->version.value += 1;
          const auto saved =
              work.adapter_bindings().save(*binding, request.context.expected_version);
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          auto operation = accepted_operation(work, request.context, resource, now);
          if (const auto* failure = std::get_if<Failure>(&operation)) {
            return *failure;
          }
          const auto remaining = work.active_bindings(binding->adapter_instance);
          if (const auto* failure = std::get_if<Failure>(&remaining)) {
            return *failure;
          }
          if (std::get<std::size_t>(remaining) == 0) {
            const auto revoked =
                work.bridge_credentials().revoke(binding->adapter_instance.value, now);
            if (const auto* failure = std::get_if<Failure>(&revoked)) {
              return *failure;
            }
          }
          return *binding;
        },
        [](const AdapterBindingRecord& value) {
          return ResourceRef{ResourceKind::Account, value.account};
        },
        [](const AdapterBindingRecord& value) { return value.version; }, true);
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<AdapterBindingRecord>>(result));
    return BindingMutation{std::move(mutation.value), accepted(std::move(mutation.operation)),
                           std::nullopt, std::nullopt};
  }

  Outcome<AdapterRegistrationResult>
  register_adapter(const AdapterRegistrationCommand& request) override {
    if (!canonical(request.request_id) || !canonical(request.trace_id) ||
        !positive_time(request.deadline) || !canonical(request.adapter.value) ||
        !canonical(request.authentication.subject.value)) {
      return invalid_runtime_input();
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
      auto now_result = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&now_result)) {
        return *failure;
      }
      auto now = std::get<TimePoint>(now_result);
      if (now >= request.deadline) {
        return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
      }
      if (const auto failure =
              status_failure(validate_authentication(request.authentication, now))) {
        return *failure;
      }
      // The direct peer must be the adapter resource being registered under the same authenticated
      // subject; a platform-service or another adapter identity cannot register this adapter.
      const auto* adapter_peer = std::get_if<AdapterPeer>(&request.authentication.direct_peer);
      if (!adapter_peer || adapter_peer->adapter != request.adapter ||
          adapter_peer->subject != request.authentication.subject) {
        return permission_denied();
      }
      ResourceVersion authorization_version{1};
      if (!request.bootstrap_authenticated) {
        // Finish the original time/authentication/peer preflight before the remote check.
        work->rollback();
        work.reset();
        const auto connected =
            authorize(authorization_, request.authentication, Permission::AdapterConnect,
                      "RuntimeService.RegisterAdapter",
                      {ResourceRef{ResourceKind::Adapter, request.adapter}}, request.deadline);
        if (const auto* failure = std::get_if<Failure>(&connected)) {
          return *failure;
        }
        authorization_version =
            std::get<RuntimeAuthorizationDecision>(connected).authorization_version;
        // Identity may have waited while the owner state changed. Reopen and read all resource
        // facts below from a new UoW; never reuse a pre-authorization registration snapshot.
        begun = transactions_.begin();
        if (const auto* failure = std::get_if<Failure>(&begun)) {
          return *failure;
        }
        work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
        if (!work) {
          return internal_failure();
        }
        now_result = work->current_time();
        if (const auto* failure = std::get_if<Failure>(&now_result)) {
          return *failure;
        }
        now = std::get<TimePoint>(now_result);
        if (now >= request.deadline) {
          return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
        }
        if (const auto failure =
                status_failure(validate_authentication(request.authentication, now))) {
          return *failure;
        }
      }
      if (request.declared_accounts.size() > kMaximumAdapterAccountCount) {
        return invalid_runtime_input();
      }
      std::set<std::string> declared;
      for (const auto& account : request.declared_accounts) {
        if (!declared.insert(account.value).second) {
          return invalid_runtime_input();
        }
      }
      auto effective =
          negotiate_capabilities(request.metadata.declared_capabilities, supported_capabilities_);
      if (const auto* failure = std::get_if<Failure>(&effective)) {
        return *failure;
      }
      AdapterRegistrationMetadata metadata = request.metadata;
      metadata.effective_capabilities = std::get<std::vector<Capability>>(effective);
      auto current = work->adapters().find(request.adapter);
      if (const auto* failure = std::get_if<Failure>(&current)) {
        return *failure;
      }
      auto current_record = std::move(std::get<std::optional<AdapterRecord>>(current));
      const auto expected_version =
          current_record ? std::optional<ResourceVersion>{current_record->version} : std::nullopt;
      // P06 external bridge: the bootstrap resource id is the assigned adapter instance; the
      // first registration creates the record under that identity and a reconnect keeps it.
      std::optional<InstanceId> allocated_instance;
      if (current_record) {
        allocated_instance = current_record->current.instance;
      } else if (request.assigned_instance) {
        allocated_instance = request.assigned_instance;
      } else {
        auto allocated = ids_.adapter_instance_id();
        if (const auto* failure = std::get_if<Failure>(&allocated)) {
          return *failure;
        }
        allocated_instance = std::get<InstanceId>(allocated);
      }
      auto allocated_connection = ids_.connection_id();
      if (const auto* failure = std::get_if<Failure>(&allocated_connection)) {
        return *failure;
      }
      std::optional<RuntimeEpoch> epoch = request.runtime_epoch;
      if (!epoch) {
        auto sampled = ids_.runtime_epoch();
        if (const auto* failure = std::get_if<Failure>(&sampled)) {
          return *failure;
        }
        epoch = std::get<RuntimeEpoch>(sampled);
      }
      // Declared accounts are validated before any registration effect: they must exist, be
      // active, and not currently bound to another adapter instance. Registration never creates
      // bindings; binding is an explicit pre-existing management command.
      const InstanceId target_instance = *allocated_instance;
      if (request.assigned_instance && *request.assigned_instance != target_instance) {
        return permission_denied();
      }
      if (request.bootstrap_authenticated && request.presented_instance &&
          *request.presented_instance != target_instance) {
        return permission_denied();
      }
      for (const auto& account_id : request.declared_accounts) {
        if (!canonical_id(account_id.value)) {
          return invalid_runtime_input();
        }
        auto account = work->accounts().find(account_id);
        if (const auto* failure = std::get_if<Failure>(&account)) {
          return *failure;
        }
        const auto account_value = std::get<std::optional<BotAccount>>(account);
        if (!account_value) {
          return runtime_not_found();
        }
        if (account_value->state != LifecycleState::Active) {
          return runtime_conflict();
        }
        auto binding = work->adapter_bindings().find(account_id);
        if (const auto* failure = std::get_if<Failure>(&binding)) {
          return *failure;
        }
        const auto binding_value = std::get<std::optional<AdapterBindingRecord>>(binding);
        if (binding_value && binding_value->state == LifecycleState::Active &&
            binding_value->adapter_instance != target_instance) {
          return runtime_conflict();
        }
        // The bootstrap connection must own exactly the bindings it declares; the binding is the
        // pre-registration authority created by Control and cannot be claimed by another instance.
        if (request.bootstrap_authenticated &&
            (!binding_value || binding_value->state != LifecycleState::Active ||
             binding_value->adapter_instance != target_instance)) {
          return runtime_conflict();
        }
      }
      if (request.bootstrap_authenticated) {
        auto bound = work->active_bindings(target_instance);
        if (const auto* failure = std::get_if<Failure>(&bound)) {
          return *failure;
        }
        if (std::get<std::size_t>(bound) != request.declared_accounts.size()) {
          return runtime_conflict();
        }
      }
      // The domain rule treats a presented instance on the first registration as a reconnect
      // claim; for a bootstrap first registration the assigned identity above is authoritative.
      std::optional<InstanceId> presented_instance = request.presented_instance;
      if (!current_record && request.bootstrap_authenticated) {
        presented_instance = std::nullopt;
      }
      AdapterRegistrationRequest registration{request.adapter,
                                              std::move(metadata),
                                              std::move(presented_instance),
                                              *epoch,
                                              target_instance,
                                              std::get<ConnectionId>(allocated_connection),
                                              now};
      auto accepted = accept_adapter_registration(std::move(current_record), registration);
      if (const auto* failure = std::get_if<Failure>(&accepted)) {
        return *failure;
      }
      const auto& adapter = std::get<AdapterRecord>(accepted);
      const auto saved = work->adapters().save(adapter, expected_version);
      if (const auto* failure = std::get_if<Failure>(&saved)) {
        return *failure;
      }
      const auto audited = append_peer_audit(
          *work, request.authentication.subject, request.request_id, request.trace_id,
          "RuntimeService.RegisterAdapter", ResourceRef{ResourceKind::Adapter, request.adapter},
          authorization_version, std::nullopt, now);
      if (const auto* failure = std::get_if<Failure>(&audited)) {
        return *failure;
      }
      const auto committed = work->commit();
      if (committed != CommitResult::Committed) {
        return map_commit(committed);
      }
      return AdapterRegistrationResult{adapter, adapter.registration.effective_capabilities,
                                       adapter.current, 30};
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<HeartbeatResult> heartbeat(const HeartbeatCommand& request) override {
    if (!canonical(request.request_id) || !canonical(request.trace_id) ||
        !positive_time(request.deadline)) {
      return invalid_runtime_input();
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
      auto now_result = work->current_time();
      if (const auto* failure = std::get_if<Failure>(&now_result)) {
        return *failure;
      }
      const auto now = std::get<TimePoint>(now_result);
      if (now >= request.deadline) {
        return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
      }
      if (const auto failure =
              status_failure(validate_authentication(request.authentication, now))) {
        return *failure;
      }
      auto found = work->adapters().by_instance(request.fence.instance);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      auto current = std::move(std::get<std::optional<AdapterRecord>>(found));
      if (!current) {
        return not_found();
      }
      // Heartbeat has requires_online_authorization=false, but the direct peer binding must still
      // resolve to this exact adapter resource under the authenticated subject; the current
      // registration/generation is authoritative.
      const auto* adapter_peer = std::get_if<AdapterPeer>(&request.authentication.direct_peer);
      if (!adapter_peer || adapter_peer->adapter != current->adapter ||
          adapter_peer->subject != request.authentication.subject) {
        return permission_denied();
      }
      if (const auto failure = status_failure(validate_heartbeat(*current, request.fence, now))) {
        return *failure;
      }
      auto updated = accept_heartbeat(*current, now);
      if (const auto* failure = std::get_if<Failure>(&updated)) {
        return *failure;
      }
      const auto& record = std::get<AdapterRecord>(updated);
      const auto saved = work->adapters().save(record, current->version);
      if (const auto* failure = std::get_if<Failure>(&saved)) {
        return *failure;
      }
      const auto audited = append_peer_audit(
          *work, request.authentication.subject, request.request_id, request.trace_id,
          "RuntimeService.Heartbeat", ResourceRef{ResourceKind::Adapter, record.adapter},
          request.authentication.authorization_version, std::nullopt, now);
      if (const auto* failure = std::get_if<Failure>(&audited)) {
        return *failure;
      }
      const auto committed = work->commit();
      if (committed != CommitResult::Committed) {
        return map_commit(committed);
      }
      return HeartbeatResult{record.current, record.last_heartbeat_at};
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<AdapterRecord> get_adapter(const AdapterQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    const ResourceRef resource{ResourceKind::Adapter, request.adapter};
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AdapterRead,
                  request.context.method, {resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<AdapterRecord>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<AdapterRecord> {
          auto found = work.adapters().find(request.adapter);
          if (const auto* failure = std::get_if<Failure>(&found)) {
            return *failure;
          }
          auto value = std::move(std::get<std::optional<AdapterRecord>>(found));
          if (!value) {
            return not_found();
          }
          return std::move(*value);
        });
  }

  Outcome<ListResult<AdapterRecord>> list_adapters(const AdapterListQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.page.page_size == 0 || request.page.page_size > kMaximumPageSize) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize_catalog(authorization_, resource_catalogs_, request.context,
                          ResourceKind::Adapter, request.catalog, Permission::AdapterRead);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<AdapterRecord>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<AdapterRecord>> {
          return pages_.list_page<AdapterRecord>(
              work, request.context, request.page, "adapters|" + request.catalog.id.value,
              authorization_version, now, Permission::AdapterRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_adapters(limit, after);
              },
              [](const AdapterRecord& value) {
                return ResourceRef{ResourceKind::Adapter, value.adapter};
              },
              [](const AdapterRecord& value) -> const std::string& { return value.adapter.value; });
        },
        true);
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  RuntimeAuthorizationPort& authorization_;
  SnapshotPages& pages_;
  const std::map<ResourceKind, ResourceId> resource_catalogs_;
  const std::vector<std::string> supported_capabilities_;
};
} // namespace

std::unique_ptr<RuntimeAccountAdapterApplication>
make_account_adapter(const RuntimeResourceDependencies& dependencies, SnapshotPages& pages) {
  return std::make_unique<RuntimeAccountAdapterApplicationImpl>(dependencies, pages);
}

} // namespace kiarabot::core::target::runtime_resources_detail
