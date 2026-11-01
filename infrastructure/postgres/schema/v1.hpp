#pragma once

#include <string_view>

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr int kVersion = 1;
// Single source for the initial storage foundation. Business repositories are added only
// after their Core contracts are reviewed; this marker never claims business readiness.
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
