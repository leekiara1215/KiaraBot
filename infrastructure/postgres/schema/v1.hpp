#pragma once

#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr int version(core::target::Service owner) {
  switch (owner) {
  case core::target::Service::Identity:
    // P14h aligns exact_permissions/result_exact_permissions CHECK with wire Permission 1..50.
    // v1.2 adds the owner-local observation feed journal (logs family) and the shared
    // model_intents error CHECK correction (InvalidArgument=0); shared result snapshot.
    return 17;
  case core::target::Service::Runtime:
    // P05 adds Runtime relations/columns, shared replay-result relations and the private
    // bridge_credentials relation; P06 adds the five event/delivery relations and A1's
    // binding-before-registration constraint; P07 adds the six Action/quota/policy/evidence
    // relations and the shared audit target columns; P09 adds the four namespace/storage
    // relations plus the typed namespace receipt payload relation; P08b adds the authoritative
    // host_connections projection (connection generation/epoch/liveness/desired config version)
    // and P08b-3 host_commands outbox + host_observations inbox (P08b-3a adds the
    // installation snapshot column; P08b-3b fixes the command-kind CHECK range); P13a adds
    // operation retention/participants, audit/outbox cleanup indexes and the lifecycle tombstone
    // relation; P14h aligns the shared result_exact_permissions CHECK with wire Permission 1..50.
    // WS-02 owner-local operation observation journal; BOOT-02 adds the Runtime bootstrap
    // delivery intent/receipt relation and the persisted bridge credential version; HOST-03 adds
    // the Runtime-owned Host admission gate row; v1.2 adds the owner-local observation feed
    // journal (logs + messages families) and the shared model_intents error CHECK correction.
    // Empty-database schema only, no migration. Identity remains unchanged.
    return 23;
  case core::target::Service::Scheduler:
    // P10a adds task/occurrence observation columns, the quota counter relation and due/owner
    // indexes; P07 adds the shared nullable audit target columns to every service schema. The
    // merged schema keeps version 9 (both prior increments were measured at Scheduler v8);
    // P14h aligns the shared result_exact_permissions CHECK with wire Permission 1..50.
    // WS-02 owner-local operation observation journal; v1.2 adds the owner-local observation
    // feed journal (tasks family) and the shared model_intents error CHECK correction.
    return 13;
  case core::target::Service::Model:
    // P11a adds model_configs + encrypted_provider_secrets and extends model_intents with the
    // durable call facts (disposition/UNKNOWN resolution/usage); P07 adds the shared nullable
    // audit target columns to every service schema; P14h aligns the shared
    // result_exact_permissions CHECK with wire Permission 1..50.
    // WS-02 owner-local operation observation journal; v1.2 fixes the model_intents error CHECK to
    // allow the domain InvalidArgument encoding (0), matching the other shared result snapshots.
    return 11;
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
