#pragma once

#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr int version(core::target::Service owner) {
  switch (owner) {
  case core::target::Service::Identity:
    return 14;
  case core::target::Service::Runtime:
    // P05 adds Runtime relations/columns, shared replay-result relations and the private
    // bridge_credentials relation; P06 adds the five event/delivery relations and A1's
    // binding-before-registration constraint; P07 adds the six Action/quota/policy/evidence
    // relations and the shared audit target columns; P09 adds the four namespace/storage
    // relations plus the typed namespace receipt payload relation; P08b adds the authoritative
    // host_connections projection (connection generation/epoch/liveness/desired config version)
    // and P08b-3 host_commands outbox + host_observations inbox; P13a adds operation
    // retention/participants, audit/outbox cleanup indexes and the lifecycle tombstone relation.
    return 14;
  case core::target::Service::Scheduler:
    // P10a adds task/occurrence observation columns, the quota counter relation and due/owner
    // indexes; P07 adds the shared nullable audit target columns to every service schema. The
    // merged schema keeps version 9 (both prior increments were measured at Scheduler v8).
    return 9;
  case core::target::Service::Model:
    // P11a adds model_configs + encrypted_provider_secrets and extends model_intents with the
    // durable call facts (disposition/UNKNOWN resolution/usage); P07 adds the shared nullable
    // audit target columns to every service schema.
    return 8;
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
