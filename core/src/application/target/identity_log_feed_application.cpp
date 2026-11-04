#include "kiarabot/core/application/target/identity_log_feed_application.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "identity_security_application_detail.hpp"

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;
Failure invalid() {
  return detail::failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
}
Failure unavailable() {
  return detail::failure(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
}
bool control_scope(const std::vector<ResourceRef>& scope) {
  return scope.size() == 1 && scope.front().kind == ResourceKind::Catalog &&
         scope.front().id.value == "control";
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
bool cursor_valid(const ObservationFeedCursor& cursor, const std::vector<ResourceRef>& scope) {
  return cursor.owner == Service::Identity && cursor.family == ObservationFamily::Logs &&
         !cursor.epoch.empty() && cursor.epoch.size() <= 128 &&
         cursor.epoch.find('\0') == std::string::npos && cursor.scope == scope;
}
bool valid_page(const ObservationFeedPage& page, const ObservationFeedQuery& query) {
  if (page.next_cursor.owner != Service::Identity ||
      page.next_cursor.family != ObservationFamily::Logs || page.next_cursor.scope != query.scope ||
      page.next_cursor.epoch.empty() || page.checked_at <= TimePoint{} ||
      page.changes.size() > (query.page_size == 0 ? 32 : query.page_size) ||
      (page.has_more && page.changes.empty())) {
    return false;
  }
  auto sequence = query.cursor ? query.cursor->sequence : page.next_cursor.sequence;
  if (query.cursor &&
      (query.cursor->epoch != page.next_cursor.epoch || sequence > page.next_cursor.sequence)) {
    return false;
  }
  if (!query.cursor && !page.changes.empty()) {
    return false;
  }
  for (const auto& change : page.changes) {
    const auto* log = std::get_if<LogObservation>(&change.payload);
    if (log == nullptr || (log->owner != Service::Identity && log->owner != Service::Runtime) ||
        log->occurred_at <= TimePoint{} || change.cursor.owner != Service::Identity ||
        change.cursor.family != ObservationFamily::Logs || change.cursor.scope != query.scope ||
        change.cursor.epoch != page.next_cursor.epoch || change.cursor.sequence <= sequence ||
        change.cursor.sequence > page.next_cursor.sequence) {
      return false;
    }
    sequence = change.cursor.sequence;
  }
  return !page.has_more || page.next_cursor.sequence == sequence;
}

class IdentityLogFeedApplicationImpl final : public IdentityLogFeedApplication {
public:
  IdentityLogFeedApplicationImpl(
      IdentitySecurityUnitOfWorkFactory& transactions,
      std::function<Outcome<ObservationFeedPage>(UnitOfWork&, const ObservationFeedQuery&)> query)
      : transactions_(transactions), query_(std::move(query)) {}
  Outcome<ObservationFeedPage> list_log_changes(const IdentityFeedCommand& command) override {
    try {
      auto input = command.query;
      if (input.family != ObservationFamily::Logs || input.page_size > 100 ||
          !control_scope(input.scope) ||
          (input.cursor && !cursor_valid(*input.cursor, input.scope))) {
        return invalid();
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
      auto authorized = authorize_exact(authority.subject.principal, Permission::LogRead,
                                        input.scope.front(), authority.subject.assignments);
      if (const auto* error = std::get_if<Failure>(&authorized)) {
        return *error;
      }
      auto checkpoint =
          detail::final_check(work, command.context, peer_valid_until(authority), true);
      if (const auto* error = std::get_if<Failure>(&checkpoint)) {
        return *error;
      }
      if (std::chrono::steady_clock::now() >= input.deadline) {
        return detail::failure(ErrorCode::Timeout, Recovery::Backoff);
      }
      input.deadline = std::min(input.deadline,
                                std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
      auto queried = query_(*work.transaction, input);
      work.transaction->rollback();
      if (const auto* error = std::get_if<Failure>(&queried)) {
        return *error;
      }
      auto page = std::get<ObservationFeedPage>(std::move(queried));
      page.checked_at = work.now;
      if (!valid_page(page, input) || page.checked_at > work.now + std::chrono::seconds{2}) {
        return unavailable();
      }
      return page;
    } catch (...) {
      return unavailable();
    }
  }

private:
  IdentitySecurityUnitOfWorkFactory& transactions_;
  std::function<Outcome<ObservationFeedPage>(UnitOfWork&, const ObservationFeedQuery&)> query_;
};
} // namespace

Outcome<std::unique_ptr<IdentityLogFeedApplication>> make_identity_log_feed_application(
    IdentitySecurityUnitOfWorkFactory& transactions,
    std::function<Outcome<ObservationFeedPage>(UnitOfWork&, const ObservationFeedQuery&)> query) {
  if (!query) {
    return invalid();
  }
  try {
    return std::unique_ptr<IdentityLogFeedApplication>(
        std::make_unique<IdentityLogFeedApplicationImpl>(transactions, std::move(query)));
  } catch (...) {
    return unavailable();
  }
}
} // namespace kiarabot::core::target
