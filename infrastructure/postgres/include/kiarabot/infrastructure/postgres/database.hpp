#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::infrastructure::postgres {

// Infrastructure configuration only. Never log this object or a connection exception.
// No DSN is accepted: callers cannot override TLS, authentication or timeout policy.
struct DatabaseConfig {
  core::target::Service service;
  std::string host;
  std::uint16_t port = 5432;
  std::string database;
  std::string user;
  std::string password;
  std::string root_certificate;
  std::string migration_role;
  std::string runtime_role;
};

// Single-use explicit empty database initialization, with the migration identity.
// No database/role creation, old data import, destructive reset or automatic retry.
core::target::Status initialize_empty(const DatabaseConfig& config);

// Owns the sole writer session. Not thread safe. A lost session/lock permanently disables
// this instance; a new process must validate/acquire again. No transparent reconnection.
class Database final {
  struct Impl;

public:
  // Construction requires the private implementation; use open().
  explicit Database(std::unique_ptr<Impl> impl);
  static core::target::Outcome<std::unique_ptr<Database>> open(const DatabaseConfig& config);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&&) = delete;
  Database& operator=(Database&&) = delete;

  // Call before accepting work and each dispatch/lease boundary. Does not perform DDL.
  core::target::Status check_writer();

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::infrastructure::postgres
