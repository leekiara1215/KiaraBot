#pragma once

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "identity_security.hpp"
#include "kiarabot/core/domain/target/persistence_rules.hpp"
#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail::identity {
namespace target = core::target;

inline target::Failure invalid() {
  return target::rejected(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
}
inline target::Failure conflict() {
  return target::rejected(target::ErrorCode::Conflict, target::Recovery::ReadCurrent);
}
// A typed result is captured only inside CommonWork::invoke, so SQL/decode exceptions and
// explicit validation failures poison the same transaction as inherited repositories.
template <typename Value, typename Function>
target::Outcome<Value> query(SqlWork& sql, Function function) {
  std::optional<Value> value;
  auto status = sql.run([&](pqxx::transaction_base& transaction) -> target::Status {
    auto result = function(transaction);
    if (const auto* failure = std::get_if<target::Failure>(&result)) {
      return *failure;
    }
    value.emplace(std::move(std::get<Value>(result)));
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<target::Failure>(&status)) {
    return *failure;
  }
  return std::move(value.value());
}
inline bool valid_id(std::string_view id) {
  return !id.empty() && valid_sql_text(id);
}
inline bool valid_next_version(target::ResourceVersion version,
                               std::optional<target::ResourceVersion> expected) {
  return version.value > 0 &&
         (!expected || (expected->value < std::numeric_limits<std::uint64_t>::max() &&
                        version.value == expected->value + 1));
}
inline void require_stored(bool condition) {
  if (!condition) {
    throw std::runtime_error("invalid stored Identity security relation");
  }
}
} // namespace kiarabot::infrastructure::postgres::detail::identity
