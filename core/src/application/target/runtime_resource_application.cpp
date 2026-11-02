#include "kiarabot/core/application/target/runtime_resource_application.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "kiarabot/core/application/target/runtime_readiness.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"

namespace kiarabot::core::target {
namespace {

Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose,
                Acceptance acceptance = Acceptance::NotAccepted) {
  return {code, acceptance, recovery, std::nullopt, std::nullopt};
}
Failure internal_failure() {
  return failure(ErrorCode::Internal);
}
Failure unknown_result() {
  return failure(ErrorCode::Internal, Recovery::QuerySameIdentity, Acceptance::Unknown);
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
Failure stale_version() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}

// Converts a pure Status into a copyable failure before a temporary variant expires; callers must
// never hold a pointer into a temporary result.
std::optional<Failure> status_failure(Status status) {
  if (const auto* failure = std::get_if<Failure>(&status)) {
    return *failure;
  }
  return std::nullopt;
}

bool positive_time(TimePoint value) {
  return value.time_since_epoch().count() > 0;
}
// Boundary clock sample used only to judge authorization-decision freshness and command deadlines;
// authoritative business timestamps still come from UoW::current_time().
TimePoint now() {
  return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool canonical(const std::string& value) {
  return canonical_id(value);
}

Status validate_command_context(const CommandContext& context, bool mutating) {
  if (!canonical(context.request_id) || !canonical(context.trace_id) ||
      !positive_time(context.deadline) || context.method.empty() || context.method.size() > 128) {
    return invalid_runtime_input();
  }
  if (mutating && (context.idempotency_key.size() < 16 || context.idempotency_key.size() > 128)) {
    return invalid_runtime_input();
  }
  const auto& authentication = context.authentication;
  if (authentication.audience != Service::Runtime || !canonical(authentication.subject.value) ||
      authentication.authorization_version.value == 0 ||
      !positive_time(authentication.verified_at) || !positive_time(authentication.expires_at) ||
      authentication.verified_at > authentication.expires_at ||
      authentication.expires_at <= authentication.verified_at) {
    return failure(ErrorCode::Unauthenticated, Recovery::Authenticate);
  }
  return std::monostate{};
}

Status validate_authentication(const AuthenticationContext& authentication, TimePoint now) {
  if (authentication.audience != Service::Runtime || !canonical(authentication.subject.value) ||
      authentication.authorization_version.value == 0 ||
      !positive_time(authentication.verified_at) || !positive_time(authentication.expires_at) ||
      now < authentication.verified_at || now >= authentication.expires_at) {
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
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  auto decision = std::move(std::get<RuntimeAuthorizationDecision>(checked));
  if (!decision.allowed) {
    return decision.denial ? *decision.denial : permission_denied();
  }
  const auto sampled = now();
  // Reject a decision that does not bind exactly this call or is not a current online observation.
  // A transport-layer allow that names another subject/permission/scope or replays an old version
  // is an invalid upstream result, never an authorization grant.
  if (decision.subject != authentication.subject || decision.permission != permission ||
      decision.scope != scope || decision.authorization_version.value == 0 ||
      !positive_time(decision.checked_at) ||
      decision.checked_at > sampled + std::chrono::seconds{2} ||
      sampled - decision.checked_at > std::chrono::seconds{60}) {
    return internal_failure();
  }
  return decision;
}

Failure map_commit(CommitResult result) {
  switch (result) {
  case CommitResult::Committed:
    return internal_failure();
  case CommitResult::RolledBack:
    return unavailable();
  case CommitResult::Unknown:
  default:
    return unknown_result();
  }
}

// Every use case owns its transaction lifetime here; exceptions never escape the application.
template <typename Value>
Outcome<Value>
with_work(RuntimeUnitOfWorkFactory& transactions, const CommandContext& context,
          const std::function<Outcome<Value>(RuntimeUnitOfWork&, TimePoint now)>& body) {
  try {
    auto begun = transactions.begin();
    if (const auto* failure = std::get_if<Failure>(&begun)) {
      return *failure;
    }
    auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
    if (!work) {
      return internal_failure();
    }
    auto now = work->current_time();
    if (const auto* failure = std::get_if<Failure>(&now)) {
      return *failure;
    }
    if (std::get<TimePoint>(now) >= context.deadline) {
      work->rollback();
      return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
    }
    auto result = body(*work, std::get<TimePoint>(now));
    if (const auto* failure = std::get_if<Failure>(&result)) {
      work->rollback();
      return *failure;
    }
    work->rollback();
    return std::move(std::get<Value>(result));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

ReceiptKey receipt_key(const CommandContext& context, const ResourceRef& resource) {
  return {context.authentication.subject, context.method, resource, context.idempotency_key};
}

// Idempotent replay: a matching fingerprint returns the original immutable success snapshot; a
// different fingerprint under the same key is CONFLICT. A missing receipt still requires the
// current authorization check the caller already performed.
template <typename Value>
std::optional<Outcome<Value>> replay(RuntimeUnitOfWork& work, const CommandContext& context,
                                     const ResourceRef& resource) {
  auto found = work.receipts().find(receipt_key(context, resource));
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  auto receipt = std::move(std::get<std::optional<CommandReceipt>>(found));
  if (!receipt) {
    return std::nullopt;
  }
  if (receipt->fingerprint != context.fingerprint) {
    return stale_version();
  }
  if (auto* value = std::get_if<Value>(&receipt->result)) {
    return std::optional<Outcome<Value>>(std::in_place, *value);
  }
  return internal_failure();
}

Status append_peer_audit(RuntimeUnitOfWork& work, const SubjectId& actor,
                         const std::string& request_id, const std::string& trace_id,
                         std::string method, const ResourceRef& resource,
                         const ResourceVersion& authorization_version,
                         std::optional<ErrorCode> rejection, TimePoint now) {
  return work.journal().append_audit(AuditEntry{actor, resource, request_id, trace_id,
                                                std::move(method), rejection, authorization_version,
                                                now});
}
Status append_audit(RuntimeUnitOfWork& work, const CommandContext& context,
                    const ResourceRef& resource, const ResourceVersion& authorization_version,
                    std::optional<ErrorCode> rejection, TimePoint now) {
  AuditEntry entry{context.authentication.subject,
                   resource,
                   context.request_id,
                   context.trace_id,
                   context.method,
                   rejection,
                   authorization_version,
                   now};
  return work.journal().append_audit(entry);
}

Status record_receipt(RuntimeUnitOfWork& work, const CommandContext& context,
                      const ResourceRef& resource, ReplayResult result, TimePoint now) {
  CommandReceipt receipt{receipt_key(context, resource), context.fingerprint, std::move(result),
                         now, now + kMinimumReceiptRetention};
  auto written = work.receipts().insert(receipt, ReceiptCapacity{});
  if (const auto* failure = std::get_if<Failure>(&written)) {
    return *failure;
  }
  return std::monostate{};
}

std::optional<OperationAccepted> accepted(std::optional<OperationRef> operation) {
  return operation ? std::optional<OperationAccepted>{OperationAccepted{*operation}} : std::nullopt;
}
template <typename Value> struct Mutation {
  Value value;
  std::optional<OperationRef> operation;
};

// Shared mutation transaction: authorization already succeeded outside the UoW; this helper owns
// receipt replay, one live effect, operation/receipt/audit atomicity and the Unknown commit result.
// The operation reference is durable in the same UoW (id derived from the request identity), so
// both the first response and an idempotent replay restore the original accepted operation
// reference.
template <typename Value>
Outcome<Mutation<Value>>
mutate(RuntimeUnitOfWorkFactory& transactions, const CommandContext& context,
       const ResourceRef& resource, const ResourceVersion& authorization_version,
       const std::function<Outcome<Value>(RuntimeUnitOfWork&, TimePoint now)>& effect,
       const std::function<ResourceRef(const Value&)>& result_resource,
       const std::function<ResourceVersion(const Value&)>& result_version) {
  try {
    auto begun = transactions.begin();
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
    if (now >= context.deadline) {
      return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
    }
    const auto operation_id = OperationId{context.request_id};
    auto restore_operation = [&]() -> std::optional<OperationRef> {
      auto stored = work->operations().find(operation_id);
      if (std::holds_alternative<Failure>(stored)) {
        return std::nullopt;
      }
      const auto operation = std::move(std::get<std::optional<Operation>>(stored));
      return operation ? std::optional<OperationRef>{operation->ref} : std::nullopt;
    };
    auto found = work->receipts().find(receipt_key(context, resource));
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    if (const auto& original = std::get<std::optional<CommandReceipt>>(found)) {
      if (original->fingerprint != context.fingerprint) {
        return stale_version();
      }
      if (const auto* value = std::get_if<Value>(&original->result)) {
        return Mutation<Value>{*value, restore_operation()};
      }
      return internal_failure();
    }
    auto applied = effect(*work, now);
    if (const auto* failure = std::get_if<Failure>(&applied)) {
      if (const auto audited =
              append_audit(*work, context, resource, authorization_version, failure->code, now);
          std::holds_alternative<Failure>(audited)) {
        work->rollback();
        return *failure;
      }
      const auto committed = work->commit();
      if (committed == CommitResult::Unknown) {
        return unknown_result();
      }
      return *failure;
    }
    auto value = std::move(std::get<Value>(applied));
    auto audited = append_audit(*work, context, result_resource(value), authorization_version,
                                std::nullopt, now);
    if (const auto* failure = std::get_if<Failure>(&audited)) {
      return *failure;
    }
    auto receipt = record_receipt(*work, context, resource, ReplayResult(value), now);
    if (const auto* failure = std::get_if<Failure>(&receipt)) {
      return *failure;
    }
    if (result_version(value).value == 0) {
      return internal_failure();
    }
    auto operation = restore_operation();
    const auto committed = work->commit();
    if (committed != CommitResult::Committed) {
      return map_commit(committed);
    }
    return Mutation<Value>{std::move(value), std::move(operation)};
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

template <typename Value> Outcome<Value> require_found(Outcome<std::optional<Value>> found) {
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  auto value = std::move(std::get<std::optional<Value>>(found));
  if (!value) {
    return not_found();
  }
  return std::move(*value);
}

} // namespace

class RuntimeResourceApplicationImpl final : public RuntimeResourceApplication {
public:
  explicit RuntimeResourceApplicationImpl(RuntimeResourceDependencies dependencies)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        secrets_(dependencies.secrets), authorization_(dependencies.authorization),
        supported_capabilities_(std::move(dependencies.supported_capabilities)) {}

  Service service() const noexcept override {
    return Service::Runtime;
  }
  // The application knows only that its business implementation is assembled; database/schema and
  // Identity observations belong to the composition root, so those stay Unverified here and ready
  // remains false. The composition replaces this with runtime_readiness() using real observations.
  ServiceReadiness readiness() const override {
    return evaluate_runtime_readiness(RuntimeReadinessObservation{
        DependencyState::Available, DependencyState::Unverified, DependencyState::Unverified});
  }

  Outcome<BotAccount> create_account(const CreateAccountRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::AccountManage,
                  request.context.method, {}, request.context.deadline);
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
        [](const BotAccount& value) { return value.version; });
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
        authorize(authorization_, request.context.authentication, Permission::AccountRead,
                  request.context.method, {}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<BotAccount>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<BotAccount>> {
          return list_page<BotAccount>(
              work, request.context, request.page, "accounts", authorization_version, now,
              Permission::AccountRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_accounts(limit, after);
              },
              [](const BotAccount& value) {
                return ResourceRef{ResourceKind::Account, value.id};
              },
              [](const BotAccount& value) -> const std::string& { return value.id.value; });
        });
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
          auto issued = issue_bootstrap_credential(work, request.adapter_instance.value, now);
          if (const auto* failure = std::get_if<Failure>(&issued)) {
            return *failure;
          }
          bootstrap_credential = std::move(std::get<std::optional<std::string>>(issued));
          return binding;
        },
        [](const AdapterBindingRecord& value) {
          return ResourceRef{ResourceKind::Account, value.account};
        },
        [](const AdapterBindingRecord& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<AdapterBindingRecord>>(result));
    return BindingMutation{std::move(mutation.value), accepted(std::move(mutation.operation)),
                           std::move(bootstrap_credential)};
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
        [](const AdapterBindingRecord& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<AdapterBindingRecord>>(result));
    return BindingMutation{std::move(mutation.value), accepted(std::move(mutation.operation)),
                           std::nullopt};
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
      const auto now = std::get<TimePoint>(now_result);
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
        const auto connected =
            authorize(authorization_, request.authentication, Permission::AdapterConnect,
                      "RuntimeService.RegisterAdapter",
                      {ResourceRef{ResourceKind::Adapter, request.adapter}}, request.deadline);
        if (const auto* failure = std::get_if<Failure>(&connected)) {
          return *failure;
        }
        authorization_version =
            std::get<RuntimeAuthorizationDecision>(connected).authorization_version;
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
        authorize(authorization_, request.context.authentication, Permission::AdapterRead,
                  request.context.method, {}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<AdapterRecord>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<AdapterRecord>> {
          return list_page<AdapterRecord>(
              work, request.context, request.page, "adapters", authorization_version, now,
              Permission::AdapterRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_adapters(limit, after);
              },
              [](const AdapterRecord& value) {
                return ResourceRef{ResourceKind::Adapter, value.adapter};
              },
              [](const AdapterRecord& value) -> const std::string& { return value.adapter.value; });
        });
  }

  Outcome<ManifestValidation> validate_manifest(const ValidateManifestCommand& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::PluginManage,
                  request.context.method, {}, request.context.deadline);
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
        authorize(authorization_, request.context.authentication, Permission::PluginManage,
                  request.context.method, {}, request.context.deadline);
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
    auto checked = authorize(authorization_, request.context.authentication, Permission::PluginRead,
                             request.context.method, {}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<PluginInstallation>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<PluginInstallation>> {
          return list_page<PluginInstallation>(
              work, request.context, request.page, "installations", authorization_version, now,
              Permission::PluginRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_installations(limit, after);
              },
              [](const PluginInstallation& value) {
                return ResourceRef{ResourceKind::Installation, {value.id.value}};
              },
              [](const PluginInstallation& value) -> const std::string& { return value.id.value; });
        });
  }

  Outcome<ListResult<PluginInstance>> list_instances(const InstanceListQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.page.page_size == 0 || request.page.page_size > kMaximumPageSize) {
      return invalid_runtime_input();
    }
    auto checked = authorize(authorization_, request.context.authentication, Permission::PluginRead,
                             request.context.method, {}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    return with_work<ListResult<PluginInstance>>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ListResult<PluginInstance>> {
          return list_page<PluginInstance>(
              work, request.context, request.page, "instances", authorization_version, now,
              Permission::PluginRead,
              [](RuntimeUnitOfWork& unit, std::size_t limit, const std::string& after) {
                return unit.list_instances(limit, after);
              },
              [](const PluginInstance& value) {
                return ResourceRef{ResourceKind::Instance, {value.id.value}};
              },
              [](const PluginInstance& value) -> const std::string& { return value.id.value; });
        });
  }

  Outcome<InstanceCreation> create_instance(const CreateInstanceRequest& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::PluginManage,
                  request.context.method, {}, request.context.deadline);
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
          if (request.externally_managed) {
            auto issued = issue_bootstrap_credential(work, id.value, now);
            if (const auto* failure = std::get_if<Failure>(&issued)) {
              return *failure;
            }
            bootstrap_credential = std::move(std::get<std::optional<std::string>>(issued));
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
    return InstanceCreation{std::move(mutation.value), std::move(bootstrap_credential)};
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
          return updated;
        },
        [](const PluginInstance& value) {
          return ResourceRef{ResourceKind::Instance, {value.id.value}};
        },
        [](const PluginInstance& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<PluginInstance>>(result));
    return InstanceMutation{std::move(mutation.value), accepted(std::move(mutation.operation))};
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
        [](const InstanceConfiguration& value) { return value.version; });
    if (const auto* failure = std::get_if<Failure>(&result)) {
      return *failure;
    }
    auto mutation = std::move(std::get<Mutation<InstanceConfiguration>>(result));
    return ConfigMutation{std::move(mutation.value), accepted(std::move(mutation.operation))};
  }

  Outcome<ResourceState> get_resource_state(const ResourceStateQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (!canonical(request.resource.id.value)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceRead,
                  request.context.method, {request.resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<ResourceState>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<ResourceState> {
          if (request.resource.kind == ResourceKind::Account) {
            auto found = work.accounts().find(request.resource.id);
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<BotAccount>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = DesiredState::Running;
            state.source_service = Service::Runtime;
            state.observed_at = now;
            return state;
          }
          if (request.resource.kind == ResourceKind::Adapter) {
            auto found = work.adapters().find(request.resource.id);
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<AdapterRecord>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = DesiredState::Running;
            state.adapter_fence = value->current;
            state.registered = adapter_online(*value, now);
            state.observed_at = now;
            state.source_service = Service::Runtime;
            return state;
          }
          if (request.resource.kind == ResourceKind::Installation) {
            auto found = work.installations().find(InstallationId{request.resource.id.value});
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<PluginInstallation>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = DesiredState::Running;
            state.observed_at = now;
            return state;
          }
          if (request.resource.kind == ResourceKind::Instance) {
            auto found = work.instances().find(InstanceId{request.resource.id.value});
            if (const auto* failure = std::get_if<Failure>(&found)) {
              return *failure;
            }
            const auto value = std::move(std::get<std::optional<PluginInstance>>(found));
            if (!value) {
              return not_found();
            }
            ResourceState state;
            state.resource = request.resource;
            state.version = value->version;
            state.lifecycle = value->state;
            state.desired = value->desired;
            state.fence = value->current;
            state.registered = value->current.has_value();
            state.desired_config_version = value->desired_version;
            state.applied_config_version = value->applied_config_version;
            state.observed_at = now;
            state.source_service = Service::Runtime;
            return state;
          }
          return failure(ErrorCode::CapabilityNotSupported);
        });
  }

  Outcome<Operation> get_operation(const OperationQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (request.operation.owner != Service::Runtime || !canonical(request.operation.id.value) ||
        !canonical(request.operation.resource.id.value)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceRead,
                  request.context.method, {request.operation.resource}, request.context.deadline);
    if (const auto* failure = std::get_if<Failure>(&checked)) {
      return *failure;
    }
    return with_work<Operation>(
        transactions_, request.context, [&](RuntimeUnitOfWork& work, TimePoint) {
          return require_found(work.operations().find(request.operation.id));
        });
  }

private:
  // A current-authority read cursor: opaque runtime-assigned id bound to the authenticated subject,
  // query family, authorization version and page size. It never persists a frozen row set (unlike
  // Identity session snapshots) and is never a client-derivable offset.
  struct PageCursor {
    std::string snapshot_id;
    SubjectId subject;
    std::string query;
    ResourceVersion authorization_version;
    std::uint32_t page_size = 0;
    std::string after;
    TimePoint expires_at;
  };
  static constexpr std::size_t kMaximumPageCursors = 1024;
  static constexpr std::size_t kMaximumPageCursorsPerSubject = 64;

  // Cursors are immutable: a page token names exactly one keyset position (after) plus the bound
  // query/subject/authorization-version/page-size/window. A consumed token is never mutated, so a
  // lost-response retry returns the same page instead of skipping rows, and two concurrent callers
  // cannot advance each other's cursor.
  Outcome<PageCursor> begin_page(const CommandContext& context, std::string_view query,
                                 const ResourceVersion& authorization_version,
                                 const PageRequest& page, TimePoint now) {
    std::lock_guard lock(cursors_mutex_);
    if (!page.page_token.empty()) {
      const auto found = cursors_.find(page.page_token);
      if (found == cursors_.end() || found->second.expires_at <= now ||
          found->second.subject != context.authentication.subject || found->second.query != query ||
          found->second.page_size != page.page_size ||
          found->second.authorization_version != authorization_version) {
        return Failure{
            ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::CorrectInput, {}, {}};
      }
      return found->second;
    }
    auto allocated = ids_.snapshot_id();
    if (const auto* failure = std::get_if<Failure>(&allocated)) {
      return *failure;
    }
    PageCursor cursor{std::get<std::string>(allocated),
                      context.authentication.subject,
                      std::string(query),
                      authorization_version,
                      page.page_size,
                      {},
                      now + kSnapshotLifetime};
    prune_cursors_locked(now);
    cursors_[cursor.snapshot_id] = cursor;
    return cursor;
  }
  Outcome<std::string> publish_page(const CommandContext& context, std::string_view query,
                                    const ResourceVersion& authorization_version,
                                    std::uint32_t page_size, std::string after, TimePoint now) {
    auto allocated = ids_.snapshot_id();
    if (const auto* failure = std::get_if<Failure>(&allocated)) {
      return *failure;
    }
    std::string token = std::get<std::string>(allocated);
    std::lock_guard lock(cursors_mutex_);
    prune_cursors_locked(now);
    // Per-subject cap: one subject cannot occupy the whole bounded cursor pool.
    std::size_t count = 0;
    for (const auto& [key, cursor] : cursors_) {
      (void)key;
      if (cursor.subject == context.authentication.subject) {
        ++count;
      }
    }
    while (count >= kMaximumPageCursorsPerSubject) {
      auto oldest = cursors_.end();
      for (auto it = cursors_.begin(); it != cursors_.end(); ++it) {
        if (it->second.subject != context.authentication.subject) {
          continue;
        }
        if (oldest == cursors_.end() || it->second.expires_at < oldest->second.expires_at) {
          oldest = it;
        }
      }
      if (oldest == cursors_.end()) {
        break;
      }
      cursors_.erase(oldest);
      --count;
    }
    cursors_[token] = PageCursor{token,
                                 context.authentication.subject,
                                 std::string(query),
                                 authorization_version,
                                 page_size,
                                 std::move(after),
                                 now + kSnapshotLifetime};
    return token;
  }
  void prune_cursors_locked(TimePoint now) {
    for (auto it = cursors_.begin(); it != cursors_.end();) {
      if (it->second.expires_at <= now) {
        it = cursors_.erase(it);
      } else {
        ++it;
      }
    }
    while (cursors_.size() > kMaximumPageCursors) {
      auto oldest = std::min_element(cursors_.begin(), cursors_.end(),
                                     [](const auto& left, const auto& right) {
                                       return left.second.expires_at < right.second.expires_at;
                                     });
      cursors_.erase(oldest);
    }
  }

  // Reads one current-authority page. `loader` performs an ascending keyset read after the given
  // stable id; every candidate is then checked against current authorization for the exact
  // resource, so a caller only ever sees resources it is currently granted (catalog permission does
  // not grant children). Dependency or authorization faults fail the whole page instead of
  // returning a silently truncated page.
  template <typename Value>
  Outcome<ListResult<Value>>
  list_page(RuntimeUnitOfWork& work, const CommandContext& context, const PageRequest& page,
            std::string_view query, const ResourceVersion& authorization_version, TimePoint now,
            Permission permission,
            const std::function<Outcome<std::vector<Value>>(RuntimeUnitOfWork&, std::size_t,
                                                            const std::string&)>& loader,
            const std::function<ResourceRef(const Value&)>& resource_of,
            const std::function<const std::string&(const Value&)>& id) {
    auto cursor = begin_page(context, query, authorization_version, page, now);
    if (const auto* failure = std::get_if<Failure>(&cursor)) {
      return *failure;
    }
    const auto current_cursor = std::get<PageCursor>(cursor);
    std::vector<Value> visible;
    std::string after = current_cursor.after;
    std::string resume_after = after;
    bool more = false;
    for (int batch = 0; batch < 32; ++batch) {
      auto loaded = loader(work, std::size_t{current_cursor.page_size} + 1, after);
      if (const auto* failure = std::get_if<Failure>(&loaded)) {
        return *failure;
      }
      auto raw = std::move(std::get<std::vector<Value>>(loaded));
      if (raw.empty()) {
        more = false;
        break;
      }
      const bool lookahead = raw.size() > current_cursor.page_size;
      const std::string last_scanned = id(raw.back());
      for (auto& item : raw) {
        if (visible.size() > current_cursor.page_size) {
          break;
        }
        auto allowed = authorize(authorization_, context.authentication, permission, context.method,
                                 {resource_of(item)}, context.deadline);
        if (const auto* failure = std::get_if<Failure>(&allowed)) {
          if (failure->code == ErrorCode::PermissionDenied ||
              failure->code == ErrorCode::NotFound) {
            continue;
          }
          return *failure;
        }
        visible.push_back(std::move(item));
      }
      resume_after = last_scanned;
      if (visible.size() > current_cursor.page_size) {
        more = true;
        resume_after = id(visible[current_cursor.page_size - 1]);
        break;
      }
      if (visible.size() == current_cursor.page_size) {
        // The page is full: a lookahead row proves more data exists after the displayed page.
        more = lookahead;
        resume_after = id(visible.back());
        break;
      }
      if (!lookahead) {
        more = false;
        break;
      }
      after = last_scanned;
    }
    ListResult<Value> result;
    result.page.page_size = current_cursor.page_size;
    result.snapshot.authorization_version = authorization_version;
    result.snapshot.observed_at = now;
    result.snapshot.expires_at = current_cursor.expires_at;
    if (visible.size() > current_cursor.page_size) {
      visible.resize(current_cursor.page_size);
      more = true;
    }
    result.items = std::move(visible);
    if (more) {
      auto token = publish_page(context, query, authorization_version, current_cursor.page_size,
                                resume_after, now);
      if (const auto* failure = std::get_if<Failure>(&token)) {
        return *failure;
      }
      result.page.next_page_token = std::get<std::string>(token);
      result.snapshot.snapshot_id = result.page.next_page_token;
      result.snapshot.expires_at = now + kSnapshotLifetime;
    } else {
      auto allocated = ids_.snapshot_id();
      if (const auto* failure = std::get_if<Failure>(&allocated)) {
        return *failure;
      }
      result.snapshot.snapshot_id = std::get<std::string>(allocated);
    }
    return result;
  }

  // Operation records are Runtime-owned progress rows created in the same UoW as their effect; a
  // first P05 mutation has no asynchronous participant yet, so it starts Accepted with an audit
  // trail rather than claiming completion.
  Outcome<Operation> accepted_operation(RuntimeUnitOfWork& work, const CommandContext& context,
                                        const ResourceRef& resource, TimePoint now) {
    // The request identity is the operation identity for this owner command; a durable retry with
    // the same RPC request identity therefore restores the same accepted operation reference.
    if (!canonical(context.request_id)) {
      return invalid_runtime_input();
    }
    Operation operation;
    operation.ref = {Service::Runtime, OperationId{context.request_id}, resource};
    operation.version = ResourceVersion{1};
    operation.state = OperationState::Accepted;
    (void)now;
    const auto saved = work.operations().save(operation, std::nullopt);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    return operation;
  }

  // Issues a one-time external-bridge bootstrap credential for the resource unless an active
  // credential already exists. Only the SHA-256 digest is persisted; plaintext is returned exactly
  // once through the RPC response and is never written to receipts, audit or logs.
  Outcome<std::optional<std::string>> issue_bootstrap_credential(RuntimeUnitOfWork& work,
                                                                 const std::string& resource_id,
                                                                 TimePoint now) {
    auto active = work.bridge_credentials().active(resource_id);
    if (const auto* failure = std::get_if<Failure>(&active)) {
      return *failure;
    }
    if (std::get<bool>(active)) {
      return std::nullopt;
    }
    auto text = secrets_.bootstrap_credential();
    if (const auto* failure = std::get_if<Failure>(&text)) {
      return *failure;
    }
    auto digest = secrets_.sha256(std::get<std::string>(text));
    if (const auto* failure = std::get_if<Failure>(&digest)) {
      return *failure;
    }
    auto id = ids_.snapshot_id();
    if (const auto* failure = std::get_if<Failure>(&id)) {
      return *failure;
    }
    BridgeCredentialRecord record{std::get<std::string>(id), resource_id,
                                  std::get<Sha256Digest>(digest), now, false};
    const auto saved = work.bridge_credentials().insert(record);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    return std::optional<std::string>{std::get<std::string>(text)};
  }

  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  std::mutex cursors_mutex_;
  std::unordered_map<std::string, PageCursor> cursors_;
  RuntimeAuthorizationPort& authorization_;
  std::vector<std::string> supported_capabilities_;
};

Outcome<std::unique_ptr<RuntimeResourceApplication>>
make_runtime_resource_application(RuntimeResourceDependencies dependencies) {
  try {
    return std::unique_ptr<RuntimeResourceApplication>(
        std::make_unique<RuntimeResourceApplicationImpl>(std::move(dependencies)));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace kiarabot::core::target
