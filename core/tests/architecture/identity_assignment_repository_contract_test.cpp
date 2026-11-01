// clang-format off
#include "kiarabot/core/ports/target/identity_repository.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
// clang-format on

namespace {
namespace t = kiarabot::core::target;
static_assert(std::is_same_v<decltype(&t::IdentityExactAssignmentRepository::current),
                             t::Outcome<std::optional<t::AssignmentStateRecord>> (
                                 t::IdentityExactAssignmentRepository::*)(const t::AssignmentId&)>);
static_assert(
    std::is_same_v<decltype(&t::IdentityExactAssignmentRepository::save_state),
                   t::Status (t::IdentityExactAssignmentRepository::*)(
                       const t::AssignmentStateRecord&, std::optional<t::ResourceVersion>)>);
t::Failure fail(t::ErrorCode code) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}};
}
bool same_assignment(const t::ExactAssignment& left, const t::ExactAssignment& right) {
  return left.id == right.id && left.subject == right.subject && left.roles == right.roles &&
         left.explicit_permissions == right.explicit_permissions && left.version == right.version;
}
bool same_state(const t::AssignmentStateRecord& left, const t::AssignmentStateRecord& right) {
  return same_assignment(left.assignment, right.assignment) && left.state == right.state &&
         left.revoked_at == right.revoked_at;
}
// A contract test double, not a PostgreSQL implementation or persistence acceptance claim.
// Copy-on-write here makes the typed authority and its base projection one atomic test operation.
class MemoryAssignments final : public t::IdentityExactAssignmentRepository,
                                public t::Repository<t::AssignmentId, t::ResourceGrant> {
public:
  t::Outcome<std::optional<t::AssignmentStateRecord>> current(const t::AssignmentId& id) override {
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto found = records_.find(id.value);
    return found == records_.end() ? std::optional<t::AssignmentStateRecord>{}
                                   : std::optional{found->second};
  }
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& subject) override {
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::ExactAssignment> active;
    for (const auto& [id, row] : records_) {
      static_cast<void>(id);
      if (row.state == t::AssignmentState::Active && row.assignment.subject == subject) {
        active.push_back(row.assignment);
      }
    }
    return active;
  }
  t::Status save_state(const t::AssignmentStateRecord& row,
                       std::optional<t::ResourceVersion> expected) override {
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto valid = t::validate_assignment_state(row);
    if (const auto* error = std::get_if<t::Failure>(&valid)) {
      return *error;
    }
    const auto found = records_.find(row.assignment.id.value);
    if (found == records_.end()) {
      if (expected || row.assignment.version.value != 1 ||
          row.state != t::AssignmentState::Active) {
        return fail(t::ErrorCode::Conflict);
      }
    } else {
      const auto& previous = found->second;
      if (previous.state == t::AssignmentState::Revoked || !expected ||
          previous.assignment.version != *expected ||
          expected->value == std::numeric_limits<std::uint64_t>::max() ||
          row.assignment.version.value != expected->value + 1 ||
          row.assignment.subject != previous.assignment.subject) {
        return fail(t::ErrorCode::Conflict);
      }
      if (row.state == t::AssignmentState::Revoked &&
          (row.assignment.roles != previous.assignment.roles ||
           row.assignment.explicit_permissions != previous.assignment.explicit_permissions)) {
        return fail(t::ErrorCode::Conflict);
      }
    }
    if (row.state == t::AssignmentState::Revoked && row.revoked_at != database_now) {
      return fail(t::ErrorCode::InvalidArgument);
    }
    auto projection = t::to_resource_grant_metadata(row.assignment);
    if (const auto* error = std::get_if<t::Failure>(&projection)) {
      return *error;
    }
    auto records = records_;
    auto base = base_;
    records.insert_or_assign(row.assignment.id.value, row);
    if (fail_projection) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    if (row.state == t::AssignmentState::Active) {
      base.insert_or_assign(row.assignment.id.value, std::get<t::ResourceGrant>(projection));
    } else {
      base.erase(row.assignment.id.value);
    }
    records_.swap(records);
    base_.swap(base);
    return std::monostate{};
  }
  t::Status save(const t::ExactAssignment& row,
                 std::optional<t::ResourceVersion> expected) override {
    return save_state({row, t::AssignmentState::Active, {}}, expected);
  }
  t::Outcome<std::optional<t::ResourceGrant>> find(const t::AssignmentId& id) override {
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto found = base_.find(id.value);
    return found == base_.end() ? std::optional<t::ResourceGrant>{} : std::optional{found->second};
  }
  t::Status save(const t::ResourceGrant& row, std::optional<t::ResourceVersion> expected) override {
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto found = records_.find(row.id.value);
    if (found != records_.end() && found->second.state == t::AssignmentState::Revoked) {
      return fail(t::ErrorCode::Conflict);
    }
    // The legacy explicit projection delegates to the SAME authority, never an independent map.
    t::ExactAssignment exact{row.id, row.subject, {}, {}, row.version};
    for (const auto& scope : row.scopes) {
      if (row.role) {
        const t::ScopedRole role{*row.role, scope.resource};
        if (std::find(exact.roles.begin(), exact.roles.end(), role) == exact.roles.end()) {
          exact.roles.push_back(role);
        }
      } else {
        const auto permission = t::permission_from_name(scope.permission);
        if (const auto* error = std::get_if<t::Failure>(&permission)) {
          return *error;
        }
        exact.explicit_permissions.push_back({std::get<t::Permission>(permission), scope.resource});
      }
    }
    return save(exact, expected);
  }
  t::TimePoint database_now{std::chrono::seconds{1000}};
  bool unavailable = false, fail_projection = false;

