#pragma once

#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr int version(core::target::Service owner) {
  switch (owner) {
  case core::target::Service::Identity:
    return 13;
  case core::target::Service::Runtime:
    // P05 adds Runtime relations/columns, shared replay-result relations and the private
    // bridge_credentials relation; P06 adds the five event/delivery relations and A1's
    // binding-before-registration constraint.
    return 9;
  case core::target::Service::Scheduler:
    // P10a adds the one-shot task/occurrence trigger-observation columns and due/owner indexes.
    return 7;
  case core::target::Service::Model:
    return 6;
  }
  return 0;
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
