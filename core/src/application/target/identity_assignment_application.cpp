// clang-format off
#include "kiarabot/core/application/target/identity_assignment_application.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include "kiarabot/core/application/target/identity_invalidation_application.hpp"

#include "identity_security_application_detail.hpp"
// clang-format on

namespace kiarabot::core::target {
namespace {
namespace detail = identity_security_detail;

using detail::failure;

constexpr std::uint32_t kDefaultAssignmentPageSize = 20;
constexpr std::uint32_t kMaximumAssignmentPageSize = 100;
constexpr auto kAssignmentSnapshotLifetime = std::chrono::minutes{10};
constexpr auto kAssignmentReceiptRetention = std::chrono::hours{24};
constexpr std::size_t kAssignmentReceiptCapacity = 10000;
constexpr std::string_view kSetAssignmentMethod = "SetAssignment";
constexpr std::string_view kRevokeAssignmentMethod = "RevokeAssignment";

Failure conflict() {
  return failure(ErrorCode::Conflict, Recovery::ReadCurrent);
}
Failure invalid_output() {
  return failure(ErrorCode::Internal, Recovery::Diagnose, Acceptance::Unknown);
}
bool hex_digit(char character) {
  return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}
bool uuid_like(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!hex_digit(character)) {
      return false;
    }
  }
  return true;
}
bool valid_filter(const AssignmentScopeFilter& filter) {
  for (std::size_t index = 0; index < filter.resources.size(); ++index) {
    if (!valid_policy_resource(filter.resources[index])) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (filter.resources[previous] == filter.resources[index]) {
        return false;
      }
    }
  }
  return true;
}
Outcome<detail::PeerAuthority> current_control_actor(detail::Work& work,
                                                     const VerifiedPeer& presented) {
  auto read = detail::read_peer(*work.transaction, presented);
  if (const auto* error = std::get_if<Failure>(&read)) {
    return *error;
  }
  auto authority = std::get<detail::PeerAuthority>(std::move(read));
  const auto valid = detail::check_peer(authority, presented, work.now, true);
  if (const auto* error = std::get_if<Failure>(&valid)) {
    return *error;
  }
  return authority;
}
Outcome<Principal> current_target(IdentitySecurityUnitOfWork& work, const SubjectId& subject) {
  auto found = work.principals().find(subject);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& principal = std::get<std::optional<Principal>>(found);
  if (!principal) {
    return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
  }
  if (principal->id != subject || principal->version.value == 0 ||
      !known_principal_kind(principal->kind)) {
    return failure(ErrorCode::Internal);
  }
  if (principal->state != LifecycleState::Active) {
    return conflict();
  }
  return *principal;
}
Outcome<AuthorizationState> current_authorization(IdentitySecurityUnitOfWork& work,
                                                  const SubjectId& subject) {
  auto found = work.authorizations().find(subject);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  const auto& authorization = std::get<std::optional<AuthorizationState>>(found);
  if (!authorization || authorization->subject != subject || authorization->version.value == 0) {
    return failure(ErrorCode::Internal);
  }
  return *authorization;
}
// Advances the target subject's persistent authorization watermark in the SAME transaction that
// mutates its exact assignment, so consumers fetching the snapshot can detect the change.
Status advance_authorization(IdentitySecurityUnitOfWork& work, const AuthorizationState& current) {
  if (current.version.value == 0 ||
      current.version.value == std::numeric_limits<std::uint64_t>::max()) {
    return failure(ErrorCode::Internal);
  }
  const auto next = assignment_changed(current, current.version);
  if (const auto* error = std::get_if<Failure>(&next)) {
    return *error;
  }
  return work.authorizations().save(std::get<AuthorizationState>(next), current.version);
}
ReceiptKey set_receipt_key(const SubjectId& actor, const SubjectId& target,
                           const std::string& idempotency_key) {
  return {actor,
          std::string{kSetAssignmentMethod},
          {ResourceKind::Principal, ResourceId{target.value}},
          idempotency_key};
}
ReceiptKey revoke_receipt_key(const SubjectId& actor, const AssignmentId& target,
                              const std::string& idempotency_key) {
  return {actor,
          std::string{kRevokeAssignmentMethod},
          {ResourceKind::Assignment, ResourceId{target.value}},
          idempotency_key};
}
Outcome<std::optional<CommandReceipt>> stored_receipt(IdentitySecurityUnitOfWork& work,
                                                      const ReceiptKey& key) {
  auto found = work.receipts().find(key);
  if (const auto* error = std::get_if<Failure>(&found)) {
    return *error;
  }
  return std::get<std::optional<CommandReceipt>>(found);
}
bool well_formed_assignment(const ExactAssignment& assignment) {
  if (!uuid_like(assignment.id.value) || !uuid_like(assignment.subject.value) ||
      assignment.version.value == 0 ||
      std::holds_alternative<Failure>(validate_exact_assignment(assignment))) {
    return false;
  }
  return true;
}
bool positive_receipt_time(TimePoint value) {
  return value > TimePoint{} && value < TimePoint::max();
}
Outcome<const ExactAssignment*> replay_assignment(const CommandReceipt& receipt,
                                                  const Sha256Digest& fingerprint) {
  if (!(receipt.fingerprint == fingerprint)) {
    return conflict();
  }
  const auto* assignment = std::get_if<ExactAssignment>(&receipt.result);
  if (!assignment || !well_formed_assignment(*assignment) ||
      !positive_receipt_time(receipt.created_at)) {
    return invalid_output();
  }
  return assignment;
}
Outcome<std::string> new_uuid(RandomPort& random) {
  auto generated = detail::random_uuid(random);
  if (const auto* error = std::get_if<Failure>(&generated)) {
    return *error;
  }
  return std::get<std::string>(generated);
}
// A stored assignment record is converted back into the request basis only for management-scope
// authorization; the tombstone itself (never this projection) remains the authority.
Outcome<ExactAssignmentSpec> spec_from_assignment(const ExactAssignment& assignment) {
  ExactAssignmentSpec spec{assignment.subject, ExplicitAssignmentSpec{{}, {}}};
  if (!assignment.roles.empty()) {
    RoleAssignmentSpec role{assignment.roles.front().role, {}};
    for (const auto& binding : assignment.roles) {
      if (binding.role != role.role) {
        return failure(ErrorCode::Internal);
      }
      role.scope.push_back(binding.resource);
    }
    spec.basis = std::move(role);
    return spec;
  }
  ExplicitAssignmentSpec explicit_spec{{}, {}};
  for (const auto& binding : assignment.explicit_permissions) {
    if (std::find(explicit_spec.permissions.begin(), explicit_spec.permissions.end(),
                  binding.permission) == explicit_spec.permissions.end()) {
      explicit_spec.permissions.push_back(binding.permission);
    }
    if (std::find(explicit_spec.scope.begin(), explicit_spec.scope.end(), binding.resource) ==
        explicit_spec.scope.end()) {
      explicit_spec.scope.push_back(binding.resource);
    }
  }
  spec.basis = std::move(explicit_spec);
  return spec;
}
Status authorize_management(const Principal& actor, const ExactAssignment* previous,
                            const ExactAssignmentSpec& proposed,
                            std::span<const ExactAssignment> grants) {
  return authorize_assignment_change(actor, previous, proposed, grants);
}
Status authorize_list(const Principal& actor, const SubjectId& target,
                      const AssignmentScopeFilter& filter,
                      std::span<const ExactAssignment> grants) {
  const auto target_scope =
      authorize_exact(actor, Permission::IdentityAssignmentRead,
                      {ResourceKind::Principal, ResourceId{target.value}}, grants);
  if (const auto* error = std::get_if<Failure>(&target_scope)) {
    return *error;
  }
  for (const auto& resource : filter.resources) {
    const auto allowed =
        authorize_exact(actor, Permission::IdentityAssignmentRead, resource, grants);
    if (const auto* error = std::get_if<Failure>(&allowed)) {
      return *error;
    }
  }
  return std::monostate{};
}
bool inside_filter(const ExactAssignment& assignment, const AssignmentScopeFilter& filter) {
  if (filter.resources.empty()) {
    return true;
  }
  for (const auto& resource : assignment_scope(assignment)) {
    if (std::find(filter.resources.begin(), filter.resources.end(), resource) ==
        filter.resources.end()) {
      return false;
    }
  }
  return true;
}
struct FilterOwner {
  SubjectId actor;
  SubjectId target;
  std::vector<ResourceRef> filter;
  ResourceVersion authorization_version;
  std::uint32_t page_size = 0;
  bool operator==(const FilterOwner&) const = default;
};
struct PageState {
  FilterOwner owner;
  std::string id;
  std::vector<std::string> cursors;
  std::vector<AssignmentProjection> records;
  TimePoint expires_at;
};
// Bounded, restart-ephemeral immutable list snapshots. Every continuation freshly authorizes the
// caller and rechecks the target subject's current authorization version before serving a page.
class AssignmentSnapshots {
public:
  AssignmentSnapshots(std::size_t maximum_snapshots, std::size_t maximum_records)
      : maximum_snapshots_(maximum_snapshots), maximum_records_(maximum_records) {}

