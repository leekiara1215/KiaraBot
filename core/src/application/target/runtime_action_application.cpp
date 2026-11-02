#include "kiarabot/core/application/target/runtime_action_application.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/runtime_rules.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"

namespace kiarabot::core::target {
namespace {

constexpr std::size_t kMaximumSweepBatch = 100;
constexpr std::size_t kMaximumMetadataPage = 100;
constexpr std::string_view kActionResultFeature = "action.result.v1";
constexpr std::array<std::string_view, 11> kPluginPermissions{
    "message.read",  "message.send", "member.read",       "member.mute",
    "member.kick",   "request.read", "request.approve",   "adapter.read",
    "task.schedule", "model.chat",   "storage.namespace",
};
constexpr std::string_view kMethodSubmit = "kiarabot.v1.PluginBridge/SubmitAction";
constexpr std::string_view kMethodQuery = "kiarabot.v1.PluginBridge/QueryAction";
constexpr std::string_view kMethodCancel = "kiarabot.v1.PluginBridge/CancelAction";
constexpr std::string_view kMethodQueryControl = "kiarabot.v1.RuntimeService/QueryActionResult";
constexpr std::string_view kMethodListMetadata = "kiarabot.v1.RuntimeService/ListActionMetadata";
constexpr std::string_view kMethodInspectMetadata =
    "kiarabot.v1.RuntimeService/InspectActionMetadata";
constexpr std::string_view kMethodGetPolicy = "kiarabot.v1.RuntimeService/GetActionPolicy";
constexpr std::string_view kMethodUpdatePolicy = "kiarabot.v1.RuntimeService/UpdateActionPolicy";

Failure invalid_input() {
  return {ErrorCode::InvalidArgument, Acceptance::NotAccepted, Recovery::CorrectInput, {}, {}};
}
Failure permission_denied() {
  return {ErrorCode::PermissionDenied, Acceptance::NotAccepted, Recovery::Authenticate, {}, {}};
}
Failure unauthenticated() {
  return {ErrorCode::Unauthenticated, Acceptance::NotAccepted, Recovery::Authenticate, {}, {}};
}
Failure conflict() {
  return {ErrorCode::Conflict, Acceptance::NotAccepted, Recovery::ReadCurrent, {}, {}};
}
Failure not_found() {
  return {ErrorCode::NotFound, Acceptance::NotAccepted, Recovery::ReadCurrent, {}, {}};
}
Failure unavailable() {
  return {ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted, Recovery::Backoff, {}, {}};
}
Failure internal_failure() {
  return {ErrorCode::Internal, Acceptance::NotAccepted, Recovery::Diagnose, {}, {}};
}
Failure unknown_commit() {
  return {ErrorCode::UpstreamUnavailable, Acceptance::Unknown, Recovery::QuerySameIdentity, {}, {}};
}
bool positive(TimePoint value) {
  return value.time_since_epoch().count() > 0;
}
bool valid_authentication(const AuthenticationContext& authentication, TimePoint now) {
  return canonical_id(authentication.subject.value) &&
         authentication.audience == Service::Runtime &&
         authentication.authorization_version.value > 0 && positive(authentication.verified_at) &&
         positive(authentication.expires_at) &&
         authentication.verified_at < authentication.expires_at && now < authentication.expires_at;
}
bool bounded_text(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(), [](char character) {
           return character == '\0' || character == '\n' || character == '\r';
         });
}
std::string plugin_permission(ActionKind kind) {
  switch (kind) {
  case ActionKind::SendMessage:
  case ActionKind::ReactMessage:
  case ActionKind::DeleteMessage:
    return "message.send";
  case ActionKind::MuteMember:
    return "member.mute";
  case ActionKind::KickMember:
    return "member.kick";
  case ActionKind::ApproveRequest:
    return "request.approve";
  }
  return {};
}
bool declared_permission(std::string_view value) {
  return std::find(kPluginPermissions.begin(), kPluginPermissions.end(), value) !=
         kPluginPermissions.end();
}
bool has_permission(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}
bool has_feature(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}
Outcome<RuntimeAuthorizationDecision>
authorize_control(RuntimeAuthorizationPort* port, const AuthenticationContext& authentication,
                  Permission permission, std::string_view method, const ResourceId& account,
                  TimePoint deadline) {
  if (port == nullptr || !port->available()) {
    return unavailable();
  }
  std::vector<ResourceRef> scope{{ResourceKind::Account, account}};
  auto checked = port->check({authentication, permission, std::string{method}, scope, deadline});
  if (const auto* failure = std::get_if<Failure>(&checked)) {
    return *failure;
  }
  const auto decision = std::get<RuntimeAuthorizationDecision>(checked);
  const auto now = TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  if (!decision.allowed) {
    return decision.denial ? *decision.denial : permission_denied();
  }
  if (decision.subject != authentication.subject || decision.permission != permission ||
      decision.scope != scope || decision.authorization_version.value == 0 ||
      !positive(decision.checked_at) || decision.checked_at > now + std::chrono::seconds{2} ||
      now - decision.checked_at > std::chrono::seconds{60}) {
    return internal_failure();
  }
  return decision;
}
SubjectId actor_for(const AuthenticationContext& authentication) {
  return authentication.subject;
}
Status append_audit(RuntimeUnitOfWork& work, const SubjectId& actor, const ResourceRef& resource,
                    std::string_view request_id, std::string_view trace_id, std::string_view method,
                    ResourceVersion authorization_version, TimePoint now,
                    std::optional<ErrorCode> rejection = std::nullopt) {
  return work.journal().append_audit({actor, resource, std::string{request_id},
                                      std::string{trace_id}, std::string{method}, rejection,
                                      authorization_version, now});
}
Outcome<ActionRecord> find_owner(RuntimeUnitOfWork& work, const ActionOwnerQuery& query) {
  if (query.action_id) {
    if (!bounded_text(query.action_id->value, 1, 128)) {
      return invalid_input();
    }
    auto found = work.actions().find_by_id(*query.action_id);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    const auto& record = std::get<std::optional<ActionRecord>>(found);
    if (!record) {
      return not_found();
    }
    if (record->owner_instance != query.owner_instance) {
      return not_found();
    }
    return *record;
  }
  if (!query.idempotency_key || !bounded_text(*query.idempotency_key, 16, 128)) {
    return invalid_input();
  }
  auto found = work.actions().find_by_owner_key(query.owner_instance, *query.idempotency_key);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  const auto& record = std::get<std::optional<ActionRecord>>(found);
  if (!record) {
    return not_found();
  }
  return *record;
}
Outcome<std::optional<ActionRecord>> read_by_identity(RuntimeUnitOfWorkFactory& transactions,
                                                      const ActionId& action_id,
                                                      const InstanceId& owner,
                                                      const std::string& idempotency_key) {
  auto begun = transactions.begin();
  if (const auto* failure = std::get_if<Failure>(&begun)) {
    return *failure;
  }
  auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
  if (!work) {
    return internal_failure();
  }
  if (!idempotency_key.empty()) {
    auto found = work->actions().find_by_owner_key(owner, idempotency_key);
    if (const auto* failure = std::get_if<Failure>(&found)) {
      return *failure;
    }
    return std::get<std::optional<ActionRecord>>(found);
  }
  auto found = work->actions().find_by_id(action_id);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  return std::get<std::optional<ActionRecord>>(found);
}
Outcome<ActionRecord> resolve_unknown_submit(RuntimeUnitOfWorkFactory& transactions,
                                             const ActionSubmitRequest& request) {
  auto found = read_by_identity(transactions, request.action_id, request.owner_instance,
                                request.idempotency_key);
  if (const auto* failure = std::get_if<Failure>(&found)) {
    return *failure;
  }
  const auto& record = std::get<std::optional<ActionRecord>>(found);
  if (!record || record->owner_instance != request.owner_instance) {
    return unknown_commit();
  }
  return *record;
}
Outcome<ActionSubmitOutcome> replay(ActionRecord record, const ActionRequest& admitted) {
  auto replayed = replay_action(std::move(record), admitted);
  if (const auto* failure = std::get_if<Failure>(&replayed)) {
    return *failure;
  }
  return ActionSubmitOutcome{std::get<ActionRecord>(std::move(replayed)), true};
}
bool valid_policy(const ActionPolicy& policy) {
  if (!canonical_id(policy.account_id.value) || policy.allowed_group_ids.size() > 1024) {
    return false;
  }
  std::vector<std::string> sorted = policy.allowed_group_ids;
  std::sort(sorted.begin(), sorted.end());
  return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end() &&
         std::all_of(policy.allowed_group_ids.begin(), policy.allowed_group_ids.end(),
                     [](const std::string& value) { return bounded_text(value, 1, 128); });
}
Failure receipt_failure(const ActionReportReceipt& receipt) {
  return receipt.failure ? *receipt.failure : conflict();
}

class RuntimeActionApplicationImpl final : public RuntimeActionApplication {
public:
  explicit RuntimeActionApplicationImpl(RuntimeActionDependencies dependencies)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        secrets_(dependencies.secrets), authorization_(dependencies.authorization),
        capacity_(dependencies.capacity), quota_(dependencies.quota) {
    (void)secrets_;
  }

