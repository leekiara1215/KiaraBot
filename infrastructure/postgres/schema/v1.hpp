#pragma once

#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr int version(core::target::Service owner) {
  return owner == core::target::Service::Identity ? 10 : 5;
}
// Explicit empty-database schema version; no upgrade or reset of existing databases.
inline constexpr std::string_view kCreate = R"sql(
CREATE SCHEMA kiarabot;
REVOKE ALL ON SCHEMA kiarabot FROM PUBLIC;
CREATE TABLE kiarabot.schema_versions (
  singleton boolean PRIMARY KEY CHECK (singleton),
  version integer NOT NULL CHECK (version > 0),
  service text NOT NULL CHECK (service IN ('identity','runtime','scheduler','model'))
);
)sql";

} // namespace kiarabot::infrastructure::postgres::schema
