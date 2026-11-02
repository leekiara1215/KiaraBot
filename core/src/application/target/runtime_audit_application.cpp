#include "kiarabot/core/application/target/runtime_audit_application.hpp"

#include <chrono>
#include <type_traits>
#include <utility>
#include <vector>

namespace kiarabot::core::target {
namespace {
Failure failure(ErrorCode code, Recovery recovery = Recovery::Diagnose) {
  return {code, Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
Failure invalid() {
  return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure unavailable() {
  return failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
Failure permission_denied() {
  return failure(ErrorCode::PermissionDenied, Recovery::Authenticate);
}
bool control_scope(const ResourceRef& scope) {
  return scope.kind == ResourceKind::Catalog && scope.id.value == "control";
}
Status validate(const RuntimeAuditCommand& command) {
  if (!control_scope(command.scope) || command.query.limit == 0 || command.query.limit > 100) {
    return invalid();
  }
  if (command.query.filter.occurred_from && command.query.filter.occurred_until &&
      *command.query.filter.occurred_from >= *command.query.filter.occurred_until) {
    return invalid();
  }
  if (command.query.cursor &&
      (command.query.cursor->id <= 0 || command.query.cursor->occurred_at <= TimePoint{})) {
    return invalid();
  }
  return std::monostate{};
}
class RuntimeAuditApplicationImpl final : public RuntimeAuditApplication {
public:
  explicit RuntimeAuditApplicationImpl(RuntimeAuditDependencies dependencies)
      : dependencies_(dependencies) {}
  Outcome<AuditPage> list_audit(const RuntimeAuditCommand& command) override {
    return list<AuditPage>(command, Permission::AuditRead);
  }
  Outcome<LogPage> list_logs(const RuntimeAuditCommand& command) override {
    return list<LogPage>(command, Permission::LogRead);
  }

private:
  Outcome<RuntimeAuthorizationDecision> authorize(const CommandContext& context,
                                                  Permission permission) {
    auto expected = std::vector<ResourceRef>{context.resource};
    auto checked = dependencies_.authorization.check(
        {context.authentication, permission, context.method, expected, context.deadline});
    if (const auto* error = std::get_if<Failure>(&checked)) {
      return *error;
    }
    auto decision = std::move(std::get<RuntimeAuthorizationDecision>(checked));
    if (!decision.allowed) {
      return decision.denial ? *decision.denial : permission_denied();
    }
    const auto sampled = TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
    if (decision.subject != context.authentication.subject || decision.permission != permission ||
        decision.scope != expected || decision.authorization_version.value == 0 ||
        decision.checked_at <= TimePoint{} ||
        decision.checked_at > sampled + std::chrono::seconds{2} ||
        sampled - decision.checked_at > std::chrono::seconds{60}) {
      return failure(ErrorCode::Internal);
    }
    return decision;
  }
  template <typename Page> Outcome<Page> list(const RuntimeAuditCommand& command,
                                              Permission permission) {
    try {
      if (const auto valid = validate(command); std::holds_alternative<Failure>(valid)) {
        return std::get<Failure>(valid);
      }
      auto authorized = authorize(command.context, permission);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return *error;
      }
      auto begun = dependencies_.transactions.begin();
      if (const auto* error = std::get_if<Failure>(&begun)) {
        return *error;
      }
      auto work = std::move(std::get<std::unique_ptr<RuntimeUnitOfWork>>(begun));
      if (!work) {
        return unavailable();
      }
      auto sampled = work->current_time();
      if (const auto* error = std::get_if<Failure>(&sampled)) {
        return *error;
      }
      Outcome<Page> queried;
      if constexpr (std::is_same_v<Page, AuditPage>) {
        queried = work->query_audit(command.query);
      } else {
        queried = work->query_logs(command.query);
      }
      if (const auto* error = std::get_if<Failure>(&queried)) {
        (void)error;
        return unavailable();
      }
      auto page = std::get<Page>(std::move(queried));
      page.checked_at = std::get<TimePoint>(sampled);
      work->rollback();
      return page;
    } catch (...) {
      return unavailable();
    }
  }
  RuntimeAuditDependencies dependencies_;
};
} // namespace

Outcome<std::unique_ptr<RuntimeAuditApplication>>
make_runtime_audit_application(RuntimeAuditDependencies dependencies) {
  try {
    return std::unique_ptr<RuntimeAuditApplication>(
        std::make_unique<RuntimeAuditApplicationImpl>(dependencies));
  } catch (...) {
    return unavailable();
  }
}
} // namespace kiarabot::core::target
