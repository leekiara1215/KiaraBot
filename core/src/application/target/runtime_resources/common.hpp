#pragma once

#include <chrono>
#include <functional>
#include <new>
#include <optional>
#include <utility>

#include "kiarabot/core/application/target/runtime_resources/types.hpp"

namespace kiarabot::core::target::runtime_resources_detail {

Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose,
                Acceptance acceptance = Acceptance::NotAccepted);
Failure internal_failure();
Failure unknown_result();
Failure unavailable();
Failure permission_denied();
Failure not_found();
Failure stale_version();
std::optional<Failure> status_failure(Status status);
bool positive_time(TimePoint value);
TimePoint now();
bool canonical(const std::string& value);
Status validate_command_context(const CommandContext& context, bool mutating);
Status validate_authentication(const AuthenticationContext& authentication, TimePoint now);
Outcome<RuntimeAuthorizationDecision> authorize(RuntimeAuthorizationPort& port,
                                                const AuthenticationContext& authentication,
                                                Permission permission, std::string method,
                                                const std::vector<ResourceRef>& scope,
                                                TimePoint deadline);
Outcome<RuntimeAuthorizationDecision>
authorize_catalog(RuntimeAuthorizationPort& authorization,
                  const std::map<ResourceKind, ResourceId>& catalogs, const CommandContext& context,
                  ResourceKind family, const ResourceRef& catalog, Permission permission);
Failure map_commit(CommitResult result);
ReceiptKey receipt_key(const CommandContext& context, const ResourceRef& resource);
Status append_peer_audit(RuntimeUnitOfWork& work, const SubjectId& actor,
                         const std::string& request_id, const std::string& trace_id,
                         std::string method, const ResourceRef& resource,
                         const ResourceVersion& authorization_version,
                         std::optional<ErrorCode> rejection, TimePoint now);
Status append_audit(RuntimeUnitOfWork& work, const CommandContext& context,
                    const ResourceRef& resource, const ResourceVersion& authorization_version,
                    std::optional<ErrorCode> rejection, TimePoint now);
Status record_receipt(RuntimeUnitOfWork& work, const CommandContext& context,
                      const ResourceRef& resource, ReplayResult result, TimePoint now);
std::optional<OperationAccepted> accepted(std::optional<OperationRef> operation);
Outcome<Operation> accepted_operation(RuntimeUnitOfWork& work, const CommandContext& context,
                                      const ResourceRef& resource, TimePoint now);
Outcome<std::optional<std::string>>
issue_bootstrap_credential(RuntimeUnitOfWork& work, RuntimeIdPort& ids, RuntimeSecretPort& secrets,
                           const std::string& resource_id, TimePoint now);
Status enqueue_host_command(RuntimeUnitOfWork& work, RuntimeSecretPort& secrets,
                            const PluginInstance& instance, TimePoint now);

// Every use case owns its transaction lifetime here; exceptions never escape the application.
template <typename Value>
Outcome<Value>
with_work(RuntimeUnitOfWorkFactory& transactions, const CommandContext& context,
          const std::function<Outcome<Value>(RuntimeUnitOfWork&, TimePoint now)>& body,
          bool resource_snapshot = false) {
  try {
    auto begun = resource_snapshot ? transactions.begin_resource_snapshot() : transactions.begin();
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

} // namespace kiarabot::core::target::runtime_resources_detail
