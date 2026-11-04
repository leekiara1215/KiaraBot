#pragma once

#include <filesystem>

#include "kiarabot/infrastructure/postgres/database.hpp"

namespace kiarabot::infrastructure::postgres::test {
void check_typed(const DatabaseConfig& config);
void check_runtime_management_receipts(const DatabaseConfig& runtime,
                                       const DatabaseConfig& migration,
                                       const std::filesystem::path& root, std::uint16_t proxy_port);
} // namespace kiarabot::infrastructure::postgres::test
