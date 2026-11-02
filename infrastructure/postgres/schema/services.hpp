#pragma once

#include <string>
#include <string_view>

#include "identity_security.hpp"
#include "kiarabot/core/domain/target/foundation.hpp"
#include "runtime_events.hpp"

namespace kiarabot::infrastructure::postgres::schema {

inline std::string service_sql(core::target::Service owner) {
  std::string sql;
  const auto relation = [&sql](std::string_view table, std::string_view keys) {
    // Same typed column definitions as immutable results, distinct authoritative relations.
    // LIKE does not copy FK/indexes; their owner-local constraints are declared below.
    sql += "CREATE TABLE kiarabot." + std::string(table) + " (LIKE kiarabot.result_" +
           std::string(table) + " INCLUDING CONSTRAINTS);";
    sql += "ALTER TABLE kiarabot." + std::string(table) + " DROP COLUMN result_id;";
    sql += "ALTER TABLE kiarabot." + std::string(table) + " ADD PRIMARY KEY(" + std::string(keys) +
           ");";
  };
  switch (owner) {
  case core::target::Service::Identity:
    relation("principals", "id");
    relation("sessions", "id");
    relation("grants", "id");
    sql += R"sql(
ALTER TABLE kiarabot.sessions ADD FOREIGN KEY(subject) REFERENCES kiarabot.principals(id);
ALTER TABLE kiarabot.grants ADD FOREIGN KEY(subject) REFERENCES kiarabot.principals(id);
CREATE TABLE kiarabot.grant_scopes (
  grant_id text NOT NULL REFERENCES kiarabot.grants(id) ON DELETE CASCADE,
  position integer NOT NULL CHECK(position>=0), resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL, permission text NOT NULL, PRIMARY KEY(grant_id,position)
);
CREATE TABLE kiarabot.authorizations (
  subject text PRIMARY KEY REFERENCES kiarabot.principals(id), version kiarabot.uint64 NOT NULL,
  invalidation_dirty boolean NOT NULL
);
-- P04-A v1 Identity rule: one Principal per exact resource binding, independent of request
-- idempotency keys. Principals without a binding are not part of this uniqueness domain.
CREATE UNIQUE INDEX principals_binding_unique ON kiarabot.principals(binding_kind,binding_id)
  WHERE binding_kind IS NOT NULL AND binding_id IS NOT NULL;
)sql";
    sql += kIdentitySecurity;
    break;
  case core::target::Service::Runtime:
    relation("accounts", "id");
    relation("installations", "id");
    relation("instances", "id");
    relation("adapters", "id");
    relation("adapter_bindings", "account");
    relation("hosts", "id");
    relation("instance_configs", "instance");
    sql += R"sql(
ALTER TABLE kiarabot.accounts ADD UNIQUE(platform,platform_account_id);
ALTER TABLE kiarabot.installations ADD UNIQUE(id,plugin_type);
ALTER TABLE kiarabot.instances ADD FOREIGN KEY(installation,plugin_type) REFERENCES kiarabot.installations(id,plugin_type);
ALTER TABLE kiarabot.instances ADD CHECK(fence_instance IS NULL OR fence_instance=id);
ALTER TABLE kiarabot.adapters ADD UNIQUE(instance);
ALTER TABLE kiarabot.adapter_bindings ADD FOREIGN KEY(account) REFERENCES kiarabot.accounts(id);
-- A1: the external self-managed adapter binding is authoritative before its first bridge
-- registration creates the adapters row, so adapter_instance is a stable external identity rather
-- than a FK to a pre-existing registration. The registration handler verifies the binding owns the
-- bootstrap identity and that every declared account binding references this instance.
ALTER TABLE kiarabot.adapter_bindings ADD CHECK(btrim(adapter_instance)<>'');
ALTER TABLE kiarabot.instance_configs ADD FOREIGN KEY(instance) REFERENCES kiarabot.instances(id);
CREATE TABLE kiarabot.bridge_credentials (
  id text PRIMARY KEY CHECK(id<>''),
  resource_id text NOT NULL CHECK(resource_id<>''),
  digest bytea NOT NULL CHECK(octet_length(digest)=32),
  created_at bigint NOT NULL,
  revoked boolean NOT NULL DEFAULT false
);
CREATE UNIQUE INDEX bridge_credentials_active ON kiarabot.bridge_credentials(resource_id)
  WHERE NOT revoked;
)sql";
    sql += kRuntimeEvents;
    break;
  case core::target::Service::Scheduler:
    relation("tasks", "owner,task_id");
    relation("occurrences", "occurrence_id");
    sql += R"sql(
ALTER TABLE kiarabot.tasks ADD UNIQUE(occurrence_id);
ALTER TABLE kiarabot.tasks ADD UNIQUE(owner,task_id,installation,occurrence_id);
ALTER TABLE kiarabot.occurrences ADD UNIQUE(owner,task_id);
ALTER TABLE kiarabot.occurrences ADD FOREIGN KEY(owner,task_id,installation,occurrence_id) REFERENCES kiarabot.tasks(owner,task_id,installation,occurrence_id);
)sql";
    break;
  case core::target::Service::Model:
    relation("model_intents", "owner,logical_call_id");
    break;
  default:
    break;
  }
  return sql;
}
} // namespace kiarabot::infrastructure::postgres::schema
