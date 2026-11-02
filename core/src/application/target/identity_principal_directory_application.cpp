#include "kiarabot/core/application/target/identity_principal_directory_application.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
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
Outcome<detail::PeerAuthority> current_actor(IdentitySecurityUnitOfWork& work,
                                             const VerifiedPeer& peer, TimePoint now) {
  auto read = detail::read_peer(work, peer);
  if (const auto* error = std::get_if<Failure>(&read)) {
    return *error;
  }
  auto authority = std::get<detail::PeerAuthority>(read);
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
bool valid_uuid(std::string_view value) {
  return value.size() == 36 && value[8] == '-' && value[13] == '-' && value[18] == '-' &&
         value[23] == '-';
}
bool deleted_or_purged(LifecycleState state) {
  return state == LifecycleState::Deleted || state == LifecycleState::Purged;
}
std::string encode_token(const SubjectId& last, std::optional<LifecycleState> filter) {
  return "v1:" + std::to_string(filter ? static_cast<int>(*filter) : -1) + ":" + last.value;
}
std::optional<SubjectId> decode_token(std::string_view token,
                                      std::optional<LifecycleState> filter) {
  constexpr std::string_view prefix = "v1:";
  if (!token.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto separator = token.find(':', prefix.size());
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  const auto state = token.substr(prefix.size(), separator - prefix.size());
  if (state != std::to_string(filter ? static_cast<int>(*filter) : -1)) {
    return std::nullopt;
  }
  const auto id = token.substr(separator + 1);
  if (!valid_uuid(id)) {
    return std::nullopt;
  }
  return SubjectId{std::string{id}};
}
// A catalog-control grant is the management directory scope; every other grant only reveals the
// principals/roles on that explicit resource.
std::vector<ResourceRef> visible_resources(const detail::PeerAuthority& authority, bool& catalog) {
  std::vector<ResourceRef> visible;
  std::set<std::pair<int, std::string>> seen;
  catalog = false;
  for (const auto& assignment : authority.subject.assignments) {
    std::vector<ResourceRef> resources;
    for (const auto& role : assignment.roles) {
      resources.push_back(role.resource);
    }
    for (const auto& binding : assignment.explicit_permissions) {
      resources.push_back(binding.resource);
    }
    for (const auto& resource : resources) {
      const auto authorized =
          authorize_exact(authority.subject.principal, Permission::IdentityPrincipalRead, resource,
                          authority.subject.assignments);
      if (std::holds_alternative<Failure>(authorized)) {
        continue;
      }
      if (resource.kind == ResourceKind::Catalog && resource.id.value == "control") {
        catalog = true;
      } else if (seen.emplace(static_cast<int>(resource.kind), resource.id.value).second) {
        visible.push_back(resource);
      }
    }
  }
  return visible;
}
class IdentityPrincipalDirectoryApplicationImpl final
    : public IdentityPrincipalDirectoryApplication {
public:
  explicit IdentityPrincipalDirectoryApplicationImpl(
      IdentitySecurityUnitOfWorkFactory& transactions)
      : transactions_(transactions) {}

  Outcome<PrincipalDirectoryReply>
  list_principals(const PrincipalDirectoryCommand& command) override {
    try {
      const std::uint32_t page_size =
          command.page_size == 0 ? 20 : std::min<std::uint32_t>(command.page_size, 100);
      if (command.page_size > 100 || command.page_token.size() > 128) {
        return invalid();
      }
      std::optional<SubjectId> after;
      if (!command.page_token.empty()) {
        after = decode_token(command.page_token, command.state_filter);
        if (!after) {
          return invalid();
        }
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
      bool catalog = false;
      auto visible = visible_resources(authority, catalog);
      if (!catalog && visible.empty()) {
        return detail::failure(ErrorCode::PermissionDenied, Recovery::Authenticate);
      }
      auto checkpoint =
          detail::final_check(work, command.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      if (command.state_filter && deleted_or_purged(*command.state_filter)) {
        // Deleted/purged principals are never exposed; the explicit filter is an empty page with
        // the authoritative database observation time.
        PrincipalDirectoryReply reply;
        reply.page_size = page_size;
        reply.checked_at = work.now;
        work.transaction->rollback();
        return reply;
      }
      PrincipalDirectoryQuery query;
      query.after = after;
      query.limit = page_size;
      query.include_all_states = !command.state_filter.has_value();
      query.state_filter = command.state_filter.value_or(LifecycleState::Active);
      query.catalog_scope = catalog;
      query.visible_resources = std::move(visible);
      auto queried = work.transaction->query_principals(query);
      if (std::holds_alternative<Failure>(queried)) {
        return unavailable();
      }
      auto page = std::get<PrincipalDirectoryPage>(std::move(queried));
      PrincipalDirectoryReply reply;
      reply.page_size = page_size;
      reply.checked_at = work.now;
      for (auto& entry : page.entries) {
        reply.principals.push_back(
            PrincipalDirectorySummary{std::move(entry.principal), std::move(entry.roles)});
      }
      if (page.has_more && !reply.principals.empty()) {
        reply.next_page_token =
            encode_token(reply.principals.back().principal.id, command.state_filter);
      }
      work.transaction->rollback();
      return reply;
    } catch (...) {
      return unavailable();
    }
  }

private:
  IdentitySecurityUnitOfWorkFactory& transactions_;
};
} // namespace

Outcome<std::unique_ptr<IdentityPrincipalDirectoryApplication>>
make_identity_principal_directory_application(IdentitySecurityUnitOfWorkFactory& transactions) {
  try {
    return std::unique_ptr<IdentityPrincipalDirectoryApplication>(
        std::make_unique<IdentityPrincipalDirectoryApplicationImpl>(transactions));
  } catch (...) {
    return unavailable();
  }
}
} // namespace kiarabot::core::target
