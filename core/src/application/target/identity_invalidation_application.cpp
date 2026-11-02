// clang-format off
#include "kiarabot/core/application/target/identity_invalidation_application.hpp"

#include <algorithm>
#include <exception>
#include <span>
#include <string>
#include <utility>

#include "identity_security_application_detail.hpp"
// clang-format on

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;

using detail::failure;

constexpr std::size_t kMaximumBatch = 256;
constexpr std::uint32_t kSnapshotPageLimit = 100;

Failure unavailable() {
  return failure(ErrorCode::UpstreamUnavailable);
}
Failure internal() {
  return failure(ErrorCode::Internal);
}
IdentityInvalidationRepository* invalidation_store(IdentitySecurityUnitOfWork& work) {
  return work.invalidations();
}
Outcome<detail::PeerAuthority> current_actor(detail::Work& work, const VerifiedPeer& presented,
                                             bool control_only) {
  auto read = detail::read_peer(*work.transaction, presented);
  if (const auto* error = std::get_if<Failure>(&read)) {
    return *error;
  }
  auto authority = std::get<detail::PeerAuthority>(std::move(read));
  const auto valid = detail::check_peer(authority, presented, work.now, control_only);
  if (const auto* error = std::get_if<Failure>(&valid)) {
    return *error;
  }
  return authority;
}
Status authorize_scope(const Principal& actor, std::span<const ExactAssignment> grants,
                       std::span<const ResourceRef> scope, Permission permission) {
  for (const auto& resource : scope) {
    const auto allowed = authorize_exact(actor, permission, resource, grants);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
  }
  return std::monostate{};
}
Outcome<InvalidationWatermark> watermark_of(IdentitySecurityUnitOfWork& work,
                                            std::string_view expected_epoch) {
  auto repo = invalidation_store(work);
  if (!repo) {
    return unavailable();
  }
  auto state = repo->state();
  if (const auto* error = std::get_if<Failure>(&state)) {
    return *error;
  }
  const auto& current = std::get<std::optional<InvalidationStreamState>>(state);
  if (!current || current->epoch.empty()) {
    return internal();
  }
  if (!expected_epoch.empty() && current->epoch != expected_epoch) {
    // The durable log was reinitialized/replaced: continuity cannot be proven.
    return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
  }
  return InvalidationWatermark{current->epoch, current->next_sequence};
}
class Application final : public IdentityInvalidationApplication {
public:
  Application(IdentitySecurityUnitOfWorkFactory& transactions, RandomPort& random,
              IdentityInvalidationOptions options)
      : transactions_(transactions), random_(random), batch_(options.batch) {}
  Outcome<InvalidationWatchStart> open_watch(const InvalidationWatchRequest& request) override {
    try {
      return open_impl(request);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal();
    }
  }
  Outcome<InvalidationPollReply> poll(const InvalidationPollRequest& request) override {
    try {
      return poll_impl(request);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal();
    }
  }
  Outcome<AuthorizationSnapshotReply>
  snapshot(const AuthorizationSnapshotRequest& request) override {
    try {
      return snapshot_impl(request);
    } catch (const std::bad_alloc&) {
      return unavailable();
    } catch (...) {
      return internal();
    }
  }

private:
  Outcome<InvalidationWatchStart> open_impl(const InvalidationWatchRequest& request) {
    const auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    const auto cursor_shape = validate_invalidation_cursor(request.cursor);
    if (const auto* error = std::get_if<Failure>(&cursor_shape)) {
      return *error;
    }
    if (!unique_invalidation_scope(request.scope)) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto work = std::get<detail::Work>(std::move(opened));
    auto actor = current_actor(work, request.peer, false);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(actor);
    const auto allowed = authorize_scope(authority.subject.principal, authority.subject.assignments,
                                         request.scope, Permission::IdentityInvalidationRead);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
    auto repo = invalidation_store(*work.transaction);
    if (!repo) {
      return unavailable();
    }
    auto epoch = ensure_identity_invalidation_epoch(*work.transaction, random_);
    if (const auto* error = std::get_if<Failure>(&epoch)) {
      return *error;
    }
    if (const auto* error = std::get_if<Failure>(&epoch)) {
      return *error;
    }
    const auto watermark = watermark_of(*work.transaction, std::get<std::string>(epoch));
    if (const auto* error = std::get_if<Failure>(&watermark)) {
      return *error;
    }
    const auto current = std::get<InvalidationWatermark>(watermark);
    InvalidationWatchStart start;
    start.watermark = current;
    auto oldest = repo->oldest_sequence(current.epoch);
    if (const auto* error = std::get_if<Failure>(&oldest)) {
      return *error;
    }
    const auto& retained = std::get<std::optional<std::uint64_t>>(oldest);
    start.resync_required = invalidation_resync_required(current, retained, request.cursor);
    if (!start.resync_required) {
      const auto continuation = validate_invalidation_continuation(current, request.cursor);
      if (const auto* error = std::get_if<Failure>(&continuation)) {
        return *error;
      }
      auto records =
          repo->read_after(current.epoch, request.cursor.sequence, std::min(batch_, kMaximumBatch));
      if (const auto* error = std::get_if<Failure>(&records)) {
        return *error;
      }
      start.backlog = std::move(std::get<std::vector<InvalidationRecord>>(records));
    }
    const auto committed = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&committed)) {
      return *error;
    }
    return start;
  }
  Outcome<InvalidationPollReply> poll_impl(const InvalidationPollRequest& request) {
    const auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    if (!unique_invalidation_scope(request.scope) || request.epoch.empty() || request.limit == 0 ||
        request.limit > kMaximumBatch) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto work = std::get<detail::Work>(std::move(opened));
    auto actor = current_actor(work, request.peer, false);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(actor);
    const auto allowed = authorize_scope(authority.subject.principal, authority.subject.assignments,
                                         request.scope, Permission::IdentityInvalidationRead);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
    InvalidationPollReply reply;
    const auto watermark = watermark_of(*work.transaction, request.epoch);
    if (const auto* error = std::get_if<Failure>(&watermark)) {
      if (error->code == ErrorCode::Conflict) {
        // Epoch changed while subscribed: signal resync instead of pretending continuity.
        auto current = watermark_of(*work.transaction, {});
        if (const auto* current_error = std::get_if<Failure>(&current)) {
          return *current_error;
        }
        reply.watermark = std::get<InvalidationWatermark>(current);
        reply.resync_required = true;
        const auto committed = detail::committed(*work.transaction);
        if (const auto* commit_error = std::get_if<Failure>(&committed)) {
          return *commit_error;
        }
        return reply;
      }
      return *error;
    }
    reply.watermark = std::get<InvalidationWatermark>(watermark);
    return poll_records(work, request, std::move(reply));
  }
  Outcome<InvalidationPollReply> poll_records(detail::Work& work,
                                              const InvalidationPollRequest& request,
                                              InvalidationPollReply reply) {
    auto repo = invalidation_store(*work.transaction);
    if (!repo) {
      return unavailable();
    }
    auto records = repo->read_after(reply.watermark.epoch, request.after_sequence,
                                    std::min(request.limit, kMaximumBatch));
    if (const auto* error = std::get_if<Failure>(&records)) {
      return *error;
    }
    reply.records = std::move(std::get<std::vector<InvalidationRecord>>(records));
    const auto committed = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&committed)) {
      return *error;
    }
    return reply;
  }
  Outcome<AuthorizationSnapshotReply> snapshot_impl(const AuthorizationSnapshotRequest& request) {
    const auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    const auto cursor_shape = validate_invalidation_cursor(request.after);
    if (const auto* error = std::get_if<Failure>(&cursor_shape)) {
      return *error;
    }
    if (!unique_invalidation_scope(request.scope)) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto work = std::get<detail::Work>(std::move(opened));
    auto actor = current_actor(work, request.peer, false);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(actor);
    const auto allowed = authorize_scope(authority.subject.principal, authority.subject.assignments,
                                         request.scope, Permission::IdentityInvalidationRead);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
    auto epoch = ensure_identity_invalidation_epoch(*work.transaction, random_);
    if (const auto* error = std::get_if<Failure>(&epoch)) {
      return *error;
    }
    const auto watermark = watermark_of(*work.transaction, std::get<std::string>(epoch));
    if (const auto* error = std::get_if<Failure>(&watermark)) {
      return *error;
    }
    const auto current = std::get<InvalidationWatermark>(watermark);
    auto records = work.transaction->exact_assignments().for_scope(request.scope);
    if (const auto* error = std::get_if<Failure>(&records)) {
      return *error;
    }
    AuthorizationSnapshotReply reply;
    reply.cursor = InvalidationCursor{std::string{kInvalidationSourceService}, current.epoch,
                                      request.scope, current.sequence};
    for (const auto& assignment : std::get<std::vector<ExactAssignment>>(records)) {
      const auto projected = project_assignment(assignment);
      if (std::holds_alternative<Failure>(projected)) {
        return internal();
      }
      reply.assignments.push_back(std::get<AssignmentProjection>(projected));
      auto authorization = work.transaction->authorizations().find(assignment.subject);
      if (const auto* error = std::get_if<Failure>(&authorization)) {
        return *error;
      }
      const auto& state = std::get<std::optional<AuthorizationState>>(authorization);
      if (!state || state->subject != assignment.subject || state->version.value == 0) {
        return internal();
      }
      if (state->version.value > reply.authorization_version.value) {
        reply.authorization_version = state->version;
      }
    }
    if (reply.assignments.size() > kSnapshotPageLimit) {
      return failure(ErrorCode::RateLimited, Recovery::Backoff);
    }
    const auto checkpoint = detail::final_check(work, request.context, TimePoint::max(), false);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    reply.checked_at = work.now;
    const auto committed = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&committed)) {
      return *error;
    }
    return reply;
  }

  IdentitySecurityUnitOfWorkFactory& transactions_;
  RandomPort& random_;
  std::size_t batch_;
};
} // namespace

