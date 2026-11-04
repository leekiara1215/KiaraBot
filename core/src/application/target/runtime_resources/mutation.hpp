#pragma once

#include "common.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
template <typename Value> struct Mutation {
  Value value;
  std::optional<OperationRef> operation;
  std::optional<ResourceState> committed_state{};
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
       const std::function<ResourceVersion(const Value&)>& result_version,
       bool management_receipt = false) {
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
      if (!operation || operation->ref.owner != Service::Runtime ||
          operation->ref.resource != resource) {
        return std::nullopt;
      }
      return operation->ref;
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
        if (management_receipt) {
          auto saved = work->management_receipt(receipt_key(context, resource));
          if (const auto* failure = std::get_if<Failure>(&saved)) {
            return *failure;
          }
          const auto& receipt = std::get<std::optional<RuntimeManagementReceipt>>(saved);
          if (!receipt || receipt->format_version != 1 ||
              receipt->operation.owner != Service::Runtime ||
              receipt->operation.resource != resource ||
              (resource.kind == ResourceKind::Instance) != receipt->committed_state.has_value() ||
              (receipt->committed_state &&
               (receipt->committed_state->resource != resource ||
                receipt->committed_state->source_service != Service::Runtime ||
                receipt->committed_state->adapter_fence))) {
            return internal_failure();
          }
          return Mutation<Value>{*value, receipt->operation, receipt->committed_state};
        }
        return Mutation<Value>{*value, restore_operation(), std::nullopt};
      }
      return internal_failure();
    }
    auto applied = effect(*work, now);
    if (const auto* failure = std::get_if<Failure>(&applied)) {
      // A pure dependency (crypto/id generation) may fail after a repository has written.
      // Roll back the entire business transaction before recording the rejection separately.
      // Neither rejection-audit failure nor its unknown commit turns a rejected command into
      // an uncertain business effect: its business transaction has already been rolled back.
      work->rollback();
      auto audit_begun = transactions.begin();
      if (auto* audit_work = std::get_if<std::unique_ptr<RuntimeUnitOfWork>>(&audit_begun);
          audit_work && *audit_work) {
        if (const auto audited = append_audit(**audit_work, context, resource,
                                              authorization_version, failure->code, now);
            !std::holds_alternative<Failure>(audited)) {
          (void)(*audit_work)->commit();
        }
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
    std::optional<ResourceState> committed_state{};
    if (management_receipt) {
      if (!operation) {
        return internal_failure();
      }
      if (resource.kind == ResourceKind::Instance) {
        auto found = work->instances().find(InstanceId{resource.id.value});
        if (const auto* failure = std::get_if<Failure>(&found)) {
          return *failure;
        }
        const auto& instance = std::get<std::optional<PluginInstance>>(found);
        if (!instance) {
          return internal_failure();
        }
        committed_state = ResourceState{resource,
                                        instance->version,
                                        instance->state,
                                        instance->desired,
                                        instance->current,
                                        std::nullopt,
                                        instance->current.has_value(),
                                        instance->desired_version,
                                        instance->applied_config_version,
                                        ExecutionOutcome::NotStarted,
                                        now,
                                        Service::Runtime};
      }
      const auto saved = work->save_management_receipt(
          receipt_key(context, resource), RuntimeManagementReceipt{1, *operation, committed_state});
      if (const auto* failure = std::get_if<Failure>(&saved)) {
        return *failure;
      }
    }
    const auto committed = work->commit();
    if (committed != CommitResult::Committed) {
      return map_commit(committed);
    }
    return Mutation<Value>{std::move(value), std::move(operation), std::move(committed_state)};
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal_failure();
  }
}

} // namespace kiarabot::core::target::runtime_resources_detail
