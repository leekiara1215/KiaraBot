#include "common.hpp"

#include "kiarabot/core/application/target/runtime_resources/bootstrap.hpp"

namespace kiarabot::core::target::runtime_resources_detail {
Failure failure(ErrorCode code, Recovery recovery, Acceptance acceptance) {
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

ReceiptKey receipt_key(const CommandContext& context, const ResourceRef& resource) {
  return {context.authentication.subject, context.method, resource, context.idempotency_key};
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

Outcome<RuntimeAuthorizationDecision>
authorize_catalog(RuntimeAuthorizationPort& authorization,
                  const std::map<ResourceKind, ResourceId>& catalogs, const CommandContext& context,
                  ResourceKind family, const ResourceRef& catalog, Permission permission) {
  if (catalog.kind != ResourceKind::Catalog || !canonical(catalog.id.value)) {
    return invalid_runtime_input();
  }
  const auto configured = catalogs.find(family);
  if (configured == catalogs.end()) {
    return failure(ErrorCode::CapabilityNotSupported);
  }
  if (configured->second != catalog.id) {
    return failure(ErrorCode::PermissionDenied);
  }
  return authorize(authorization, context.authentication, permission, context.method, {catalog},
                   context.deadline);
}

// Operation records are Runtime-owned progress rows created in the same UoW as their effect; a
// first P05 mutation has no asynchronous participant yet, so it starts Accepted with an audit
// trail rather than claiming completion.
Outcome<Operation> accepted_operation(RuntimeUnitOfWork& work, const CommandContext& context,
                                      const ResourceRef& resource, TimePoint now) {
  // The first request identity allocates this operation identity. The five management methods
  // persist that reference beside their success receipt, independently of later request IDs.
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
Outcome<std::optional<std::string>>
issue_bootstrap_credential(RuntimeUnitOfWork& work, RuntimeIdPort& ids, RuntimeSecretPort& secrets,
                           const std::string& resource_id, TimePoint now) {
  // The persisted version is authoritative even on the legacy omitted-recipient path: a reissued
  // credential must never reuse a version that an old proof/refresh could still present.
  auto latest = work.bridge_credentials().latest(resource_id);
  if (const auto* failure = std::get_if<Failure>(&latest)) {
    return *failure;
  }
  const auto& latest_record = std::get<std::optional<BridgeCredentialRecord>>(latest);
  if (latest_record && !latest_record->revoked) {
    return std::nullopt;
  }
  auto version = next_bootstrap_credential_version(latest_record);
  if (const auto* failure = std::get_if<Failure>(&version)) {
    return *failure;
  }
  auto text = secrets.bootstrap_credential();
  if (const auto* failure = std::get_if<Failure>(&text)) {
    return *failure;
  }
  auto digest = secrets.sha256(std::get<std::string>(text));
  if (const auto* failure = std::get_if<Failure>(&digest)) {
    return *failure;
  }
  auto id = ids.snapshot_id();
  if (const auto* failure = std::get_if<Failure>(&id)) {
    return *failure;
  }
  BridgeCredentialRecord record{
      std::get<std::string>(id),       resource_id, std::get<Sha256Digest>(digest), now, false,
      std::get<std::uint64_t>(version)};
  const auto saved = work.bridge_credentials().insert(record);
  if (const auto* failure = std::get_if<Failure>(&saved)) {
    return *failure;
  }
  return std::optional<std::string>{std::get<std::string>(text)};
}

} // namespace kiarabot::core::target::runtime_resources_detail
