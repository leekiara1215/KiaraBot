#pragma once

#include <cstdint>
#include <filesystem>

#include "kiarabot/infrastructure/postgres/database.hpp"

namespace kiarabot::infrastructure::postgres::test {
void check_common(Database& database, const DatabaseConfig& config);
void check_dependency_fault(const DatabaseConfig& migration, const DatabaseConfig& runtime);
void check_deadline(const DatabaseConfig& config);
void check_unknown_commit(const DatabaseConfig& config, const std::filesystem::path& root,
                          std::uint16_t proxy_port);
} // namespace kiarabot::infrastructure::postgres::test
