#pragma once

#include "kiarabot/infrastructure/postgres/database.hpp"

namespace kiarabot::infrastructure::postgres::test {
void check_typed(const DatabaseConfig& config);
} // namespace kiarabot::infrastructure::postgres::test
