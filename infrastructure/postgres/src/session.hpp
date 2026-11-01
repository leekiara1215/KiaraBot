#pragma once

#include <memory>
#include <semaphore>

#include <pqxx/pqxx>

#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::infrastructure::postgres::detail {

inline constexpr int kWriterLockClass = 1262638164;
inline constexpr int kWriterLockId = 1;
struct Session {
  std::unique_ptr<pqxx::connection> connection;
  core::target::Service owner;
  bool disabled = false;
  std::binary_semaphore permit{1};
};
core::target::Outcome<std::unique_ptr<core::target::UnitOfWork>>
begin_common(const std::shared_ptr<Session>& session);

} // namespace kiarabot::infrastructure::postgres::detail