  Outcome<ActionSubmitOutcome> submit_action(const ActionSubmitRequest& request) override {
    try {
      if (!canonical_id(request.owner_instance.value) || !canonical_id(request.account_id.value) ||
          !canonical_id(request.adapter_id.value) ||
          !bounded_text(request.action_id.value, 1, 128) ||
          !bounded_text(request.idempotency_key, 16, 128) || !positive(request.deadline) ||
          !known_action_kind(request.kind) || request.serialized_envelope.empty() ||
          request.serialized_envelope.size() > kMaximumSerializedEnvelope ||
          !bounded_text(request.target_conversation, 1, 128) ||
          request.target_conversation_kind > 2 ||
          !has_feature(request.enabled_features, kActionResultFeature) ||
          !has_permission(request.owner_permissions, plugin_permission(request.kind)) ||
          !declared_permission(plugin_permission(request.kind))) {
        return invalid_input();
      }
      auto request_hash = secrets_.sha256(request.serialized_envelope);
      if (const auto* failure = std::get_if<Failure>(&request_hash)) {
        return *failure;
      }
      ActionRequest admitted_request;
      admitted_request.owner_instance = request.owner_instance;
      admitted_request.account_id = request.account_id;
      admitted_request.adapter_id = request.adapter_id;
      admitted_request.kind = request.kind;
      admitted_request.idempotency_key = request.idempotency_key;
      admitted_request.request_hash = std::get<Sha256Digest>(request_hash);
      admitted_request.serialized_envelope = request.serialized_envelope;
      admitted_request.target_conversation = request.target_conversation;
      admitted_request.target_conversation_kind = request.target_conversation_kind;
      admitted_request.deadline = request.deadline;
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
      auto existing =
          work->actions().find_by_owner_key(request.owner_instance, request.idempotency_key);
      if (const auto* failure = std::get_if<Failure>(&existing)) {
        return *failure;
      }
      if (const auto& record = std::get<std::optional<ActionRecord>>(existing)) {
        return replay(*record, admitted_request);
      }
      auto owner = work->instances().find(request.owner_instance);
      if (const auto* failure = std::get_if<Failure>(&owner)) {
        return *failure;
      }
      const auto& owner_record = std::get<std::optional<PluginInstance>>(owner);
      if (!owner_record || owner_record->state != LifecycleState::Active) {
        return permission_denied();
      }
      auto binding = work->adapter_bindings().find(request.account_id);
      if (const auto* failure = std::get_if<Failure>(&binding)) {
        return *failure;
      }
      const auto& bound = std::get<std::optional<AdapterBindingRecord>>(binding);
      if (!bound || bound->state != LifecycleState::Active) {
        return permission_denied();
      }
      auto registered_adapter = work->adapters().by_instance(bound->adapter_instance);
      if (const auto* failure = std::get_if<Failure>(&registered_adapter)) {
        return *failure;
      }
      const auto& adapter = std::get<std::optional<AdapterRecord>>(registered_adapter);
      if (!adapter || adapter->state != LifecycleState::Active ||
          adapter->adapter != request.adapter_id) {
        return permission_denied();
      }
      admitted_request.binding_version = bound->version;
      auto admitted = admit_action(request.action_id, admitted_request, now);
      if (const auto* failure = std::get_if<Failure>(&admitted)) {
        return *failure;
      }
      const auto& record = std::get<ActionRecord>(admitted);
      if (action_policy_required(request.kind)) {
        auto policy = work->action_policies().get(request.account_id);
        if (const auto* failure = std::get_if<Failure>(&policy)) {
          return *failure;
        }
        const auto checked = check_action_policy(
            request.kind, request.target_conversation, request.target_conversation_kind,
            std::get<std::optional<ActionPolicy>>(policy), request.account_id, now);
        if (const auto* failure = std::get_if<Failure>(&checked)) {
          return *failure;
        }
      }
      auto inserted = work->actions().insert(record, capacity_);
      if (const auto* failure = std::get_if<Failure>(&inserted)) {
        return *failure;
      }
      const auto stored = std::get<ActionRecord>(inserted);
      // quota_windows references actions(action_id); occupancy is charged in the same UoW after
      // the authoritative row exists, and any quota denial rolls back the insert.
      const bool already_expired = stored.state == ActionState::Expired;
      if (!already_expired) {
        const auto scopes =
            action_quota_scopes(request.kind, request.account_id, request.target_conversation,
                                request.target_conversation_kind);
        const auto admitted_quota =
            work->action_quota().admit(stored.action_id, request.kind, scopes, quota_, now);
        if (const auto* failure = std::get_if<Failure>(&admitted_quota)) {
          return *failure;
        }
      }
      const auto audited = append_audit(
          *work, SubjectId{request.owner_instance.value},
          ResourceRef{ResourceKind::Action, ResourceId{stored.action_id.value}}, request.request_id,
          request.trace_id, kMethodSubmit, ResourceVersion{1}, now);
      if (const auto* failure = std::get_if<Failure>(&audited)) {
        return *failure;
      }

      const auto committed = work->commit();
      if (committed == CommitResult::Committed) {
        return ActionSubmitOutcome{stored, false};
      }
      if (committed == CommitResult::Unknown) {
        auto resolved = resolve_unknown_submit(transactions_, request);
        if (const auto* failure = std::get_if<Failure>(&resolved)) {
          return *failure;
        }
        return ActionSubmitOutcome{std::get<ActionRecord>(resolved), true};
      }
      return unknown_commit();
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionRecord> query_owner_action(const ActionOwnerQuery& query) override {
    try {
      if (!canonical_id(query.owner_instance.value)) {
        return invalid_input();
      }
      auto begun = transactions_.begin();
      if (const auto* failure = std::get_if<Failure>(&begun)) {
        return *failure;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      return find_owner(*work, query);
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionRecord> cancel_action(const ActionCancelRequest& request) override {
    try {
      if (!canonical_id(request.owner_instance.value) ||
          !bounded_text(request.action_id.value, 1, 128)) {
        return invalid_input();
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
      auto found = work->actions().find_by_id(request.action_id);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      const auto& current = std::get<std::optional<ActionRecord>>(found);
      if (!current || current->owner_instance != request.owner_instance) {
        return not_found();
      }
      auto record = *current;
      const auto cancelled = ::kiarabot::core::target::cancel_action(record, now);
      if (const auto* failure = std::get_if<Failure>(&cancelled)) {
        return *failure;
      }
      const auto persisted = work->actions().cancel(record.action_id, now);
      if (const auto* failure = std::get_if<Failure>(&persisted)) {
        return *failure;
      }
      auto after = work->actions().find_by_id(record.action_id);
      if (const auto* failure = std::get_if<Failure>(&after)) {
        return *failure;
      }
      const auto& authoritative = std::get<std::optional<ActionRecord>>(after);
      if (!authoritative) {
        return internal_failure();
      }
      const auto audited = append_audit(
          *work, SubjectId{request.owner_instance.value},
          ResourceRef{ResourceKind::Action, ResourceId{record.action_id.value}}, request.request_id,
          request.trace_id, kMethodCancel, ResourceVersion{1}, now);
      if (const auto* failure = std::get_if<Failure>(&audited)) {
        return *failure;
      }
      const auto committed = work->commit();
      if (committed == CommitResult::Committed) {
        return *authoritative;
      }
      if (committed == CommitResult::Unknown) {
        auto resolved =
            read_by_identity(transactions_, request.action_id, request.owner_instance, {});
        if (const auto* failure = std::get_if<Failure>(&resolved)) {
          return *failure;
        }
        const auto& record_after = std::get<std::optional<ActionRecord>>(resolved);
        if (record_after) {
          return *record_after;
        }
        return unknown_commit();
      }
      return conflict();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionRecord> query_control_action(const ActionControlResultQuery& query) override {
    try {
      if (!canonical_id(query.account_id.value) || !positive(query.deadline) ||
          !bounded_text(query.method, 1, 128) ||
          (query.action_id.has_value() == query.idempotency_key.has_value()) ||
          (!query.action_id && !query.owner_instance)) {
        return invalid_input();
      }
      auto decision =
          authorize_control(authorization_, query.authentication, Permission::ActionInspect,
                            query.method.empty() ? kMethodQueryControl : query.method,
                            query.account_id, query.deadline);
      if (const auto* failure = std::get_if<Failure>(&decision)) {
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
      std::optional<ActionRecord> record;
      if (query.action_id) {
        auto found = work->actions().find_by_id(*query.action_id);
        if (const auto* failure = std::get_if<Failure>(&found)) {
          return *failure;
        }
        record = std::get<std::optional<ActionRecord>>(found);
      } else {
        auto found =
            work->actions().find_by_owner_key(*query.owner_instance, *query.idempotency_key);
        if (const auto* failure = std::get_if<Failure>(&found)) {
          return *failure;
        }
        record = std::get<std::optional<ActionRecord>>(found);
      }
      if (!record || record->account_id != query.account_id) {
        return not_found();
      }
      return *record;
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionMetadataPage> list_metadata(const ActionMetadataQuery& query) override {
    try {
      if (!canonical_id(query.account_id.value) || !positive(query.deadline) || query.limit == 0 ||
          query.limit > kMaximumMetadataPage || !bounded_text(query.method, 1, 128)) {
        return invalid_input();
      }
      auto decision =
          authorize_control(authorization_, query.authentication, Permission::ActionInspect,
                            query.method.empty() ? kMethodListMetadata : query.method,
                            query.account_id, query.deadline);
      if (const auto* failure = std::get_if<Failure>(&decision)) {
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
      ActionControlQuery control;
      control.scope =
          ResourceScope{ResourceRef{ResourceKind::Account, query.account_id}, "action.inspect"};
      control.state = query.state;
      control.kind = query.kind;
      control.page = ActionPageRequest{query.limit, query.after_action_id};
      auto listed = work->actions().list_control(control);
      if (const auto* failure = std::get_if<Failure>(&listed)) {
        return *failure;
      }
      ActionMetadataPage page;
      page.records = std::get<std::vector<ActionRecord>>(listed);
      if (page.records.size() == query.limit) {
        page.next_action_id = page.records.back().action_id.value;
      }
      return page;
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionPolicy> get_policy(const ActionPolicyQuery& query) override {
    try {
      if (!canonical_id(query.account_id.value) || !positive(query.deadline)) {
        return invalid_input();
      }
      auto decision =
          authorize_control(authorization_, query.authentication, Permission::AccountPolicyRead,
                            kMethodGetPolicy, query.account_id, query.deadline);
      if (const auto* failure = std::get_if<Failure>(&decision)) {
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
      auto found = work->action_policies().get(query.account_id);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      const auto& policy = std::get<std::optional<ActionPolicy>>(found);
      if (!policy) {
        return not_found();
      }
      return *policy;
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionPolicy> update_policy(const ActionPolicyUpdate& request) override {
    try {
      if (!valid_policy(request.policy) || !positive(request.deadline)) {
        return invalid_input();
      }
      const auto& account = request.policy.account_id;
      auto decision =
          authorize_control(authorization_, request.authentication, Permission::AccountPolicyManage,
                            kMethodUpdatePolicy, account, request.deadline);
      if (const auto* failure = std::get_if<Failure>(&decision)) {
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
      auto existing = work->action_policies().get(account);
      if (const auto* failure = std::get_if<Failure>(&existing)) {
        return *failure;
      }
      const auto& current = std::get<std::optional<ActionPolicy>>(existing);
      auto policy = request.policy;
      policy.updated_at = now;
      if (request.expected_version) {
        if (!current || current->version != *request.expected_version) {
          return conflict();
        }
      } else if (current) {
        return conflict();
      }
      const auto upserted = work->action_policies().upsert(policy, request.expected_version);
      if (const auto* failure = std::get_if<Failure>(&upserted)) {
        return *failure;
      }
      auto after = work->action_policies().get(account);
      if (const auto* failure = std::get_if<Failure>(&after)) {
        return *failure;
      }
      const auto& stored = std::get<std::optional<ActionPolicy>>(after);
      if (!stored) {
        return internal_failure();
      }
      const auto audited = append_audit(*work, actor_for(request.authentication),
                                        ResourceRef{ResourceKind::Account, account},
                                        request.request_id, request.trace_id, kMethodUpdatePolicy,
                                        request.authentication.authorization_version, now);
      if (const auto* failure = std::get_if<Failure>(&audited)) {
        return *failure;
      }
      const auto committed = work->commit();
      if (committed != CommitResult::Committed) {
        return unknown_commit();
      }
      return *stored;
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionReportReceipt> record_result(const ActionResultReport& report) override {
    try {
      if (!canonical_id(report.adapter_instance.value) ||
          !bounded_text(report.action_id.value, 1, 128) || !report.dispatch_attempt_id ||
          !bounded_text(report.dispatch_attempt_id->value, 1, 128)) {
        return invalid_input();
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
      auto found = work->actions().find_by_id(report.action_id);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      const auto& current = std::get<std::optional<ActionRecord>>(found);
      if (!current) {
        return not_found();
      }
      if (current->dispatched_to_instance != report.adapter_instance) {
        return permission_denied();
      }
      auto receipt = work->actions().record_terminal(
          *current, report.result, report.dispatch_attempt_id, report.adapter_instance, now);
      if (const auto* failure = std::get_if<Failure>(&receipt)) {
        return *failure;
      }
      const auto& persisted = std::get<ActionTerminalReceipt>(receipt);
      if (persisted.disposition == ActionReceiptDisposition::Recorded ||
          persisted.disposition == ActionReceiptDisposition::EvidenceOnly) {
        const auto audited = append_audit(
            *work, SubjectId{report.adapter_instance.value},
            ResourceRef{ResourceKind::Action, ResourceId{report.action_id.value}}, report.trace_id,
            report.trace_id, "kiarabot.v1.AdapterBridge/ActionResult", ResourceVersion{1}, now);
        if (const auto* failure = std::get_if<Failure>(&audited)) {
          return *failure;
        }
      }
      const auto committed = work->commit();
      if (committed == CommitResult::Committed) {
        ActionReportReceipt output;
        output.disposition = persisted.disposition;
        output.record = persisted.record;
        output.evidence_appended = persisted.evidence_appended;
        return output;
      }
      if (committed == CommitResult::Unknown) {
        auto resolved = read_by_identity(transactions_, report.action_id, {}, "");
        if (const auto* failure = std::get_if<Failure>(&resolved)) {
          return *failure;
        }
        const auto& record = std::get<std::optional<ActionRecord>>(resolved);
        if (record) {
          ActionReportReceipt output;
          output.disposition = ActionReceiptDisposition::Duplicate;
          output.record = *record;
          return output;
        }
        return unknown_commit();
      }
      return conflict();
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionReportReceipt> record_evidence(const ActionAdapterEvidence& report) override {
    try {
      if (!canonical_id(report.adapter_instance.value) ||
          !bounded_text(report.action_id.value, 1, 128) ||
          !bounded_text(report.dispatch_attempt_id.value, 1, 128)) {
        return invalid_input();
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
      auto found = work->actions().find_by_id(report.action_id);
      if (const auto* failure = std::get_if<Failure>(&found)) {
        return *failure;
      }
      const auto& current = std::get<std::optional<ActionRecord>>(found);
      if (!current) {
        return not_found();
      }
      ActionEvidenceKind kind = ActionEvidenceKind::Unknown;
      if (report.execution_outcome == ExecutionOutcome::NotStarted) {
        kind = ActionEvidenceKind::NotStarted;
      }
      ActionEvidenceRecord evidence;
      evidence.action_id = report.action_id;
      evidence.dispatch_attempt_id = report.dispatch_attempt_id;
      evidence.adapter_instance = report.adapter_instance;
      evidence.kind = kind;
      evidence.execution_outcome = report.execution_outcome;
      evidence.error_code = report.error_code;
      evidence.platform_reference = report.platform_reference;
      evidence.observed_at = report.observed_at;
      evidence.received_at = now;
      evidence.retain_until = action_evidence_retain_until(now);
      auto appended = work->action_evidence().append(evidence);
      if (const auto* failure = std::get_if<Failure>(&appended)) {
        return *failure;
      }
      const auto committed = work->commit();
      if (committed != CommitResult::Committed) {
        return unknown_commit();
      }
      ActionReportReceipt output;
      output.disposition = ActionReceiptDisposition::EvidenceOnly;
      output.record = *current;
      output.evidence_appended = true;
      return output;
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionDispatchClaim> claim_due(const InstanceId& adapter_instance, std::size_t limit,
                                         TimePoint now) override {
    try {
      if (!canonical_id(adapter_instance.value) || limit == 0 || limit > kMaximumSweepBatch ||
          !positive(now)) {
        return invalid_input();
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
      const auto database_now = std::get<TimePoint>(sampled);
      auto adapter = work->adapters().by_instance(adapter_instance);
      if (const auto* failure = std::get_if<Failure>(&adapter)) {
        return *failure;
      }
      const auto& registered = std::get<std::optional<AdapterRecord>>(adapter);
      if (!registered || registered->state != LifecycleState::Active ||
          registered->current.instance != adapter_instance ||
          (!positive(registered->online_until) || database_now >= registered->online_until)) {
        return ActionDispatchClaim{};
      }
      auto due = work->actions().list_due(registered->adapter, limit, database_now);
      if (const auto* failure = std::get_if<Failure>(&due)) {
        return *failure;
      }
      ActionDispatchClaim claim;
      for (const auto& record : std::get<std::vector<ActionRecord>>(due)) {
        auto generated = ids_.operation_id();
        if (const auto* failure = std::get_if<Failure>(&generated)) {
          return *failure;
        }
        const AttemptId attempt{std::get<OperationId>(generated).value};
        auto updated = record;
        const auto marked =
            mark_action_dispatched(updated, attempt, adapter_instance, database_now);
        if (const auto* failure = std::get_if<Failure>(&marked)) {
          return *failure;
        }
        const auto persisted = work->actions().mark_dispatched(updated.action_id, attempt,
                                                               adapter_instance, database_now);
        if (const auto* failure = std::get_if<Failure>(&persisted)) {
          return *failure;
        }
        ActionDispatchFrame frame;
        frame.action_id = updated.action_id;
        frame.dispatch_attempt_id = attempt;
        frame.dispatch_sequence = updated.dispatch_sequence;
        frame.serialized_envelope = updated.serialized_envelope;
        frame.deadline = updated.deadline;
        claim.frames.push_back(std::move(frame));
      }
      const auto committed = work->commit();
      if (committed == CommitResult::Committed) {
        return claim;
      }
      // An Unknown commit may already have marked the rows dispatched. Their frames are lost, so
      // the actions must resolve as UNKNOWN/EXPIRED rather than being dispatched a second time.
      return ActionDispatchClaim{};
    } catch (...) {
      return internal_failure();
    }
  }

  Outcome<ActionSweepResult> maintenance() override {
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
      ActionSweepResult result;
      auto expired = work->actions().expire_due(kMaximumSweepBatch, now);
      if (const auto* failure = std::get_if<Failure>(&expired)) {
        return *failure;
      }
      result.expired = std::get<std::vector<ActionRecord>>(expired).size();
      auto windows = work->action_quota().prune_windows(now, kMaximumSweepBatch);
      if (const auto* failure = std::get_if<Failure>(&windows)) {
        return *failure;
      }
      result.quota_pruned = std::get<std::size_t>(windows);
      auto evidence = work->action_evidence().prune(now, kMaximumSweepBatch);
      if (const auto* failure = std::get_if<Failure>(&evidence)) {
        return *failure;
      }
      result.evidence_pruned = std::get<std::size_t>(evidence);
      auto records = work->actions().prune_terminal(now, kMaximumSweepBatch);
      if (const auto* failure = std::get_if<Failure>(&records)) {
        return *failure;
      }
      result.records_pruned = std::get<std::size_t>(records);
      const auto committed = work->commit();
      if (committed != CommitResult::Committed) {
        return unavailable();
      }
      return result;
    } catch (...) {
      return internal_failure();
    }
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  RuntimeAuthorizationPort* authorization_;
  ActionCapacity capacity_;
  ActionQuotaLimits quota_;
};

} // namespace

Outcome<std::unique_ptr<RuntimeActionApplication>>
make_runtime_action_application(RuntimeActionDependencies dependencies) {
  try {
    return std::unique_ptr<RuntimeActionApplication>(
        std::make_unique<RuntimeActionApplicationImpl>(std::move(dependencies)));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace kiarabot::core::target