Outcome<std::string> ensure_identity_invalidation_epoch(IdentitySecurityUnitOfWork& work,
                                                        RandomPort& random) {
  auto repo = invalidation_store(work);
  if (!repo) {
    return unavailable();
  }
  auto existing = repo->state();
  if (const auto* error = std::get_if<Failure>(&existing)) {
    return *error;
  }
  if (const auto& state = std::get<std::optional<InvalidationStreamState>>(existing); state) {
    if (state->epoch.empty()) {
      return internal();
    }
    return state->epoch;
  }
  auto generated = detail::random_uuid(random);
  if (const auto* error = std::get_if<Failure>(&generated)) {
    return *error;
  }
  const auto initialized =
      repo->initialize(InvalidationStreamState{std::get<std::string>(generated), 0});
  if (const auto* error = std::get_if<Failure>(&initialized)) {
    return *error;
  }
  auto confirmed = repo->state();
  if (const auto* error = std::get_if<Failure>(&confirmed)) {
    return *error;
  }
  const auto& state = std::get<std::optional<InvalidationStreamState>>(confirmed);
  if (!state || state->epoch.empty()) {
    return internal();
  }
  return state->epoch;
}
Outcome<InvalidationRecord> append_identity_invalidation(IdentitySecurityUnitOfWork& work,
                                                         RandomPort& random,
                                                         const InvalidationEvent& event,
                                                         TimePoint now) {
  const auto epoch = ensure_identity_invalidation_epoch(work, random);
  if (const auto* error = std::get_if<Failure>(&epoch)) {
    return *error;
  }
  auto repo = invalidation_store(work);
  if (!repo) {
    return unavailable();
  }
  return repo->append(event, now);
}

Outcome<std::unique_ptr<IdentityInvalidationApplication>>
make_identity_invalidation_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                       RandomPort& random, IdentityInvalidationOptions options) {
  if (options.batch == 0 || options.batch > kMaximumBatch) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<IdentityInvalidationApplication>{
        std::make_unique<Application>(transactions, random, options)};
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return internal();
  }
}
} // namespace kiarabot::core::target
