#pragma once

#include <optional>
#include <span>
#include <string>

#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// Closed infrastructure dispatch to concrete relation tables. Not a Core port or a blob store.
enum class RecordKind {
  Principal = 3,
  Session = 4,
  Grant = 5,
  Account = 6,
  Installation = 7,
  Instance = 8,
  Task = 9,
  Occurrence = 10,
  ModelIntent = 11
};
core::target::Outcome<std::optional<core::target::ReplayResult>>
find_record(core::target::UnitOfWork& work, RecordKind kind, std::span<const std::string> key);
core::target::Status save_record(core::target::UnitOfWork& work, RecordKind kind,
                                 const core::target::ReplayResult& value,
                                 std::optional<core::target::ResourceVersion> expected);
core::target::Outcome<std::optional<core::target::AuthorizationState>>
find_authorization(core::target::UnitOfWork& work, const core::target::SubjectId& subject);
core::target::Status save_authorization(core::target::UnitOfWork& work,
                                        const core::target::AuthorizationState& state,
                                        std::optional<core::target::ResourceVersion> expected);
} // namespace kiarabot::infrastructure::postgres::detail
