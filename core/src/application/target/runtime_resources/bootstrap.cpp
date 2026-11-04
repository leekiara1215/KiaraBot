#include "kiarabot/core/application/target/runtime_resources/bootstrap.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common.hpp"
#include "kiarabot/core/domain/target/bridge_rules.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/runtime_rules.hpp"
#include "mutation.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
namespace {

bool canonical_resource_kind_for_family(ResourceKind kind, BootstrapPeerFamily family) {
  return kind ==
         (family == BootstrapPeerFamily::Adapter ? ResourceKind::Account : ResourceKind::Instance);
}

Failure invalid_state() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}

// The persisted intent must always name this Runtime as the operation owner; a stored row that
// claims another owner is internal corruption, not an authorizable delivery.
bool valid_stored_delivery(const BootstrapDeliveryRecord& delivery, const ResourceRef& resource) {
  return !delivery.delivery_id.empty() && delivery.resource == resource &&
         delivery.operation.owner == Service::Runtime && delivery.operation.resource == resource &&
         delivery.credential_version > 0 && !delivery.recipient_id.empty();
}

} // namespace

bool canonical_bootstrap_identifier(std::string_view value) {
  if (value.empty() || value.size() > kMaximumBootstrapIdentifierLength || value == "." ||
      value == "..") {
    return false;
  }
  for (const unsigned char character : value) {
    const bool accepted = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' ||
                          character == '_' || character == '.';
    if (!accepted) {
      return false;
    }
  }
  return true;
}

bool valid_bootstrap_recipient_id(std::string_view value) {
  return canonical_bootstrap_identifier(value);
}

bool valid_bootstrap_delivery_id(std::string_view value) {
  return canonical_bootstrap_identifier(value);
}

bool valid_bootstrap_receipt_id(std::string_view value) {
  return canonical_bootstrap_identifier(value);
}

Permission bootstrap_manage_permission(BootstrapPeerFamily family) {
  return family == BootstrapPeerFamily::Adapter ? Permission::AccountManage
                                                : Permission::PluginManage;
}

std::uint64_t normalized_bootstrap_credential_version(const BridgeCredentialRecord& record) {
  return record.version == 0 ? kBridgeCredentialVersion : record.version;
}

Outcome<std::uint64_t>
next_bootstrap_credential_version(const std::optional<BridgeCredentialRecord>& latest) {
  if (!latest) {
    return std::uint64_t{1};
  }
  const auto current = normalized_bootstrap_credential_version(*latest);
  if (current >= kMaximumBootstrapCredentialVersion) {
    // The persisted identity already occupies the last representable version.
    return failure(ErrorCode::RateLimited, Recovery::Diagnose);
  }
  return current + 1;
}

Status validate_expected_credential_version(std::optional<std::uint64_t> expected,
                                            const std::optional<BridgeCredentialRecord>& latest) {
  if (!expected) {
    return std::monostate{};
  }
  if (!latest || *expected != normalized_bootstrap_credential_version(*latest)) {
    return stale_version();
  }
  return std::monostate{};
}

Outcome<BootstrapDeliveryRecord>
apply_bootstrap_delivery_report(const BootstrapDeliveryRecord& current,
                                const ReportBootstrapDeliveryCommand& command,
                                std::uint64_t current_credential_version, TimePoint now) {
  if (command.delivery_id != current.delivery_id || command.recipient_id != current.recipient_id) {
    return not_found();
  }
  // A superseded version can never make a later intent current; the persisted credential version
  // is authoritative, and the deployment file writes must CAS against it.
  if (command.credential_version != current.credential_version ||
      current.credential_version != current_credential_version) {
    return stale_version();
  }
  if (!positive_time(command.observed_at) || command.observed_at > now + std::chrono::seconds{60}) {
    return invalid_runtime_input();
  }
  BootstrapDeliveryRecord next = current;
  next.observed_at = command.observed_at;
  switch (command.state) {
  case BootstrapDeliveryState::Delivered:
    if (!valid_bootstrap_receipt_id(command.receipt_id)) {
      return invalid_runtime_input();
    }
    switch (current.state) {
    case BootstrapDeliveryState::Delivered:
      if (command.receipt_id != current.receipt_id) {
        return invalid_state();
      }
      return next;
    case BootstrapDeliveryState::Unknown:
    case BootstrapDeliveryState::Pending:
      next.state = BootstrapDeliveryState::Delivered;
      next.receipt_id = command.receipt_id;
      return next;
    case BootstrapDeliveryState::Revoked:
      return invalid_state();
    }
    return internal_failure();
  case BootstrapDeliveryState::Unknown:
    switch (current.state) {
    case BootstrapDeliveryState::Delivered:
      // A later ambiguous observation never downgrades a confirmed installation.
      return invalid_state();
    case BootstrapDeliveryState::Revoked:
      return invalid_state();
    case BootstrapDeliveryState::Unknown:
    case BootstrapDeliveryState::Pending:
      next.state = BootstrapDeliveryState::Unknown;
      return next;
    }
    return internal_failure();
  case BootstrapDeliveryState::Pending:
  case BootstrapDeliveryState::Revoked:
  default:
    // Pending is a Runtime-owned intent and REVOKED is a Runtime/lifecycle fact; a delivery report
    // can never assert either.
    return invalid_runtime_input();
  }
}