private:
  std::map<std::string, t::AssignmentStateRecord> records_;
  std::map<std::string, t::ResourceGrant> base_;
};
struct Fixture {
  MemoryAssignments repository;
  t::Principal owner{{"11111111-1111-4111-8111-111111111111"}, t::PrincipalKind::Human, {}, {1}};
  t::AssignmentId id{"22222222-2222-4222-8222-222222222222"};
  t::ResourceRef first{t::ResourceKind::Model, {"33333333-3333-4333-8333-333333333333"}};
  t::ResourceRef second{t::ResourceKind::Model, {"44444444-4444-4444-8444-444444444444"}};
  t::ExactAssignmentSpec spec() const {
    return {owner.id, t::ExplicitAssignmentSpec{{t::Permission::ModelChat}, {first}}};
  }
  t::AssignmentStateRecord prepare() const {
    const auto proposed = spec();
    const auto validation = t::validate_assignment_spec(proposed, owner);
    assert(std::holds_alternative<std::monostate>(validation));
    const auto result = t::prepare_live_assignment(id, proposed, owner, nullptr, std::nullopt);
    assert(std::holds_alternative<t::AssignmentStateRecord>(result));
    return std::get<t::AssignmentStateRecord>(result);
  }
  t::AssignmentStateRecord current() {
    const auto result = repository.current(id);
    const auto& record = std::get<std::optional<t::AssignmentStateRecord>>(result);
    assert(record.has_value());
    return *record;
  }
};
void expect_error(const t::Status& status, t::ErrorCode code) {
  assert(std::get<t::Failure>(status).code == code);
}
void active_create_update_and_projection_share_one_authority() {
  Fixture f;
  auto first = f.prepare();
  assert(std::holds_alternative<std::monostate>(f.repository.save_state(first, {})));
  assert(f.current().assignment.version.value == 1);
  auto base = f.repository.find(f.id);
  const auto& projection = std::get<std::optional<t::ResourceGrant>>(base);
  assert(projection.has_value());
  assert(projection->subject == f.owner.id && projection->version.value == 1 &&
         projection->scopes.size() == 1);
  assert(projection->scopes[0].resource == f.first &&
         projection->scopes[0].permission == "model.chat");
  auto spec = f.spec();
  std::get<t::ExplicitAssignmentSpec>(spec.basis).scope = {f.second};
  const auto updated =
      t::prepare_live_assignment(f.id, spec, f.owner, &first, t::ResourceVersion{1});
  const auto& second = std::get<t::AssignmentStateRecord>(updated);
  assert(std::holds_alternative<std::monostate>(
      f.repository.save(second.assignment, t::ResourceVersion{1})));
  assert(f.current().assignment.version.value == 2);
  const auto active =
      std::get<std::vector<t::ExactAssignment>>(f.repository.for_subject(f.owner.id));
  assert(active.size() == 1 && active[0].explicit_permissions[0].resource == f.second);
  const auto other = f.repository.for_subject({"55555555-5555-4555-8555-555555555555"});
  assert(std::get<std::vector<t::ExactAssignment>>(other).empty());
}
void conflicts_never_mutate_owner_version_or_scope() {
  Fixture f;
  const auto first = f.prepare();
  assert(std::holds_alternative<std::monostate>(f.repository.save_state(first, {})));
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    auto candidate = first;
    candidate.assignment.version = {2};
    std::optional<t::ResourceVersion> expected{{1}};
    switch (mutation) {
    case 0:
      expected.reset();
      break;
    case 1:
      expected = t::ResourceVersion{9};
      break;
    case 2:
      candidate.assignment.subject = {"55555555-5555-4555-8555-555555555555"};
      break;
    case 3:
      candidate.assignment.version = {1};
      break;
    case 4:
      candidate.assignment.version = {3};
      break;
    default:
      throw std::logic_error("unexpected mutation");
    }
    expect_error(f.repository.save_state(candidate, expected), t::ErrorCode::Conflict);
    assert(same_state(f.current(), first));
  }
  auto absent = first;
  absent.assignment.id = {"66666666-6666-4666-8666-666666666666"};
  absent.assignment.version = {2};
  expect_error(f.repository.save_state(absent, {}), t::ErrorCode::Conflict);
  assert(
      !std::get<std::optional<t::AssignmentStateRecord>>(f.repository.current(absent.assignment.id))
           .has_value());
}
void revocation_preserves_tombstone_and_cannot_restore() {
  Fixture f;
  const auto live = f.prepare();
  assert(std::holds_alternative<std::monostate>(f.repository.save_state(live, {})));
  const auto result = t::prepare_assignment_revocation(live, {1}, f.repository.database_now);
  const auto revoked = std::get<t::AssignmentStateRecord>(result);
  assert(std::holds_alternative<std::monostate>(
      f.repository.save_state(revoked, t::ResourceVersion{1})));
  const auto evidence = f.current();
  assert(evidence.state == t::AssignmentState::Revoked && evidence.assignment.version.value == 2);
  assert(evidence.assignment.subject == live.assignment.subject &&
         evidence.assignment.explicit_permissions == live.assignment.explicit_permissions);
  assert(evidence.revoked_at == f.repository.database_now);
  assert(std::get<std::vector<t::ExactAssignment>>(f.repository.for_subject(f.owner.id)).empty());
  assert(!std::get<std::optional<t::ResourceGrant>>(f.repository.find(f.id)).has_value());
  const auto recreate =
      t::prepare_live_assignment(f.id, f.spec(), f.owner, &evidence, t::ResourceVersion{2});
  assert(std::get<t::Failure>(recreate).code == t::ErrorCode::Conflict);
  auto restore = live;
  restore.assignment.version = {3};
  expect_error(f.repository.save_state(restore, t::ResourceVersion{2}), t::ErrorCode::Conflict);
  expect_error(f.repository.save(restore.assignment, t::ResourceVersion{2}),
               t::ErrorCode::Conflict);
  auto legacy = std::get<t::ResourceGrant>(t::to_resource_grant_metadata(restore.assignment));
  expect_error(f.repository.save(legacy, t::ResourceVersion{2}), t::ErrorCode::Conflict);
  expect_error(f.repository.save(live.assignment, {}), t::ErrorCode::Conflict);
  auto re_revoked = revoked;
  re_revoked.assignment.version = {3};
  expect_error(f.repository.save_state(re_revoked, t::ResourceVersion{2}), t::ErrorCode::Conflict);
  assert(same_state(f.current(), evidence));
}
void revocation_requires_exact_old_scope_and_finite_database_time() {
  Fixture f;
  const auto live = f.prepare();
  assert(std::holds_alternative<std::monostate>(f.repository.save_state(live, {})));
  auto revoked = std::get<t::AssignmentStateRecord>(
      t::prepare_assignment_revocation(live, {1}, f.repository.database_now));
  auto substituted = revoked;
  substituted.assignment.explicit_permissions[0].resource = f.second;
  expect_error(f.repository.save_state(substituted, t::ResourceVersion{1}), t::ErrorCode::Conflict);
  substituted = revoked;
  substituted.assignment.explicit_permissions[0].permission = t::Permission::ModelRead;
  expect_error(f.repository.save_state(substituted, t::ResourceVersion{1}), t::ErrorCode::Conflict);
  for (const auto time : {t::TimePoint{}, t::TimePoint::max()}) {
    const auto invalid = t::prepare_assignment_revocation(live, {1}, time);
    assert(std::get<t::Failure>(invalid).code == t::ErrorCode::InvalidArgument);
  }
  revoked.revoked_at = f.repository.database_now + std::chrono::seconds{1};
  expect_error(f.repository.save_state(revoked, t::ResourceVersion{1}),
               t::ErrorCode::InvalidArgument);
  assert(same_state(f.current(), live));
}
void dependencies_and_projection_failure_never_look_absent_or_partial() {
  Fixture f;
  auto live = f.prepare();
  f.repository.fail_projection = true;
  expect_error(f.repository.save_state(live, {}), t::ErrorCode::UpstreamUnavailable);
  assert(
      !std::get<std::optional<t::AssignmentStateRecord>>(f.repository.current(f.id)).has_value());
  assert(!std::get<std::optional<t::ResourceGrant>>(f.repository.find(f.id)).has_value());
  f.repository.fail_projection = false;
  assert(std::holds_alternative<std::monostate>(f.repository.save_state(live, {})));
  auto revoked = std::get<t::AssignmentStateRecord>(
      t::prepare_assignment_revocation(live, {1}, f.repository.database_now));
  f.repository.fail_projection = true;
  expect_error(f.repository.save_state(revoked, t::ResourceVersion{1}),
               t::ErrorCode::UpstreamUnavailable);
  assert(same_state(f.current(), live));
  assert(std::get<std::optional<t::ResourceGrant>>(f.repository.find(f.id)).has_value());
  f.repository.unavailable = true;
  assert(std::get<t::Failure>(f.repository.current(f.id)).code ==
         t::ErrorCode::UpstreamUnavailable);
  assert(std::get<t::Failure>(f.repository.for_subject(f.owner.id)).code ==
         t::ErrorCode::UpstreamUnavailable);
  assert(std::get<t::Failure>(f.repository.find(f.id)).code == t::ErrorCode::UpstreamUnavailable);
  expect_error(f.repository.save_state(revoked, t::ResourceVersion{1}),
               t::ErrorCode::UpstreamUnavailable);
}
void competing_cas_has_one_winner_and_roles_keep_revoked_scope() {
  Fixture f;
  const auto first = f.prepare();
  assert(std::holds_alternative<std::monostate>(f.repository.save_state(first, {})));
  auto left_spec = f.spec();
  std::get<t::ExplicitAssignmentSpec>(left_spec.basis).scope = {f.second};
  auto right_spec = f.spec();
  std::get<t::ExplicitAssignmentSpec>(right_spec.basis).permissions = {t::Permission::ModelRead};
  const auto left = std::get<t::AssignmentStateRecord>(
      t::prepare_live_assignment(f.id, left_spec, f.owner, &first, t::ResourceVersion{1}));
  const auto right = std::get<t::AssignmentStateRecord>(
      t::prepare_live_assignment(f.id, right_spec, f.owner, &first, t::ResourceVersion{1}));
  assert(
      std::holds_alternative<std::monostate>(f.repository.save_state(left, t::ResourceVersion{1})));
  expect_error(f.repository.save_state(right, t::ResourceVersion{1}), t::ErrorCode::Conflict);
  assert(same_state(f.current(), left));
  Fixture roles;
  const t::ExactAssignmentSpec role_spec{
      roles.owner.id, t::RoleAssignmentSpec{t::FixedRole::Viewer, {roles.first, roles.second}}};
  const auto live = std::get<t::AssignmentStateRecord>(
      t::prepare_live_assignment(roles.id, role_spec, roles.owner, nullptr, {}));
  assert(std::holds_alternative<std::monostate>(roles.repository.save(live.assignment, {})));
  const auto projection =
      std::get<std::optional<t::ResourceGrant>>(roles.repository.find(roles.id));
  assert(projection.has_value() && projection->role == t::FixedRole::Viewer);
  const auto revoked = std::get<t::AssignmentStateRecord>(
      t::prepare_assignment_revocation(live, {1}, roles.repository.database_now));
  auto changed_role = revoked;
  for (auto& role : changed_role.assignment.roles) {
    role.role = t::FixedRole::Admin;
  }
  expect_error(roles.repository.save_state(changed_role, t::ResourceVersion{1}),
               t::ErrorCode::Conflict);
  assert(same_state(roles.current(), live));
  assert(std::holds_alternative<std::monostate>(
      roles.repository.save_state(revoked, t::ResourceVersion{1})));
  assert(roles.current().assignment.roles == live.assignment.roles);
  assert(std::get<std::vector<t::ExactAssignment>>(roles.repository.for_subject(roles.owner.id))
             .empty());
  assert(!std::get<std::optional<t::ResourceGrant>>(roles.repository.find(roles.id)).has_value());
  auto forged = revoked;
  forged.assignment.roles[0].resource = {t::ResourceKind::Model,
                                         {"77777777-7777-4777-8777-777777777777"}};
  ++forged.assignment.version.value;
  expect_error(roles.repository.save_state(forged, t::ResourceVersion{2}), t::ErrorCode::Conflict);
  assert(same_state(roles.current(), revoked));
}
void invalid_create_or_state_never_creates_evidence() {
  Fixture f;
  auto revoked = f.prepare();
  revoked.state = t::AssignmentState::Revoked;
  revoked.revoked_at = f.repository.database_now;
  expect_error(f.repository.save_state(revoked, {}), t::ErrorCode::Conflict);
  auto invalid_active = f.prepare();
  invalid_active.revoked_at = f.repository.database_now;
  expect_error(f.repository.save_state(invalid_active, {}), t::ErrorCode::Internal);
  assert(
      !std::get<std::optional<t::AssignmentStateRecord>>(f.repository.current(f.id)).has_value());
  auto overflow = f.prepare();
  overflow.assignment.version.value = std::numeric_limits<std::uint64_t>::max();
  const auto refused = t::prepare_assignment_revocation(overflow, overflow.assignment.version,
                                                        f.repository.database_now);
  assert(std::get<t::Failure>(refused).code == t::ErrorCode::Internal);
}

} // namespace
int main() try {
  active_create_update_and_projection_share_one_authority();
  conflicts_never_mutate_owner_version_or_scope();
  revocation_preserves_tombstone_and_cannot_restore();
  revocation_requires_exact_old_scope_and_finite_database_time();
  dependencies_and_projection_failure_never_look_absent_or_partial();
  competing_cas_has_one_winner_and_roles_keep_revoked_scope();
  invalid_create_or_state_never_creates_evidence();
} catch (...) {
  return 1;
}
