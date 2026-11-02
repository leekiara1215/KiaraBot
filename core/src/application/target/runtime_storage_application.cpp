#include "kiarabot/core/application/target/runtime_storage_application.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kiarabot/core/domain/target/persistence.hpp"
#include "kiarabot/core/domain/target/runtime_rules.hpp"

namespace kiarabot::core::target {
namespace {

Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
Failure invalid_input() {
  return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure internal_failure() {
  return failure(ErrorCode::Internal);
}
Failure unknown_result() {
  return failure(ErrorCode::Internal, Recovery::QuerySameIdentity);
}
Failure unavailable() {
  return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
Failure permission_denied() {
  return failure(ErrorCode::PermissionDenied, Recovery::ReadCurrent);
}
Failure not_found() {
  return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
}
Failure conflict() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}
Failure rate_limited() {
  return failure(ErrorCode::RateLimited, Recovery::Backoff);
}
bool positive_time(TimePoint value) {
  return value.time_since_epoch().count() > 0;
}
bool canonical(const std::string& value) {
  return canonical_id(value);
}
TimePoint app_now() {
  return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
std::optional<Failure> status_failure(Status status) {
  if (const auto* value = std::get_if<Failure>(&status)) {
    return *value;
  }
  return std::nullopt;
}
bool valid_authentication(const AuthenticationContext& authentication, TimePoint now) {
  return authentication.audience == Service::Runtime && canonical(authentication.subject.value) &&
         authentication.authorization_version.value != 0 &&
         positive_time(authentication.verified_at) && positive_time(authentication.expires_at) &&
         authentication.verified_at <= authentication.expires_at &&
         now >= authentication.verified_at && now < authentication.expires_at;
}
Status validate_storage_command(const StorageCommandContext& context, bool mutating) {
  if (!canonical(context.request_id) || !canonical(context.trace_id) || context.method.empty() ||
      context.method.size() > 128 || !positive_time(context.deadline) ||
      !canonical(context.instance.value)) {
    return invalid_input();
  }
  if (mutating && (context.idempotency_key.size() < 16 || context.idempotency_key.size() > 128)) {
    return invalid_input();
  }
  if (!valid_authentication(context.authentication, app_now())) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  return std::monostate{};
}
Status validate_management_context(const CommandContext& context, bool mutating) {
  if (!canonical(context.request_id) || !canonical(context.trace_id) || context.method.empty() ||
      context.method.size() > 128 || !positive_time(context.deadline)) {
    return invalid_input();
  }
  if (mutating && (context.idempotency_key.size() < 16 || context.idempotency_key.size() > 128)) {
    return invalid_input();
  }
  if (!valid_authentication(context.authentication, app_now())) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  return std::monostate{};
}
Outcome<RuntimeAuthorizationDecision> authorize(RuntimeAuthorizationPort& port,
                                                const AuthenticationContext& authentication,
                                                Permission permission, std::string method,
                                                const std::vector<ResourceRef>& scope,
                                                TimePoint deadline) {
  auto expected_scope = scope;
  auto checked =
      port.check({authentication, permission, std::move(method), expected_scope, deadline});
  if (const auto* error = std::get_if<Failure>(&checked)) {
    return *error;
  }
  auto decision = std::move(std::get<RuntimeAuthorizationDecision>(checked));
  if (!decision.allowed) {
    return decision.denial ? *decision.denial : permission_denied();
  }
  const auto sampled = app_now();
  if (decision.subject != authentication.subject || decision.permission != permission ||
      decision.scope != scope || decision.authorization_version.value == 0 ||
      !positive_time(decision.checked_at) ||
      decision.checked_at > sampled + std::chrono::seconds{2} ||
      sampled - decision.checked_at > std::chrono::seconds{60}) {
    return internal_failure();
  }
  return decision;
}
ResourceRef authorization_scope(const NamespaceScope& scope) {
  if (const auto* shared = std::get_if<SharedNamespace>(&scope)) {
    return {ResourceKind::Installation, ResourceId{shared->installation.value}};
  }
  return {ResourceKind::Instance, ResourceId{std::get<PrivateNamespace>(scope).instance.value}};
}
std::string namespace_id(const NamespaceScope& scope) {
  if (const auto* shared = std::get_if<SharedNamespace>(&scope)) {
    return shared->shared_namespace.value;
  }
  return std::get<PrivateNamespace>(scope).instance.value;
}
ResourceRef namespace_resource(const NamespaceScope& scope) {
  return {ResourceKind::Namespace, ResourceId{namespace_id(scope)}};
}
ResourceVersion next_version(ResourceVersion current) {
  return ResourceVersion{current.value + 1};
}
CompareCondition observed_condition(bool present, const std::optional<NamespaceValue>& current) {
  if (!present || !current) {
    return ExpectedAbsent{};
  }
  return current->version;
}
struct ResolvedScope {
  NamespaceScope scope;
  InstallationId installation;
  std::optional<SharedNamespaceRecord> shared;
};
Outcome<ResolvedScope> resolve_scope(RuntimeUnitOfWork& work,
                                     const StorageCommandContext& context) {
  auto instance = work.instances().find(context.instance);
  if (const auto* error = std::get_if<Failure>(&instance)) {
    return *error;
  }
  const auto& plugin = std::get<std::optional<PluginInstance>>(instance);
  if (!plugin || plugin->state != LifecycleState::Active) {
    return not_found();
  }
  if (context.requested == RequestedScopeKind::DefaultPrivate) {
    return ResolvedScope{PrivateNamespace{context.instance}, plugin->installation, std::nullopt};
  }
  auto shared = work.shared_namespaces().by_installation(plugin->installation);
  if (const auto* error = std::get_if<Failure>(&shared)) {
    return *error;
  }
  const auto& record = std::get<std::optional<SharedNamespaceRecord>>(shared);
  if (!record) {
    return not_found();
  }
  if (record->disabled) {
    return conflict();
  }
  if (context.requested == RequestedScopeKind::InstallationSharedId &&
      context.shared_namespace_id != record->namespace_id) {
    return permission_denied();
  }
  return ResolvedScope{SharedNamespace{plugin->installation, record->namespace_id},
                       plugin->installation, record};
}
Outcome<bool> member_allowed(RuntimeUnitOfWork& work, const ResolvedScope& resolved,
                             const InstanceId& instance, bool write) {
  const auto* shared = std::get_if<SharedNamespace>(&resolved.scope);
  if (!shared) {
    return true;
  }
  auto members = work.shared_namespaces().members(shared->shared_namespace);
  if (const auto* error = std::get_if<Failure>(&members)) {
    return *error;
  }
  const auto& list = std::get<std::vector<SharedMemberRecord>>(members);
  return (write ? member_can_write(list, instance) : member_can_read(list, instance))
             ? Outcome<bool>{true}
             : Outcome<bool>{permission_denied()};
}
Outcome<StorageEntryResult> get_entry(RuntimeUnitOfWork& work, const NamespaceScope& scope,
                                      const std::string& key, TimePoint now) {
  auto found = work.namespaces().get(scope, key, now);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& value = std::get<std::optional<NamespaceValue>>(found);
  if (!value || expired(*value, now)) {
    return not_found();
  }
  return StorageEntryResult{scope, *value};
}
struct WritePlan {
  NamespaceScope scope;
  InstallationId installation;
  CompareCondition expected;
  NamespaceValue value;
  NamespaceQuotaChange quota;
};
Outcome<WritePlan> plan_write(RuntimeUnitOfWork& work, const ResolvedScope& resolved,
                              const std::string& key, std::vector<std::byte> bytes,
                              std::optional<TimePoint> expires_at,
                              std::optional<CompareCondition> condition, bool cas, TimePoint now) {
  if (!valid_storage_key(key) || !valid_storage_expiry(expires_at, now)) {
    return invalid_input();
  }
  if (!valid_storage_value(bytes.size())) {
    return rate_limited();
  }
  const bool shared = std::holds_alternative<SharedNamespace>(resolved.scope);
  if (!shared && condition && !cas) {
    return invalid_input();
  }
  if (shared && !condition) {
    return invalid_input();
  }
  if (cas && !condition) {
    return invalid_input();
  }
  auto current = work.namespaces().get(resolved.scope, key, now);
  if (const auto* error = std::get_if<Failure>(&current)) {
    return *error;
  }
  const auto& existing = std::get<std::optional<NamespaceValue>>(current);
  const bool present = existing.has_value() && !expired(*existing, now);
  std::optional<CompareCondition> expected;
  if (condition) {
    if (const auto* version = std::get_if<ResourceVersion>(&*condition)) {
      if (!present || *version != existing->version) {
        return conflict();
      }
      expected = *version;
    } else {
      if (present) {
        return conflict();
      }
      expected = ExpectedAbsent{};
    }
  } else {
    expected = observed_condition(present, existing);
  }
  NamespaceValue value{std::move(bytes), ResourceVersion{1}, expires_at};
  if (present) {
    value.version = next_version(existing->version);
  }
  const auto keys_delta = present ? 0 : 1;
  const auto bytes_delta = static_cast<std::int64_t>(value.bytes.size()) -
                           (present ? static_cast<std::int64_t>(existing->bytes.size()) : 0);
  return WritePlan{resolved.scope,
                   resolved.installation,
                   *expected,
                   std::move(value),
                   {keys_delta, bytes_delta}};
}
Outcome<std::optional<NamespaceReceipt>>
read_receipt(RuntimeUnitOfWork& work, const ReceiptKey& key, const Sha256Digest& fingerprint) {
  auto found = work.storage_receipts().find(key);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& stored = std::get<std::optional<NamespaceReceipt>>(found);
  if (stored && stored->fingerprint != fingerprint) {
    return conflict();
  }
  return found;
}
Outcome<NamespaceReceiptWrite> store_receipt(RuntimeUnitOfWork& work,
                                             const NamespaceReceipt& receipt) {
  auto written = work.storage_receipts().insert(receipt, NamespaceReceiptCapacity{});
  if (const auto* error = std::get_if<Failure>(&written)) {
    return *error;
  }
  return std::get<NamespaceReceiptWrite>(written);
}
Outcome<NamespaceReceipt> make_receipt(const ReceiptKey& key, const Sha256Digest& fingerprint,
                                       NamespaceReceiptResult result, const InstanceId& instance,
                                       const InstallationId& installation, TimePoint now) {
  const auto retain = now + kMinimumReceiptRetention;
  if (!positive_time(now) || retain <= now) {
    return internal_failure();
  }
  return NamespaceReceipt{key, fingerprint, std::move(result), instance, installation, now, retain};
}
Status append_audit(RuntimeUnitOfWork& work, const SubjectId& actor, const ResourceRef& resource,
                    const std::string& request_id, const std::string& trace_id,
                    const std::string& method, ResourceVersion authorization_version,
                    TimePoint now) {
  return work.journal().append_audit(
      {actor, resource, request_id, trace_id, method, std::nullopt, authorization_version, now});
}
StorageEntryResult entry_from_receipt(const StorageMutationReceipt& receipt) {
  NamespaceValue value{receipt.value, receipt.version.value_or(ResourceVersion{1}),
                       receipt.expires_at};
  return StorageEntryResult{receipt.scope, std::move(value)};
}
ReceiptKey plugin_receipt_key(const StorageCommandContext& context) {
  return {context.authentication.subject,
          context.method,
          {ResourceKind::Instance, ResourceId{context.instance.value}},
          context.idempotency_key};
}
ReceiptKey namespace_receipt_key(const CommandContext& context, const ResourceRef& resource) {
  return {context.authentication.subject, context.method, resource, context.idempotency_key};
}
Outcome<OperationRef> restore_operation(RuntimeUnitOfWork& work, const std::string& request_id) {
  auto found = work.operations().find(OperationId{request_id});
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& operation = std::get<std::optional<Operation>>(found);
  if (!operation) {
    return internal_failure();
  }
  return operation->ref;
}
Outcome<OperationRef> record_operation(RuntimeUnitOfWork& work, RuntimeIdPort& ids,
                                       const ResourceRef& resource) {
  auto id = ids.operation_id();
  if (const auto* error = std::get_if<Failure>(&id)) {
    return *error;
  }
  const OperationRef ref{Service::Runtime, std::get<OperationId>(id), resource};
  const Operation operation{
      ref, {1}, OperationState::Accepted, ExecutionOutcome::NotStarted, std::nullopt, std::nullopt};
  auto saved = work.operations().save(operation, std::nullopt);
  if (const auto* error = std::get_if<Failure>(&saved)) {
    return *error;
  }
  return ref;
}

class Application final : public RuntimeStorageApplication {
public:
  explicit Application(RuntimeStorageDependencies dependencies)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        authorization_(dependencies.authorization) {}

  Outcome<StorageEntryResult> get_storage(const StorageGetRequest& request) override {
    try {
      if (const auto error = status_failure(validate_storage_command(request.context, false))) {
        return *error;
      }
      if (!valid_storage_key(request.key)) {
        return invalid_input();
      }
      NamespaceScope scope;
      {
        auto opened = open_read(request.context, false);
        if (const auto* error = std::get_if<Failure>(&opened)) {
          return *error;
        }
        auto& work = *std::get<std::unique_ptr<RuntimeUnitOfWork>>(opened);
        auto resolved = resolve_scope(work, request.context);
        if (const auto* error = std::get_if<Failure>(&resolved)) {
          return *error;
        }
        scope = std::get<ResolvedScope>(resolved).scope;
      }
      if (auto denied = authorize_service(request.context, scope, Permission::StorageNamespace);
          std::holds_alternative<Failure>(denied)) {
        return std::get<Failure>(denied);
      }
      auto opened = transactions_.begin();
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(opened));
      if (!work) {
        return internal_failure();
      }
      auto resolved = resolve_scope(*work, request.context);
      if (const auto* error = std::get_if<Failure>(&resolved)) {
        return *error;
      }
      auto allowed =
          member_allowed(*work, std::get<ResolvedScope>(resolved), request.context.instance, false);
      if (const auto* error = std::get_if<Failure>(&allowed)) {
        return *error;
      }
      auto now = work->current_time();
      if (const auto* error = std::get_if<Failure>(&now)) {
        return *error;
      }
      return get_entry(*work, std::get<ResolvedScope>(resolved).scope, request.key,
                       std::get<TimePoint>(now));
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<StorageEntryResult> set_storage(const StorageSetRequest& request) override {
    try {
      if (const auto error = status_failure(validate_storage_command(request.context, true))) {
        return *error;
      }
      auto written = write_value(request.context, request.key, request.value, request.expires_at,
                                 request.condition, false);
      if (const auto* error = std::get_if<Failure>(&written)) {
        return *error;
      }
      return std::get<StorageEntryResult>(written);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<StorageEntryResult>
  compare_and_set_storage(const StorageCompareAndSetRequest& request) override {
    try {
      if (const auto error = status_failure(validate_storage_command(request.context, true))) {
        return *error;
      }
      auto written = write_value(request.context, request.key, request.value, request.expires_at,
                                 request.condition, true);
      if (const auto* error = std::get_if<Failure>(&written)) {
        return *error;
      }
      return std::get<StorageEntryResult>(written);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<StorageDeleteResult> delete_storage(const StorageDeleteRequest& request) override {
    try {
      if (const auto error = status_failure(validate_storage_command(request.context, true))) {
        return *error;
      }
      if (!valid_storage_key(request.key)) {
        return invalid_input();
      }
      auto opened = open_read(request.context, true);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& probe = *std::get<std::unique_ptr<RuntimeUnitOfWork>>(opened);
      auto resolved_probe = resolve_scope(probe, request.context);
      if (const auto* error = std::get_if<Failure>(&resolved_probe)) {
        return *error;
      }
      if (auto denied =
              authorize_service(request.context, std::get<ResolvedScope>(resolved_probe).scope,
                                Permission::StorageNamespace);
          std::holds_alternative<Failure>(denied)) {
        return std::get<Failure>(denied);
      }
      auto begun = transactions_.begin();
      if (const auto* error = std::get_if<Failure>(&begun)) {
        return *error;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto now = work->current_time();
      if (const auto* error = std::get_if<Failure>(&now)) {
        return *error;
      }
      auto resolved = resolve_scope(*work, request.context);
      if (const auto* error = std::get_if<Failure>(&resolved)) {
        return *error;
      }
      auto allowed =
          member_allowed(*work, std::get<ResolvedScope>(resolved), request.context.instance, true);
      if (const auto* error = std::get_if<Failure>(&allowed)) {
        return *error;
      }
      const auto& scope = std::get<ResolvedScope>(resolved).scope;
      const auto key = plugin_receipt_key(request.context);
      auto replay = read_receipt(*work, key, request.context.fingerprint);
      if (const auto* error = std::get_if<Failure>(&replay)) {
        return *error;
      }
      if (const auto& stored = std::get<std::optional<NamespaceReceipt>>(replay)) {
        const auto* result = std::get_if<StorageMutationReceipt>(&stored->result);
        if (!result) {
          return internal_failure();
        }
        return StorageDeleteResult{result->scope, result->key, result->deleted, result->version};
      }
      if (std::holds_alternative<SharedNamespace>(scope) && !request.condition) {
        return invalid_input();
      }
      auto current = work->namespaces().get(scope, request.key, std::get<TimePoint>(now));
      if (const auto* error = std::get_if<Failure>(&current)) {
        return *error;
      }
      const auto& existing = std::get<std::optional<NamespaceValue>>(current);
      const bool present = existing.has_value() && !expired(*existing, std::get<TimePoint>(now));
      StorageDeleteResult result{scope, request.key, false, std::nullopt};
      if (present) {
        if (request.condition) {
          if (const auto* version = std::get_if<ResourceVersion>(&*request.condition)) {
            if (*version != existing->version) {
              return conflict();
            }
          } else {
            return conflict();
          }
        }
        auto expected = observed_condition(present, existing);
        // Reserve the release BEFORE removing the row so the same-UoW quota read still observes
        // the value being deleted; the conditional remove and quota change commit atomically.
        auto released =
            work->namespace_quota().admit(scope, std::get<ResolvedScope>(resolved).installation,
                                          {-1, -static_cast<std::int64_t>(existing->bytes.size())});
        if (const auto* error = std::get_if<Failure>(&released)) {
          return *error;
        }
        auto removed = work->namespaces().compare_remove(scope, request.key, expected,
                                                         std::get<TimePoint>(now));
        if (const auto* error = std::get_if<Failure>(&removed)) {
          return *error;
        }
        result.deleted = true;
        result.version = existing->version;
      } else if (request.condition) {
        return conflict();
      }
      auto receipt =
          make_receipt(key, request.context.fingerprint,
                       StorageMutationReceipt{
                           scope, request.key, result.deleted, result.version, std::nullopt, {}},
                       request.context.instance, std::get<ResolvedScope>(resolved).installation,
                       std::get<TimePoint>(now));
      if (const auto* error = std::get_if<Failure>(&receipt)) {
        return *error;
      }
      auto stored = store_receipt(*work, std::get<NamespaceReceipt>(receipt));
      if (const auto* error = std::get_if<Failure>(&stored)) {
        return *error;
      }
      if (const auto failure = status_failure(append_audit(
              *work, request.context.authentication.subject, namespace_resource(scope),
              request.context.request_id, request.context.trace_id, request.context.method,
              request.context.authentication.authorization_version, std::get<TimePoint>(now)))) {
        return *failure;
      }
      if (work->commit() != CommitResult::Committed) {
        return unknown_result();
      }
      return result;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<NamespaceMutation>
  create_shared_namespace(const CreateSharedNamespaceRequest& request) override {
    try {
      if (const auto error = status_failure(validate_management_context(request.context, true))) {
        return *error;
      }
      if (!canonical(request.installation.value) ||
          (request.expected_version && request.expected_version->value == 0)) {
        return invalid_input();
      }
      const ResourceRef scope{ResourceKind::Installation, ResourceId{request.installation.value}};
      if (const auto error = status_failure(validate_management_context(request.context, true))) {
        return *error;
      }
      auto decision =
          authorize(authorization_, request.context.authentication, Permission::NamespaceManage,
                    request.context.method, {scope}, request.context.deadline);
      if (const auto* error = std::get_if<Failure>(&decision)) {
        return *error;
      }
      auto begun = transactions_.begin();
      if (const auto* error = std::get_if<Failure>(&begun)) {
        return *error;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto now = work->current_time();
      if (const auto* error = std::get_if<Failure>(&now)) {
        return *error;
      }
      const auto key = namespace_receipt_key(
          request.context, {ResourceKind::Installation, ResourceId{request.installation.value}});
      auto replay = read_receipt(*work, key, request.context.fingerprint);
      if (const auto* error = std::get_if<Failure>(&replay)) {
        return *error;
      }
      if (const auto& stored = std::get<std::optional<NamespaceReceipt>>(replay)) {
        const auto* result = std::get_if<SharedNamespaceReceipt>(&stored->result);
        if (!result) {
          return internal_failure();
        }
        return NamespaceMutation{result->namespace_record, std::nullopt};
      }
      auto installation = work->installations().find(request.installation);
      if (const auto* error = std::get_if<Failure>(&installation)) {
        return *error;
      }
      if (!std::get<std::optional<PluginInstallation>>(installation)) {
        return not_found();
      }
      auto existing = work->shared_namespaces().by_installation(request.installation);
      if (const auto* error = std::get_if<Failure>(&existing)) {
        return *error;
      }
      const auto& current = std::get<std::optional<SharedNamespaceRecord>>(existing);
      if (current) {
        if (request.expected_version && current->version == *request.expected_version) {
          return NamespaceMutation{*current, std::nullopt};
        }
        return conflict();
      }
      if (request.expected_version && request.expected_version->value != 0) {
        return conflict();
      }
      auto operation_id = ids_.operation_id();
      if (const auto* error = std::get_if<Failure>(&operation_id)) {
        return *error;
      }
      const ResourceId namespace_value{std::get<OperationId>(operation_id).value};
      SharedNamespaceRecord record{namespace_value,          request.installation,    {1}, false,
                                   std::get<TimePoint>(now), std::get<TimePoint>(now)};
      auto saved = work->shared_namespaces().save(record, std::nullopt);
      if (const auto* error = std::get_if<Failure>(&saved)) {
        return *error;
      }
      auto operation = record_operation(*work, ids_, {ResourceKind::Namespace, namespace_value});
      if (const auto* error = std::get_if<Failure>(&operation)) {
        return *error;
      }
      auto receipt = make_receipt(key, request.context.fingerprint, SharedNamespaceReceipt{record},
                                  InstanceId{}, request.installation, std::get<TimePoint>(now));
      if (const auto* error = std::get_if<Failure>(&receipt)) {
        return *error;
      }
      auto stored = store_receipt(*work, std::get<NamespaceReceipt>(receipt));
      if (const auto* error = std::get_if<Failure>(&stored)) {
        return *error;
      }
      if (const auto failure = status_failure(append_audit(
              *work, request.context.authentication.subject,
              {ResourceKind::Namespace, namespace_value}, request.context.request_id,
              request.context.trace_id, request.context.method,
              request.context.authentication.authorization_version, std::get<TimePoint>(now)))) {
        return *failure;
      }
      if (work->commit() != CommitResult::Committed) {
        return unknown_result();
      }
      return NamespaceMutation{record, *std::get_if<OperationRef>(&operation)};
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<NamespaceView> get_shared_namespace(const GetSharedNamespaceQuery& request) override {
    try {
      if (const auto error = status_failure(validate_management_context(request.context, false))) {
        return *error;
      }
      if (!canonical(request.namespace_id.value)) {
        return invalid_input();
      }
      auto begun = transactions_.begin();
      if (const auto* error = std::get_if<Failure>(&begun)) {
        return *error;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto found = work->shared_namespaces().find(request.namespace_id);
      if (const auto* error = std::get_if<Failure>(&found)) {
        return *error;
      }
      const auto& record = std::get<std::optional<SharedNamespaceRecord>>(found);
      if (!record) {
        return not_found();
      }
      const ResourceRef scope{ResourceKind::Installation, ResourceId{record->installation.value}};
      auto decision =
          authorize(authorization_, request.context.authentication, Permission::NamespaceRead,
                    request.context.method, {scope}, request.context.deadline);
      if (const auto* error = std::get_if<Failure>(&decision)) {
        return *error;
      }
      auto now = work->current_time();
      if (const auto* error = std::get_if<Failure>(&now)) {
        return *error;
      }
      auto members = work->shared_namespaces().members(request.namespace_id);
      if (const auto* error = std::get_if<Failure>(&members)) {
        return *error;
      }
      auto quota = work->namespace_quota().usage(
          SharedNamespace{record->installation, record->namespace_id}, record->installation);
      if (const auto* error = std::get_if<Failure>(&quota)) {
        return *error;
      }
      NamespaceView view{*record, {}, std::get<NamespaceQuota>(quota)};
      for (const auto& member : std::get<std::vector<SharedMemberRecord>>(members)) {
        view.members.push_back(member.member);
      }
      return view;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<SharedMembersMutation>
  set_shared_members(const SetSharedMembersRequest& request) override {
    try {
      return mutate_members(request.context, request.namespace_id, request.installation,
                            request.members, request.expected_version, false);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<SharedMembersMutation>
  disable_shared_namespace(const DisableSharedNamespaceRequest& request) override {
    try {
      return mutate_members(request.context, request.namespace_id, InstallationId{}, std::nullopt,
                            request.expected_version, true);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

private:
  Outcome<std::unique_ptr<RuntimeUnitOfWork>> open_read(const StorageCommandContext& context,
                                                        bool write) {
    auto begun = transactions_.begin();
    if (const auto* error = std::get_if<Failure>(&begun)) {
      return *error;
    }
    auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    auto resolved = resolve_scope(*work, context);
    if (const auto* error = std::get_if<Failure>(&resolved)) {
      return *error;
    }
    auto allowed =
        member_allowed(*work, std::get<ResolvedScope>(resolved), context.instance, write);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
    return work;
  }
  Outcome<RuntimeAuthorizationDecision> authorize_service(const StorageCommandContext& context,
                                                          const NamespaceScope& scope,
                                                          Permission permission) {
    auto decision = authorize(authorization_, context.authentication, permission, context.method,
                              {authorization_scope(scope)}, context.deadline);
    if (const auto* error = std::get_if<Failure>(&decision)) {
      return *error;
    }
    return std::get<RuntimeAuthorizationDecision>(decision);
  }
  Outcome<StorageEntryResult> write_value(const StorageCommandContext& context,
                                          const std::string& key, std::vector<std::byte> value,
                                          std::optional<TimePoint> expires_at,
                                          std::optional<CompareCondition> condition, bool cas) {
    auto opened = open_read(context, true);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto& probe = *std::get<std::unique_ptr<RuntimeUnitOfWork>>(opened);
    auto probe_resolved = resolve_scope(probe, context);
    if (const auto* error = std::get_if<Failure>(&probe_resolved)) {
      return *error;
    }
    if (auto denied = authorize_service(context, std::get<ResolvedScope>(probe_resolved).scope,
                                        Permission::StorageNamespace);
        std::holds_alternative<Failure>(denied)) {
      return std::get<Failure>(denied);
    }
    auto begun = transactions_.begin();
    if (const auto* error = std::get_if<Failure>(&begun)) {
      return *error;
    }
    auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    auto now = work->current_time();
    if (const auto* error = std::get_if<Failure>(&now)) {
      return *error;
    }
    auto resolved = resolve_scope(*work, context);
    if (const auto* error = std::get_if<Failure>(&resolved)) {
      return *error;
    }
    auto allowed = member_allowed(*work, std::get<ResolvedScope>(resolved), context.instance, true);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
    const auto key_ref = plugin_receipt_key(context);
    auto replay = read_receipt(*work, key_ref, context.fingerprint);
    if (const auto* error = std::get_if<Failure>(&replay)) {
      return *error;
    }
    if (const auto& stored = std::get<std::optional<NamespaceReceipt>>(replay)) {
      const auto* result = std::get_if<StorageMutationReceipt>(&stored->result);
      if (!result) {
        return internal_failure();
      }
      return entry_from_receipt(*result);
    }
    auto plan = plan_write(*work, std::get<ResolvedScope>(resolved), key, std::move(value),
                           expires_at, condition, cas, std::get<TimePoint>(now));
    if (const auto* error = std::get_if<Failure>(&plan)) {
      return *error;
    }
    auto& plan_value = std::get<WritePlan>(plan);
    auto admitted = work->namespace_quota().admit(std::get<ResolvedScope>(resolved).scope,
                                                  std::get<ResolvedScope>(resolved).installation,
                                                  plan_value.quota);
    if (const auto* error = std::get_if<Failure>(&admitted)) {
      return *error;
    }
    auto written = work->namespaces().compare_exchange(std::get<ResolvedScope>(resolved).scope, key,
                                                       plan_value.expected, plan_value.value,
                                                       std::get<TimePoint>(now));
    if (const auto* error = std::get_if<Failure>(&written)) {
      return *error;
    }
    plan_value.value.version = std::get<ResourceVersion>(written);
    auto receipt = make_receipt(
        key_ref, context.fingerprint,
        StorageMutationReceipt{plan_value.scope, key, false, plan_value.value.version,
                               plan_value.value.expires_at, plan_value.value.bytes},
        context.instance, std::get<ResolvedScope>(resolved).installation, std::get<TimePoint>(now));
    if (const auto* error = std::get_if<Failure>(&receipt)) {
      return *error;
    }
    auto stored = store_receipt(*work, std::get<NamespaceReceipt>(receipt));
    if (const auto* error = std::get_if<Failure>(&stored)) {
      return *error;
    }
    if (const auto failure = status_failure(append_audit(
            *work, context.authentication.subject, namespace_resource(plan_value.scope),
            context.request_id, context.trace_id, context.method,
            context.authentication.authorization_version, std::get<TimePoint>(now)))) {
      return *failure;
    }
    if (work->commit() != CommitResult::Committed) {
      return unknown_result();
    }
    return StorageEntryResult{plan_value.scope, plan_value.value};
  }
  Outcome<SharedMembersMutation>
  mutate_members(const CommandContext& context, const ResourceId& namespace_id,
                 const InstallationId& installation,
                 const std::optional<std::vector<SharedMember>>& members,
                 std::optional<ResourceVersion> expected_version, bool disable) {
    if (const auto error = status_failure(validate_management_context(context, true))) {
      return *error;
    }
    if (!canonical(namespace_id.value) || (!disable && !canonical(installation.value)) ||
        (!disable && !valid_shared_members(*members)) ||
        (expected_version && expected_version->value == 0)) {
      return invalid_input();
    }
    auto begun = transactions_.begin();
    if (const auto* error = std::get_if<Failure>(&begun)) {
      return *error;
    }
    auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    auto found = work->shared_namespaces().find(namespace_id);
    if (const auto* error = std::get_if<Failure>(&found)) {
      return *error;
    }
    const auto& record = std::get<std::optional<SharedNamespaceRecord>>(found);
    if (!record || (!disable && record->installation != installation)) {
      return not_found();
    }
    const ResourceRef scope{ResourceKind::Installation, ResourceId{record->installation.value}};
    auto decision = authorize(authorization_, context.authentication, Permission::NamespaceManage,
                              context.method, {scope}, context.deadline);
    if (const auto* error = std::get_if<Failure>(&decision)) {
      return *error;
    }
    auto now = work->current_time();
    if (const auto* error = std::get_if<Failure>(&now)) {
      return *error;
    }
    const auto key = namespace_receipt_key(context, {ResourceKind::Namespace, namespace_id});
    auto replay = read_receipt(*work, key, context.fingerprint);
    if (const auto* error = std::get_if<Failure>(&replay)) {
      return *error;
    }
    if (const auto& stored = std::get<std::optional<NamespaceReceipt>>(replay)) {
      const auto* result = std::get_if<SharedMembersReceipt>(&stored->result);
      if (!result) {
        return internal_failure();
      }
      auto operation = restore_operation(*work, context.request_id);
      if (const auto* error = std::get_if<Failure>(&operation)) {
        return *error;
      }
      return SharedMembersMutation{result->version, std::get<OperationRef>(operation)};
    }
    if (record->disabled) {
      return conflict();
    }
    if (expected_version && *expected_version != record->version) {
      return conflict();
    }
    const auto next = next_version(record->version);
    if (disable) {
      auto updated = *record;
      updated.disabled = true;
      updated.version = next;
      updated.updated_at = std::get<TimePoint>(now);
      auto saved = work->shared_namespaces().save(updated, record->version);
      if (const auto* error = std::get_if<Failure>(&saved)) {
        return *error;
      }
    } else {
      auto saved = work->shared_namespaces().save_members(namespace_id, record->version, *members,
                                                          next, std::get<TimePoint>(now));
      if (const auto* error = std::get_if<Failure>(&saved)) {
        return *error;
      }
    }
    auto operation = record_operation(*work, ids_, {ResourceKind::Namespace, namespace_id});
    if (const auto* error = std::get_if<Failure>(&operation)) {
      return *error;
    }
    auto receipt = make_receipt(key, context.fingerprint, SharedMembersReceipt{next}, InstanceId{},
                                record->installation, std::get<TimePoint>(now));
    if (const auto* error = std::get_if<Failure>(&receipt)) {
      return *error;
    }
    auto stored = store_receipt(*work, std::get<NamespaceReceipt>(receipt));
    if (const auto* error = std::get_if<Failure>(&stored)) {
      return *error;
    }
    if (const auto failure = status_failure(append_audit(
            *work, context.authentication.subject, {ResourceKind::Namespace, namespace_id},
            context.request_id, context.trace_id, context.method,
            context.authentication.authorization_version, std::get<TimePoint>(now)))) {
      return *failure;
    }
    if (work->commit() != CommitResult::Committed) {
      return unknown_result();
    }
    return SharedMembersMutation{next, std::get<OperationRef>(operation)};
  }

  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeAuthorizationPort& authorization_;
};

} // namespace

Outcome<std::unique_ptr<RuntimeStorageApplication>>
make_runtime_storage_application(RuntimeStorageDependencies dependencies) {
  try {
    return std::unique_ptr<RuntimeStorageApplication>{std::make_unique<Application>(dependencies)};
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace kiarabot::core::target
