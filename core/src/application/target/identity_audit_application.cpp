#include "kiarabot/core/application/target/identity_audit_application.hpp"

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

#include "identity_security_application_detail.hpp"
#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;

Failure invalid() {
  return detail::failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure unavailable() {
  return detail::failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
bool control_scope(const ResourceRef& scope) {
  return scope.kind == ResourceKind::Catalog && scope.id.value == "control";
}
Outcome<detail::PeerAuthority> current_actor(IdentitySecurityUnitOfWork& work,
                                             const VerifiedPeer& peer, TimePoint now) {
  auto read = detail::read_peer(work, peer);
  if (const auto* error = std::get_if<Failure>(&read)) {
    return *error;
  }
  auto& authority = std::get<detail::PeerAuthority>(read);
  auto gated = detail::check_peer(authority, peer, now, true);
  if (const auto* error = std::get_if<Failure>(&gated)) {
    return *error;
  }
  return authority;
}
TimePoint peer_valid_until(const detail::PeerAuthority& authority) {
  auto expires = authority.credential.expires_at.value_or(TimePoint::min());
  const auto* certificate = std::get_if<CertificateBinding>(&authority.credential.material);
  if (!certificate) {
    return expires;
  }
  return std::min(expires, certificate->not_after);
}
Status validate(const IdentityAuditCommand& command) {
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
class IdentityAuditApplicationImpl final : public IdentityAuditApplication {
public:
  explicit IdentityAuditApplicationImpl(IdentitySecurityUnitOfWorkFactory& transactions)
      : transactions_(transactions) {}
  Outcome<AuditPage> list_audit(const IdentityAuditCommand& command) override {
    return query<AuditPage>(command, Permission::AuditRead, true);
  }
  Outcome<LogPage> list_logs(const IdentityAuditCommand& command) override {
    return query<LogPage>(command, Permission::LogRead, false);
  }

private:
  template <typename Page>
  Outcome<Page> query(const IdentityAuditCommand& command, Permission permission, bool audit) {
    try {
      if (const auto valid = validate(command); std::holds_alternative<Failure>(valid)) {
        return std::get<Failure>(valid);
      }
      auto opened = detail::begin(transactions_, command.context);
      if (const auto* error = std::get_if<Failure>(&opened)) {
        return *error;
      }
      auto& work = std::get<detail::Work>(opened);
      auto actor = current_actor(*work.transaction, command.control_peer, work.now);
      if (const auto* error = std::get_if<Failure>(&actor)) {
        return *error;
      }
      const auto& authority = std::get<detail::PeerAuthority>(actor);
      auto authorized = authorize_exact(authority.subject.principal, permission, command.scope,
                                        authority.subject.assignments);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return *error;
      }
      auto checkpoint = detail::final_check(work, command.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      Outcome<Page> queried;
      if constexpr (std::is_same_v<Page, AuditPage>) {
        queried = work.transaction->query_audit(command.query);
      } else {
        queried = work.transaction->query_logs(command.query);
      }
      if (const auto* error = std::get_if<Failure>(&queried)) {
        (void)error;
        return unavailable();
      }
      auto page = std::get<Page>(std::move(queried));
      page.checked_at = work.now;
      work.transaction->rollback();
      return page;
    } catch (...) {
      return unavailable();
    }
  }
  IdentitySecurityUnitOfWorkFactory& transactions_;
};
} // namespace

Outcome<std::unique_ptr<IdentityAuditApplication>>
make_identity_audit_application(IdentitySecurityUnitOfWorkFactory& transactions) {
  try {
    return std::unique_ptr<IdentityAuditApplication>(
        std::make_unique<IdentityAuditApplicationImpl>(transactions));
  } catch (...) {
    return unavailable();
  }
}
} // namespace kiarabot::core::target
