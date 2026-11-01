#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>

#include "kiarabot/core/domain/target/identity_assignment_state.hpp"

namespace t = kiarabot::core::target;
using std::chrono_literals::operator""s;

void require(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("assignment state assertion failed at line " +
                             std::to_string(location.line()));
  }
}
template <typename Value> bool failed(const t::Outcome<Value>& outcome, t::ErrorCode code) {
  const auto* error = std::get_if<t::Failure>(&outcome);
  return error && error->code == code;
}

int main() {
  const t::ResourceRef first{t::ResourceKind::Account, {"account-one"}};
  const t::ResourceRef second{t::ResourceKind::Account, {"account-two"}};
  const t::Principal owner{{"subject-one"}, t::PrincipalKind::Human, std::nullopt, {1}};
  const t::ExactAssignmentSpec spec{owner.id,
                                    t::RoleAssignmentSpec{t::FixedRole::Operator, {first}}};
  auto prepared =
      t::prepare_live_assignment({"assignment-one"}, spec, owner, nullptr, std::nullopt);
  const auto active = std::get<t::AssignmentStateRecord>(prepared);
  require(active.state == t::AssignmentState::Active && !active.revoked_at &&
          active.assignment.version.value == 1);

  auto revoked_result = t::prepare_assignment_revocation(active, {1}, t::TimePoint{100s});
  const auto revoked = std::get<t::AssignmentStateRecord>(revoked_result);
  require(revoked.state == t::AssignmentState::Revoked &&
          revoked.revoked_at == t::TimePoint{100s} && revoked.assignment.version.value == 2 &&
          revoked.assignment.id == active.assignment.id && revoked.assignment.subject == owner.id &&
          revoked.assignment.roles == active.assignment.roles &&
          revoked.assignment.explicit_permissions.empty());
  require(active.state == t::AssignmentState::Active && active.assignment.version.value == 1);

  // New-key repeats and conditional edits cannot resurrect a tombstone or steal its ID.
  auto proposed = spec;
  std::get<t::RoleAssignmentSpec>(proposed.basis).scope = {second};
  require(failed(t::prepare_live_assignment(active.assignment.id, proposed, owner, &revoked,
                                            t::ResourceVersion{2}),
                 t::ErrorCode::Conflict));
  require(failed(
      t::prepare_live_assignment(active.assignment.id, proposed, owner, &revoked, std::nullopt),
      t::ErrorCode::Conflict));
  require(failed(t::prepare_assignment_revocation(revoked, {2}, t::TimePoint{101s}),
                 t::ErrorCode::Conflict));
  require(failed(t::prepare_assignment_revocation(active, {2}, t::TimePoint{100s}),
                 t::ErrorCode::Conflict));

  const auto changed = t::prepare_live_assignment(active.assignment.id, proposed, owner, &active,
                                                  t::ResourceVersion{1});
  const auto& updated = std::get<t::AssignmentStateRecord>(changed);
  require(updated.assignment.version.value == 2 &&
          updated.assignment.roles.front().resource == second);
  require(failed(t::prepare_live_assignment(active.assignment.id, proposed, owner, nullptr,
                                            t::ResourceVersion{1}),
                 t::ErrorCode::NotFound));
  auto different_owner = owner;
  different_owner.id = {"subject-two"};
  proposed.subject = different_owner.id;
  require(failed(t::prepare_live_assignment(active.assignment.id, proposed, different_owner,
                                            &active, t::ResourceVersion{1}),
                 t::ErrorCode::Conflict));

  auto maximum = active;
  maximum.assignment.version = {std::numeric_limits<std::uint64_t>::max()};
  require(failed(
      t::prepare_assignment_revocation(maximum, maximum.assignment.version, t::TimePoint{100s}),
      t::ErrorCode::Internal));
  require(failed(t::prepare_assignment_revocation(active, {1}, t::TimePoint::max()),
                 t::ErrorCode::InvalidArgument));
  require(failed(t::prepare_assignment_revocation(active, {1}, t::TimePoint{}),
                 t::ErrorCode::InvalidArgument));

  // Corrupted state fails closed before it can become an active authorization projection.
  auto damaged = revoked;
  damaged.revoked_at.reset();
  require(failed(t::validate_assignment_state(damaged), t::ErrorCode::Internal));
  require(failed(t::prepare_assignment_revocation(damaged, {2}, t::TimePoint{101s}),
                 t::ErrorCode::Internal));
  damaged = active;
  damaged.revoked_at = t::TimePoint{100s};
  require(failed(t::validate_assignment_state(damaged), t::ErrorCode::Internal));
  damaged = revoked;
  damaged.assignment.roles.clear();
  require(failed(t::validate_assignment_state(damaged), t::ErrorCode::Internal));
}