Outcome<std::optional<BootstrapDeliveryRecord>>
current_delivery_metadata(RuntimeUnitOfWorkFactory& transactions, const CommandContext& context,
                          const ResourceRef& resource, const std::string& recipient_id) {
  if (!valid_bootstrap_recipient_id(recipient_id)) {
    return invalid_runtime_input();
  }
  return with_work<std::optional<BootstrapDeliveryRecord>>(
      transactions, context,
      [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<std::optional<BootstrapDeliveryRecord>> {
        auto found = work.bootstrap_deliveries().current(resource);
        if (const auto* rejected = std::get_if<Failure>(&found)) {
          return *rejected;
        }
        const auto& delivery = std::get<std::optional<BootstrapDeliveryRecord>>(found);
        if (!delivery || !valid_stored_delivery(*delivery, resource) ||
            delivery->recipient_id != recipient_id) {
          return std::optional<BootstrapDeliveryRecord>{};
        }
        return std::optional<BootstrapDeliveryRecord>{*delivery};
      });
}

Outcome<std::optional<BootstrapDeliveryIssue>>
issue_bootstrap_delivery(RuntimeUnitOfWork& work, RuntimeIdPort& ids, RuntimeSecretPort& secrets,
                         const ResourceRef& resource, BootstrapPeerFamily family,
                         const std::string& credential_resource_id, const std::string& recipient_id,
                         const OperationRef& operation, TimePoint now) {
  if (!canonical_resource_kind_for_family(resource.kind, family) || !canonical(resource.id.value) ||
      !canonical(credential_resource_id) || !valid_bootstrap_recipient_id(recipient_id) ||
      operation.owner != Service::Runtime || operation.resource != resource ||
      !positive_time(now)) {
    return invalid_runtime_input();
  }
  auto latest = work.bridge_credentials().latest(credential_resource_id);
  if (const auto* rejected = std::get_if<Failure>(&latest)) {
    return *rejected;
  }
  const auto& latest_record = std::get<std::optional<BridgeCredentialRecord>>(latest);
  if (latest_record && !latest_record->revoked) {
    // Never mint a second plaintext for an already-active credential: the first one is
    // unrecoverable by design, so only an explicit authorized reissue may promote a new version.
    auto found = work.bootstrap_deliveries().current(resource);
    if (const auto* rejected = std::get_if<Failure>(&found)) {
      return *rejected;
    }
    const auto& intent = std::get<std::optional<BootstrapDeliveryRecord>>(found);
    const auto active_version = normalized_bootstrap_credential_version(*latest_record);
    if (intent && intent->credential_version == active_version &&
        intent->recipient_id == recipient_id && intent->state != BootstrapDeliveryState::Revoked) {
      return std::optional<BootstrapDeliveryIssue>{BootstrapDeliveryIssue{*intent, std::string{}}};
    }
    return std::optional<BootstrapDeliveryIssue>{};
  }
  auto version = next_bootstrap_credential_version(latest_record);
  if (const auto* rejected = std::get_if<Failure>(&version)) {
    return *rejected;
  }
  auto text = secrets.bootstrap_credential();
  if (const auto* rejected = std::get_if<Failure>(&text)) {
    return *rejected;
  }
  auto digest = secrets.sha256(std::get<std::string>(text));
  if (const auto* rejected = std::get_if<Failure>(&digest)) {
    return *rejected;
  }
  auto credential_id = ids.snapshot_id();
  if (const auto* rejected = std::get_if<Failure>(&credential_id)) {
    return *rejected;
  }
  auto delivery_id = ids.snapshot_id();
  if (const auto* rejected = std::get_if<Failure>(&delivery_id)) {
    return *rejected;
  }
  const auto credential_version = std::get<std::uint64_t>(version);
  const auto inserted = work.bridge_credentials().insert(
      BridgeCredentialRecord{std::get<std::string>(credential_id), credential_resource_id,
                             std::get<Sha256Digest>(digest), now, false, credential_version});
  if (const auto* rejected = std::get_if<Failure>(&inserted)) {
    return *rejected;
  }
  BootstrapDeliveryRecord delivery;
  delivery.delivery_id = std::get<std::string>(delivery_id);
  delivery.resource = resource;
  delivery.recipient_id = recipient_id;
  delivery.credential_version = credential_version;
  delivery.state = BootstrapDeliveryState::Pending;
  delivery.operation = operation;
  delivery.created_at = now;
  delivery.observed_at = now;
  const auto saved = work.bootstrap_deliveries().insert(delivery);
  if (const auto* rejected = std::get_if<Failure>(&saved)) {
    return *rejected;
  }
  return std::optional<BootstrapDeliveryIssue>{
      BootstrapDeliveryIssue{std::move(delivery), std::get<std::string>(text)}};
}

namespace {

// Resolves the exact credential identity for a delivery family from the authoritative current
// business row. A deleted/disabled/unbound resource makes every pending delivery and late report
// fail closed instead of re-authorizing a stale secret.
Outcome<std::string> credential_resource_id(RuntimeUnitOfWork& work, const ResourceRef& resource,
                                            BootstrapPeerFamily family) {
  if (family == BootstrapPeerFamily::Adapter) {
    auto binding = work.adapter_bindings().find(ResourceId{resource.id.value});
    if (const auto* rejected = std::get_if<Failure>(&binding)) {
      return *rejected;
    }
    const auto& value = std::get<std::optional<AdapterBindingRecord>>(binding);
    if (!value || value->state != LifecycleState::Active || value->adapter_instance.value.empty() ||
        !canonical(value->adapter_instance.value)) {
      return not_found();
    }
    return value->adapter_instance.value;
  }
  auto instance = work.instances().find(InstanceId{resource.id.value});
  if (const auto* rejected = std::get_if<Failure>(&instance)) {
    return *rejected;
  }
  const auto& value = std::get<std::optional<PluginInstance>>(instance);
  if (!value || value->state != LifecycleState::Active || !canonical(value->id.value)) {
    return not_found();
  }
  return value->id.value;
}

Status valid_delivery_target(const ResourceRef& resource) {
  if (!canonical(resource.id.value)) {
    return invalid_runtime_input();
  }
  if (resource.kind != ResourceKind::Account && resource.kind != ResourceKind::Instance) {
    return invalid_runtime_input();
  }
  return std::monostate{};
}

// Authorized read of a durable no-secret intent in a fresh UoW. Replay paths restore metadata
// only; plaintext is never reconstructed from the persisted digest.
Outcome<BootstrapDeliveryRecord>
read_delivery(RuntimeUnitOfWorkFactory& transactions, const CommandContext& context,
              const ResourceRef& resource, const std::string& delivery_id,
              const std::optional<OperationRef>& expected_operation) {
  return with_work<BootstrapDeliveryRecord>(
      transactions, context,
      [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<BootstrapDeliveryRecord> {
        auto found = delivery_id.empty() ? work.bootstrap_deliveries().current(resource)
                                         : work.bootstrap_deliveries().find(resource, delivery_id);
        if (const auto* rejected = std::get_if<Failure>(&found)) {
          return *rejected;
        }
        const auto& delivery = std::get<std::optional<BootstrapDeliveryRecord>>(found);
        if (!delivery || !valid_stored_delivery(*delivery, resource)) {
          return internal_failure();
        }
        if (expected_operation && delivery->operation.id != expected_operation->id) {
          return internal_failure();
        }
        return *delivery;
      });
}

Outcome<Operation> read_operation(RuntimeUnitOfWorkFactory& transactions,
                                  const CommandContext& context, const OperationRef& reference) {
  return with_work<Operation>(transactions, context,
                              [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<Operation> {
                                auto found = work.operations().find(reference.id);
                                if (const auto* rejected = std::get_if<Failure>(&found)) {
                                  return *rejected;
                                }
                                const auto& operation = std::get<std::optional<Operation>>(found);
                                if (!operation || operation->ref != reference) {
                                  return internal_failure();
                                }
                                return *operation;
                              });
}

class RuntimeBootstrapDeliveryApplicationImpl final : public RuntimeBootstrapDeliveryApplication {
public:
  explicit RuntimeBootstrapDeliveryApplicationImpl(BootstrapDeliveryDependencies dependencies)
      : transactions_(dependencies.transactions), ids_(dependencies.ids),
        secrets_(dependencies.secrets), authorization_(dependencies.authorization) {}

  Outcome<BootstrapDeliveryRecord> get(const BootstrapDeliveryQuery& request) override {
    if (const auto failure = status_failure(validate_command_context(request.context, false))) {
      return *failure;
    }
    if (const auto failure = status_failure(valid_delivery_target(request.resource))) {
      return *failure;
    }
    if (!request.delivery_id.empty() && !valid_bootstrap_delivery_id(request.delivery_id)) {
      return invalid_runtime_input();
    }
    auto checked =
        authorize(authorization_, request.context.authentication, Permission::ResourceRead,
                  request.context.method, {request.resource}, request.context.deadline);
    if (const auto* rejected = std::get_if<Failure>(&checked)) {
      return *rejected;
    }
    return with_work<BootstrapDeliveryRecord>(
        transactions_, request.context,
        [&](RuntimeUnitOfWork& work, TimePoint) -> Outcome<BootstrapDeliveryRecord> {
          auto found =
              request.delivery_id.empty()
                  ? work.bootstrap_deliveries().current(request.resource)
                  : work.bootstrap_deliveries().find(request.resource, request.delivery_id);
          if (const auto* rejected = std::get_if<Failure>(&found)) {
            return *rejected;
          }
          const auto& delivery = std::get<std::optional<BootstrapDeliveryRecord>>(found);
          if (!delivery || !valid_stored_delivery(*delivery, request.resource)) {
            return not_found();
          }
          return *delivery;
        });
  }

  Outcome<BootstrapReissueResult> reissue(const ReissueBootstrapCommand& request,
                                          BootstrapPeerFamily family) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (const auto failure = status_failure(valid_delivery_target(request.resource))) {
      return *failure;
    }
    if (!canonical_resource_kind_for_family(request.resource.kind, family) ||
        !valid_bootstrap_recipient_id(request.recipient_id) ||
        (request.expected_credential_version &&
         *request.expected_credential_version == std::numeric_limits<std::uint64_t>::max())) {
      return invalid_runtime_input();
    }
    auto checked = authorize(authorization_, request.context.authentication,
                             bootstrap_manage_permission(family), request.context.method,
                             {request.resource}, request.context.deadline);
    if (const auto* rejected = std::get_if<Failure>(&checked)) {
      return *rejected;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    std::optional<BootstrapDeliveryRecord> issued_delivery;
    std::optional<Operation> issued_operation;
    std::string plaintext;
    // The receipt can only replay domain records that already live in the public ReplayResult
    // variant; the Runtime operation is the durable identity. Delivery metadata is read back in a
    // fresh authorized UoW and never carries plaintext.
    auto result = mutate<Operation>(
        transactions_, request.context, request.resource, authorization_version,
        [&](RuntimeUnitOfWork& work, TimePoint now) -> Outcome<Operation> {
          auto credential_id = credential_resource_id(work, request.resource, family);
          if (const auto* rejected = std::get_if<Failure>(&credential_id)) {
            return *rejected;
          }
          auto latest = work.bridge_credentials().latest(std::get<std::string>(credential_id));
          if (const auto* rejected = std::get_if<Failure>(&latest)) {
            return *rejected;
          }
          const auto& latest_record = std::get<std::optional<BridgeCredentialRecord>>(latest);
          if (const auto failure = status_failure(validate_expected_credential_version(
                  request.expected_credential_version, latest_record))) {
            return *failure;
          }
          // Explicit reissue atomically revokes the current digest/version and its intent before a
          // successor is issued; old bootstrap proof/refresh lose the current-version gate.
          if (latest_record && !latest_record->revoked) {
            // CAS on the exact persisted version: of two concurrent reissues only the one that
            // still observes the current version may revoke it and advance.
            auto revoked = work.bridge_credentials().revoke_if_current(
                std::get<std::string>(credential_id),
                normalized_bootstrap_credential_version(*latest_record), now);
            if (const auto* rejected = std::get_if<Failure>(&revoked)) {
              return *rejected;
            }
          }
          auto intent = work.bootstrap_deliveries().current(request.resource);
          if (const auto* rejected = std::get_if<Failure>(&intent)) {
            return *rejected;
          }
          auto current_intent = std::move(std::get<std::optional<BootstrapDeliveryRecord>>(intent));
          if (current_intent && current_intent->state != BootstrapDeliveryState::Revoked) {
            BootstrapDeliveryRecord previous = *current_intent;
            previous.state = BootstrapDeliveryState::Revoked;
            previous.observed_at = now;
            const auto transitioned =
                work.bootstrap_deliveries().transition(previous, current_intent->state);
            if (const auto* rejected = std::get_if<Failure>(&transitioned)) {
              return *rejected;
            }
          }
          auto operation = accepted_operation(work, request.context, request.resource, now);
          if (const auto* rejected = std::get_if<Failure>(&operation)) {
            return *rejected;
          }
          auto issued = issue_bootstrap_delivery(
              work, ids_, secrets_, request.resource, family, std::get<std::string>(credential_id),
              request.recipient_id, std::get<Operation>(operation).ref, now);
          if (const auto* rejected = std::get_if<Failure>(&issued)) {
            return *rejected;
          }
          auto issue = std::move(std::get<std::optional<BootstrapDeliveryIssue>>(issued));
          if (!issue) {
            return internal_failure();
          }
          issued_delivery = std::move(issue->delivery);
          plaintext = std::move(issue->plaintext);
          issued_operation = std::get<Operation>(operation);
          return std::get<Operation>(operation);
        },
        [](const Operation& value) { return value.ref.resource; },
        [](const Operation& value) { return value.version; });
    if (const auto* rejected = std::get_if<Failure>(&result)) {
      return *rejected;
    }
    auto mutation = std::move(std::get<Mutation<Operation>>(result));
    if (issued_delivery && issued_operation) {
      return BootstrapReissueResult{std::move(*issued_delivery), std::move(plaintext),
                                    std::move(*issued_operation)};
    }
    if (!mutation.operation) {
      return internal_failure();
    }
    auto delivery = read_delivery(transactions_, request.context, request.resource, std::string{},
                                  mutation.operation);
    if (const auto* rejected = std::get_if<Failure>(&delivery)) {
      return *rejected;
    }
    auto stored_operation = read_operation(transactions_, request.context, *mutation.operation);
    if (const auto* rejected = std::get_if<Failure>(&stored_operation)) {
      return *rejected;
    }
    return BootstrapReissueResult{std::move(std::get<BootstrapDeliveryRecord>(delivery)),
                                  std::string{}, std::move(std::get<Operation>(stored_operation))};
  }

  Outcome<BootstrapDeliveryRecord> report(const ReportBootstrapDeliveryCommand& request,
                                          BootstrapPeerFamily family) override {
    if (const auto failure = status_failure(validate_command_context(request.context, true))) {
      return *failure;
    }
    if (!request.control_peer_authorized) {
      // Only the registered Control platform peer may report deployment evidence; a delegated
      // user or plugin can never mark its own delivery DELIVERED.
      return permission_denied();
    }
    if (const auto failure = status_failure(valid_delivery_target(request.resource))) {
      return *failure;
    }
    if (!canonical_resource_kind_for_family(request.resource.kind, family) ||
        !valid_bootstrap_delivery_id(request.delivery_id) ||
        !valid_bootstrap_recipient_id(request.recipient_id) || request.credential_version == 0 ||
        (request.state != BootstrapDeliveryState::Delivered &&
         request.state != BootstrapDeliveryState::Unknown)) {
      return invalid_runtime_input();
    }
    auto checked = authorize(authorization_, request.context.authentication,
                             bootstrap_manage_permission(family), request.context.method,
                             {request.resource}, request.context.deadline);
    if (const auto* rejected = std::get_if<Failure>(&checked)) {
      return *rejected;
    }
    const auto authorization_version =
        std::get<RuntimeAuthorizationDecision>(checked).authorization_version;
    try {
      auto begun = transactions_.begin();
      if (const auto* rejected = std::get_if<Failure>(&begun)) {
        return *rejected;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return internal_failure();
      }
      auto sampled = work->current_time();
      if (const auto* rejected = std::get_if<Failure>(&sampled)) {
        return *rejected;
      }
      const auto now = std::get<TimePoint>(sampled);
      if (now >= request.context.deadline) {
        return failure(ErrorCode::Timeout, Recovery::QuerySameIdentity);
      }
      auto found = work->receipts().find(receipt_key(request.context, request.resource));
      if (const auto* rejected = std::get_if<Failure>(&found)) {
        return *rejected;
      }
      if (const auto& original = std::get<std::optional<CommandReceipt>>(found)) {
        if (original->fingerprint != request.context.fingerprint) {
          return stale_version();
        }
        if (!std::holds_alternative<ResourceMutationResult>(original->result)) {
          return internal_failure();
        }
        work->rollback();
        // Report replay restores the already durable state; it never reports a second time and
        // never fabricates a delivered receipt.
        return read_delivery(transactions_, request.context, request.resource, request.delivery_id,
                             std::nullopt);
      }
      auto credential_id = credential_resource_id(*work, request.resource, family);
      if (const auto* rejected = std::get_if<Failure>(&credential_id)) {
        return *rejected;
      }
      auto latest = work->bridge_credentials().latest(std::get<std::string>(credential_id));
      if (const auto* rejected = std::get_if<Failure>(&latest)) {
        return *rejected;
      }
      const auto& latest_record = std::get<std::optional<BridgeCredentialRecord>>(latest);
      if (!latest_record) {
        return not_found();
      }
      auto intent = work->bootstrap_deliveries().find(request.resource, request.delivery_id);
      if (const auto* rejected = std::get_if<Failure>(&intent)) {
        return *rejected;
      }
      const auto& current = std::get<std::optional<BootstrapDeliveryRecord>>(intent);
      if (!current || !valid_stored_delivery(*current, request.resource)) {
        return not_found();
      }
      auto next = apply_bootstrap_delivery_report(
          *current, request, normalized_bootstrap_credential_version(*latest_record), now);
      if (const auto* rejected = std::get_if<Failure>(&next)) {
        return *rejected;
      }
      auto updated = std::move(std::get<BootstrapDeliveryRecord>(next));
      const auto transitioned = work->bootstrap_deliveries().transition(updated, current->state);
      if (const auto* rejected = std::get_if<Failure>(&transitioned)) {
        return *rejected;
      }
      auto audited = append_audit(*work, request.context, request.resource, authorization_version,
                                  std::nullopt, now);
      if (const auto* rejected = std::get_if<Failure>(&audited)) {
        return *rejected;
      }
      auto receipt =
          record_receipt(*work, request.context, request.resource,
                         ReplayResult(ResourceMutationResult{
                             request.resource, ResourceVersion{request.credential_version}}),
                         now);
      if (const auto* rejected = std::get_if<Failure>(&receipt)) {
        return *rejected;
      }
      const auto committed = work->commit();
      if (committed != CommitResult::Committed) {
        return map_commit(committed);
      }
      return updated;
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal_failure();
    }
  }

private:
  RuntimeUnitOfWorkFactory& transactions_;
  RuntimeIdPort& ids_;
  RuntimeSecretPort& secrets_;
  RuntimeAuthorizationPort& authorization_;
};

} // namespace

Outcome<std::unique_ptr<RuntimeBootstrapDeliveryApplication>>
make_runtime_bootstrap_delivery_application(BootstrapDeliveryDependencies dependencies) {
  return std::unique_ptr<RuntimeBootstrapDeliveryApplication>{
      std::make_unique<RuntimeBootstrapDeliveryApplicationImpl>(dependencies)};
}

} // namespace kiarabot::core::target::runtime_resources_detail