  Outcome<ListAssignmentsReply> insert(PageState page, TimePoint now) {
    std::lock_guard lock(mutex_);
    prune(now);
    if (page.owner.page_size == 0 || page.owner.page_size > kMaximumAssignmentPageSize ||
        page.id.empty() || page.expires_at <= now || page.cursors.empty() ||
        !page.cursors.front().empty()) {
      return failure(ErrorCode::Internal);
    }
    const auto maximum_expiry = detail::add_lifetime(now, kAssignmentSnapshotLifetime);
    if (const auto* error = std::get_if<Failure>(&maximum_expiry)) {
      return *error;
    }
    if (page.expires_at > std::get<TimePoint>(maximum_expiry)) {
      return failure(ErrorCode::Internal);
    }
    const auto pages = page.records.empty() ? std::size_t{1}
                                            : (page.records.size() - 1) / page.owner.page_size + 1;
    if (page.cursors.size() != pages) {
      return failure(ErrorCode::Internal);
    }
    std::set<std::string> unique_cursors;
    for (const auto& cursor : page.cursors) {
      if (!unique_cursors.insert(cursor).second) {
        return failure(ErrorCode::Internal);
      }
    }
    if (snapshots_.size() >= maximum_snapshots_ ||
        page.records.size() > maximum_records_ - records_) {
      return failure(ErrorCode::RateLimited, Recovery::Backoff);
    }
    const auto id = page.id;
    auto stored = snapshots_.emplace(id, std::move(page)).first;
    try {
      for (std::size_t index = 1; index < stored->second.cursors.size(); ++index) {
        cursor_index_.emplace(stored->second.cursors[index], std::make_pair(id, index));
      }
    } catch (...) {
      for (std::size_t index = 1; index < stored->second.cursors.size(); ++index) {
        cursor_index_.erase(stored->second.cursors[index]);
      }
      snapshots_.erase(stored);
      throw;
    }
    records_ += stored->second.records.size();
    return page_reply(stored->second, 0, now);
  }

