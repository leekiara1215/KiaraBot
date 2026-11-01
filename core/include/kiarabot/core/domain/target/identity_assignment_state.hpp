#pragma once

#include <optional>
#include <utility>

#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace kiarabot::core::target {

enum class AssignmentState { Active, Revoked };
// A revoked ID retains its owner, final version and exact former scope permanently.
// This is current administrative evidence, never an effective authorization grant.
struct AssignmentStateRecord {
  ExactAssignment assignment;
  AssignmentState state = AssignmentState::Active;
  std::optional<TimePoint> revoked_at;
};

inline Status validate_assignment_state(const AssignmentStateRecord& record) {
  if (std::holds_alternative<Failure>(validate_exact_assignment(record.assignment)) ||
      (record.state != AssignmentState::Active && record.state != AssignmentState::Revoked) ||
      (record.state == AssignmentState::Active && record.revoked_at) ||
      (record.state == AssignmentState::Revoked &&
       (!record.revoked_at || *record.revoked_at <= TimePoint{} ||
        *record.revoked_at == TimePoint::max()))) {
    return rejected(ErrorCode::Internal, Recovery::Diagnose);
  }
  return std::monostate{};
}

// Authentication and old/new scope management authority are checked by the use case first.
// In particular, a new idempotency key cannot recreate or update a revoked identifier.
inline Outcome<AssignmentStateRecord>
prepare_live_assignment(AssignmentId id, const ExactAssignmentSpec& spec, const Principal& owner,
                        const AssignmentStateRecord* previous,
                        std::optional<ResourceVersion> expected) {
  if (previous) {
    const auto valid = validate_assignment_state(*previous);
    if (const auto* error = std::get_if<Failure>(&valid)) {
      return *error;
    }
    if (previous->state == AssignmentState::Revoked) {
      return rejected(ErrorCode::Conflict, Recovery::ReadCurrent);
    }
  }
  auto prepared = prepare_assignment(std::move(id), spec, owner,
                                     previous ? &previous->assignment : nullptr, expected);
  if (const auto* error = std::get_if<Failure>(&prepared)) {
    return *error;
  }
  return AssignmentStateRecord{std::move(std::get<ExactAssignment>(prepared)),
                               AssignmentState::Active, std::nullopt};
}

inline Outcome<AssignmentStateRecord>
prepare_assignment_revocation(const AssignmentStateRecord& current, ResourceVersion expected,
                              TimePoint checked_at) {
  const auto valid = validate_assignment_state(current);
  if (const auto* error = std::get_if<Failure>(&valid)) {
    return *error;
  }
  if (checked_at <= TimePoint{} || checked_at == TimePoint::max()) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  if (current.state == AssignmentState::Revoked) {
    return rejected(ErrorCode::Conflict, Recovery::ReadCurrent);
  }
  const auto version = next_version(current.assignment.version, expected);
  if (const auto* error = std::get_if<Failure>(&version)) {
    return *error;
  }
  auto revoked = current;
  revoked.assignment.version = std::get<ResourceVersion>(version);
  revoked.state = AssignmentState::Revoked;
  revoked.revoked_at = checked_at;
  return revoked;
}

} // namespace kiarabot::core::target