  Outcome<ListAssignmentsReply> read(const FilterOwner& owner, const AssignmentPageRequest& request,
                                     TimePoint now, ResourceVersion current_version) {
    std::lock_guard lock(mutex_);
    prune(now);
    std::string id = request.snapshot_id;
    std::size_t index = 0;
    if (!request.page_token.empty()) {
      const auto cursor = cursor_index_.find(request.page_token);
      if (cursor == cursor_index_.end()) {
        return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      if (!id.empty() && id != cursor->second.first) {
        return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
      }
      id = cursor->second.first;
      index = cursor->second.second;
    } else if (!id.empty()) {
      index = 0;
    } else {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    const auto found = snapshots_.find(id);
    if (found == snapshots_.end()) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    const auto& stored = found->second;
    if (stored.owner.actor != owner.actor || stored.owner.target != owner.target ||
        stored.owner.filter != owner.filter ||
        (owner.page_size != 0 && owner.page_size != stored.owner.page_size)) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    if (current_version != stored.owner.authorization_version) {
      return conflict();
    }
    return page_reply(stored, index, now);
  }

private:
  static ListAssignmentsReply page_reply(const PageState& page, std::size_t index, TimePoint now) {
    const auto begin = std::min(page.records.size(), index * page.owner.page_size);
    const auto end = std::min(page.records.size(), begin + page.owner.page_size);
    ListAssignmentsReply reply;
    reply.assignments.assign(page.records.begin() + static_cast<std::ptrdiff_t>(begin),
                             page.records.begin() + static_cast<std::ptrdiff_t>(end));
    reply.page_size = page.owner.page_size;
    reply.snapshot_id = page.id;
    reply.next_page_token = end < page.records.size() && index + 1 < page.cursors.size()
                                ? page.cursors[index + 1]
                                : std::string{};
    reply.expires_at = page.expires_at;
    reply.authorization_version = page.owner.authorization_version;
    reply.checked_at = now;
    return reply;
  }
  void prune(TimePoint now) {
    for (auto iterator = snapshots_.begin(); iterator != snapshots_.end();) {
      if (iterator->second.expires_at <= now) {
        records_ -= iterator->second.records.size();
        for (std::size_t index = 1; index < iterator->second.cursors.size(); ++index) {
          cursor_index_.erase(iterator->second.cursors[index]);
        }
        iterator = snapshots_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  std::size_t maximum_snapshots_;
  std::size_t maximum_records_;
  std::mutex mutex_;
  std::map<std::string, PageState> snapshots_;
  std::map<std::string, std::pair<std::string, std::size_t>> cursor_index_;
  std::size_t records_ = 0;
};
class Application final : public IdentityAssignmentApplication {
public:
  Application(IdentitySecurityUnitOfWorkFactory& transactions,
              IdentityAssignmentFingerprintPort& fingerprints, RandomPort& random,
              IdentityAssignmentOptions options)
      : transactions_(transactions), fingerprints_(fingerprints), random_(random),
        snapshots_(options.maximum_snapshots, options.maximum_snapshot_records) {}

  Outcome<ListAssignmentsReply> list_assignments(const ListAssignmentsRequest& request) override {
    try {
      return list_impl(request);
    } catch (const std::bad_alloc&) {
      return failure(ErrorCode::UpstreamUnavailable);
    } catch (...) {
      return failure(ErrorCode::Internal);
    }
  }
  Outcome<SetAssignmentReply> set_assignment(const SetAssignmentRequest& request) override {
    try {
      return set_impl(request);
    } catch (const std::bad_alloc&) {
      return failure(ErrorCode::UpstreamUnavailable);
    } catch (...) {
      return failure(ErrorCode::Internal);
    }
  }
  Outcome<RevokeAssignmentReply>
  revoke_assignment(const RevokeAssignmentRequest& request) override {
    try {
      return revoke_impl(request);
    } catch (const std::bad_alloc&) {
      return failure(ErrorCode::UpstreamUnavailable);
    } catch (...) {
      return failure(ErrorCode::Internal);
    }
  }

private:
  Outcome<ListAssignmentsReply> list_impl(const ListAssignmentsRequest& request) {
    const auto shape = detail::context_valid(request.context, false);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    if (!uuid_like(request.subject.value) || !valid_filter(request.filter) ||
        request.page.page_size > kMaximumAssignmentPageSize) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto work = std::get<detail::Work>(std::move(opened));
    auto actor = current_control_actor(work, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(actor);
    const auto authorized = authorize_list(authority.subject.principal, request.subject,
                                           request.filter, authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&authorized)) {
      return *error;
    }
    const auto authorization = current_authorization(*work.transaction, request.subject);
    if (const auto* error = std::get_if<Failure>(&authorization)) {
      return *error;
    }
    const auto version = std::get<AuthorizationState>(authorization).version;
    const auto now = work.now;
    if (!request.page.page_token.empty() || !request.page.snapshot_id.empty()) {
      // Continuation pages recheck the CURRENT authorization version of the target subject before
      // releasing this short read UoW; a changed watermark returns Conflict/ReadCurrent.
      work.transaction.reset();
      FilterOwner owner{authority.subject.principal.id, request.subject, request.filter.resources,
                        version, request.page.page_size};
      return snapshots_.read(owner, request.page, now, version);
    }
    const auto page_size =
        request.page.page_size == 0 ? kDefaultAssignmentPageSize : request.page.page_size;
    auto assignments = work.transaction->exact_assignments().for_subject(request.subject);
    if (const auto* error = std::get_if<Failure>(&assignments)) {
      return *error;
    }
    PageState page;
    page.owner = FilterOwner{authority.subject.principal.id, request.subject,
                             request.filter.resources, version, page_size};
    auto id = new_uuid(random_);
    if (const auto* error = std::get_if<Failure>(&id)) {
      return *error;
    }
    page.id = std::get<std::string>(id);
    const auto expiry = detail::add_lifetime(now, kAssignmentSnapshotLifetime);
    if (const auto* error = std::get_if<Failure>(&expiry)) {
      return *error;
    }
    page.expires_at = std::get<TimePoint>(expiry);
    for (const auto& assignment : std::get<std::vector<ExactAssignment>>(assignments)) {
      if (assignment.subject != request.subject || !inside_filter(assignment, request.filter)) {
        continue;
      }
      const auto projected = project_assignment(assignment);
      if (std::holds_alternative<Failure>(projected)) {
        return failure(ErrorCode::Internal);
      }
      page.records.push_back(std::get<AssignmentProjection>(projected));
    }
    const auto pages =
        page.records.empty() ? std::size_t{1} : (page.records.size() - 1) / page_size + 1;
    page.cursors.push_back(std::string{});
    for (std::size_t index = 1; index < pages; ++index) {
      auto cursor = new_uuid(random_);
      if (const auto* error = std::get_if<Failure>(&cursor)) {
        return *error;
      }
      page.cursors.push_back(std::get<std::string>(cursor));
    }
    // No UoW or repository reference survives into the bounded in-memory snapshot insert.
    work.transaction.reset();
    return snapshots_.insert(std::move(page), now);
  }
  Outcome<SetAssignmentReply> set_impl(const SetAssignmentRequest& request) {
    const auto shape = detail::context_valid(request.context, true);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    if (!uuid_like(request.spec.subject.value) ||
        (request.assignment_id && !uuid_like(request.assignment_id->value)) ||
        (request.expected_version && request.expected_version->value == 0) ||
        (request.assignment_id.has_value() != request.expected_version.has_value())) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    const auto digest =
        fingerprints_.set_assignment(request.spec, request.assignment_id, request.expected_version);
    if (const auto* error = std::get_if<Failure>(&digest)) {
      return *error;
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto work = std::get<detail::Work>(std::move(opened));
    auto actor = current_control_actor(work, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(actor);
    const auto key = set_receipt_key(authority.subject.principal.id, request.spec.subject,
                                     request.context.idempotency_key);
    auto stored = stored_receipt(*work.transaction, key);
    if (const auto* error = std::get_if<Failure>(&stored)) {
      return *error;
    }
    const auto& receipt = std::get<std::optional<CommandReceipt>>(stored);
    if (receipt) {
      auto replayed = replay_assignment(*receipt, std::get<Sha256Digest>(digest));
      if (const auto* error = std::get_if<Failure>(&replayed)) {
        return *error;
      }
      return SetAssignmentReply{*std::get<const ExactAssignment*>(replayed), receipt->created_at};
    }
    const auto target = current_target(*work.transaction, request.spec.subject);
    if (const auto* error = std::get_if<Failure>(&target)) {
      return *error;
    }
    std::optional<AssignmentStateRecord> previous;
    if (request.assignment_id) {
      auto current = work.transaction->exact_assignments().current(*request.assignment_id);
      if (const auto* error = std::get_if<Failure>(&current)) {
        return *error;
      }
      const auto& record = std::get<std::optional<AssignmentStateRecord>>(current);
      if (!record) {
        return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
      }
      previous = *record;
    }
    const auto prepared = prepare_live_assignment(
        request.assignment_id ? *request.assignment_id
                              : AssignmentId{std::get<std::string>(detail::random_uuid(random_))},
        request.spec, std::get<Principal>(target), previous ? &*previous : nullptr,
        request.expected_version);
    if (const auto* error = std::get_if<Failure>(&prepared)) {
      return *error;
    }
    auto& assignment = std::get<AssignmentStateRecord>(prepared);
    const auto manage = authorize_management(authority.subject.principal,
                                             previous ? &previous->assignment : nullptr,
                                             request.spec, authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&manage)) {
      return *error;
    }
    const auto authorization = current_authorization(*work.transaction, request.spec.subject);
    if (const auto* error = std::get_if<Failure>(&authorization)) {
      return *error;
    }
    const auto expiry = detail::add_lifetime(work.now, kAssignmentReceiptRetention);
    if (const auto* error = std::get_if<Failure>(&expiry)) {
      return *error;
    }
    const CommandReceipt write{key, std::get<Sha256Digest>(digest),
                               ReplayResult(assignment.assignment), work.now,
                               std::get<TimePoint>(expiry)};
    auto written =
        work.transaction->receipts().insert(write, ReceiptCapacity{kAssignmentReceiptCapacity});
    if (const auto* error = std::get_if<Failure>(&written)) {
      return *error;
    }
    const auto& write_result = std::get<ReceiptWrite>(written);
    if (write_result.duplicate) {
      auto replayed = replay_assignment(write_result.receipt, std::get<Sha256Digest>(digest));
      if (const auto* error = std::get_if<Failure>(&replayed)) {
        return *error;
      }
      return SetAssignmentReply{*std::get<const ExactAssignment*>(replayed),
                                write_result.receipt.created_at};
    }
    const auto& prior = std::get<AuthorizationState>(authorization);
    const auto advanced = advance_authorization(*work.transaction, prior);
    if (const auto* error = std::get_if<Failure>(&advanced)) {
      return *error;
    }
    // Same physical transaction: authorization watermark AND the bounded invalidation record are
    // committed together, so subscribers can never observe one without the other.
    const auto invalidation =
        append_identity_invalidation(*work.transaction, random_,
                                     InvalidationEvent{assignment.assignment.subject,
                                                       {prior.version.value + 1},
                                                       assignment_scope(assignment.assignment),
                                                       false},
                                     work.now);
    if (const auto* error = std::get_if<Failure>(&invalidation)) {
      return *error;
    }
    const auto saved = work.transaction->exact_assignments().save_state(
        assignment,
        previous ? std::optional<ResourceVersion>{previous->assignment.version} : std::nullopt);
    if (const auto* error = std::get_if<Failure>(&saved)) {
      return *error;
    }
    const auto audit = work.transaction->journal().append_audit(
        AuditEntry{authority.subject.principal.id,
                   {ResourceKind::Principal, ResourceId{request.spec.subject.value}},
                   request.context.request_id,
                   request.context.trace_id,
                   std::string{kSetAssignmentMethod},
                   std::nullopt,
                   assignment.assignment.version,
                   work.now});
    if (const auto* error = std::get_if<Failure>(&audit)) {
      return *error;
    }
    const auto checkpoint = detail::final_check(work, request.context, TimePoint::max(), false);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    const auto committed = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&committed)) {
      return *error;
    }
    return SetAssignmentReply{assignment.assignment, work.now};
  }
  Outcome<RevokeAssignmentReply> revoke_impl(const RevokeAssignmentRequest& request) {
    const auto shape = detail::context_valid(request.context, true);
    if (const auto* error = std::get_if<Failure>(&shape)) {
      return *error;
    }
    if (!uuid_like(request.assignment_id.value) || !request.expected_version ||
        request.expected_version->value == 0) {
      return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    const auto digest =
        fingerprints_.revoke_assignment(request.assignment_id, *request.expected_version);
    if (const auto* error = std::get_if<Failure>(&digest)) {
      return *error;
    }
    auto opened = detail::begin(transactions_, request.context);
    if (const auto* error = std::get_if<Failure>(&opened)) {
      return *error;
    }
    auto work = std::get<detail::Work>(std::move(opened));
    auto actor = current_control_actor(work, request.control_peer);
    if (const auto* error = std::get_if<Failure>(&actor)) {
      return *error;
    }
    const auto& authority = std::get<detail::PeerAuthority>(actor);
    const auto key = revoke_receipt_key(authority.subject.principal.id, request.assignment_id,
                                        request.context.idempotency_key);
    auto stored = stored_receipt(*work.transaction, key);
    if (const auto* error = std::get_if<Failure>(&stored)) {
      return *error;
    }
    const auto& receipt = std::get<std::optional<CommandReceipt>>(stored);
    if (receipt) {
      auto replayed = replay_assignment(*receipt, std::get<Sha256Digest>(digest));
      if (const auto* error = std::get_if<Failure>(&replayed)) {
        return *error;
      }
      return RevokeAssignmentReply{std::get<const ExactAssignment*>(replayed)->version,
                                   receipt->created_at};
    }
    auto current = work.transaction->exact_assignments().current(request.assignment_id);
    if (const auto* error = std::get_if<Failure>(&current)) {
      return *error;
    }
    const auto& record = std::get<std::optional<AssignmentStateRecord>>(current);
    if (!record) {
      return failure(ErrorCode::NotFound, Recovery::ReadCurrent);
    }
    const auto prepared =
        prepare_assignment_revocation(*record, *request.expected_version, work.now);
    if (const auto* error = std::get_if<Failure>(&prepared)) {
      return *error;
    }
    const auto& revoked = std::get<AssignmentStateRecord>(prepared);
    const auto spec = spec_from_assignment(record->assignment);
    if (const auto* error = std::get_if<Failure>(&spec)) {
      return *error;
    }
    const auto manage =
        authorize_management(authority.subject.principal, &record->assignment,
                             std::get<ExactAssignmentSpec>(spec), authority.subject.assignments);
    if (const auto* error = std::get_if<Failure>(&manage)) {
      return *error;
    }
    const auto authorization = current_authorization(*work.transaction, record->assignment.subject);
    if (const auto* error = std::get_if<Failure>(&authorization)) {
      return *error;
    }
    const auto expiry = detail::add_lifetime(work.now, kAssignmentReceiptRetention);
    if (const auto* error = std::get_if<Failure>(&expiry)) {
      return *error;
    }
    const CommandReceipt write{key, std::get<Sha256Digest>(digest),
                               ReplayResult(revoked.assignment), work.now,
                               std::get<TimePoint>(expiry)};
    auto written =
        work.transaction->receipts().insert(write, ReceiptCapacity{kAssignmentReceiptCapacity});
    if (const auto* error = std::get_if<Failure>(&written)) {
      return *error;
    }
    const auto& write_result = std::get<ReceiptWrite>(written);
    if (write_result.duplicate) {
      auto replayed = replay_assignment(write_result.receipt, std::get<Sha256Digest>(digest));
      if (const auto* error = std::get_if<Failure>(&replayed)) {
        return *error;
      }
      return RevokeAssignmentReply{std::get<const ExactAssignment*>(replayed)->version,
                                   write_result.receipt.created_at};
    }
    const auto& prior = std::get<AuthorizationState>(authorization);
    const auto advanced = advance_authorization(*work.transaction, prior);
    if (const auto* error = std::get_if<Failure>(&advanced)) {
      return *error;
    }
    const auto invalidation = append_identity_invalidation(
        *work.transaction, random_,
        InvalidationEvent{record->assignment.subject, revoked.assignment.version,
                          assignment_scope(record->assignment), false},
        work.now);
    if (const auto* error = std::get_if<Failure>(&invalidation)) {
      return *error;
    }
    const auto saved = work.transaction->exact_assignments().save_state(
        revoked, std::optional<ResourceVersion>{record->assignment.version});
    if (const auto* error = std::get_if<Failure>(&saved)) {
      return *error;
    }
    const auto audit = work.transaction->journal().append_audit(
        AuditEntry{authority.subject.principal.id,
                   {ResourceKind::Assignment, ResourceId{request.assignment_id.value}},
                   request.context.request_id,
                   request.context.trace_id,
                   std::string{kRevokeAssignmentMethod},
                   std::nullopt,
                   revoked.assignment.version,
                   work.now});
    if (const auto* error = std::get_if<Failure>(&audit)) {
      return *error;
    }
    const auto checkpoint = detail::final_check(work, request.context, TimePoint::max(), false);
    if (const auto* error = std::get_if<Failure>(&checkpoint)) {
      return *error;
    }
    const auto committed = detail::committed(*work.transaction);
    if (const auto* error = std::get_if<Failure>(&committed)) {
      return *error;
    }
    return RevokeAssignmentReply{revoked.assignment.version, work.now};
  }

  IdentitySecurityUnitOfWorkFactory& transactions_;
  IdentityAssignmentFingerprintPort& fingerprints_;
  RandomPort& random_;
  AssignmentSnapshots snapshots_;
};
} // namespace

Outcome<std::unique_ptr<IdentityAssignmentApplication>>
make_identity_assignment_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                     IdentityAssignmentFingerprintPort& fingerprints,
                                     RandomPort& random, IdentityAssignmentOptions options) {
  if (options.maximum_snapshots == 0 || options.maximum_snapshot_records == 0) {
    return failure(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<IdentityAssignmentApplication>{
        std::make_unique<Application>(transactions, fingerprints, random, options)};
  } catch (const std::bad_alloc&) {
    return failure(ErrorCode::UpstreamUnavailable);
  } catch (...) {
    return failure(ErrorCode::Internal);
  }
}
} // namespace kiarabot::core::target
