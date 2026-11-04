#pragma once

#include <string>
#include <string_view>

#include "identity_security.hpp"
#include "kiarabot/core/domain/target/foundation.hpp"
#include "operation_observations.hpp"
#include "runtime_actions.hpp"
#include "runtime_bootstrap_deliveries.hpp"
#include "runtime_events.hpp"
#include "runtime_host_admission_gates.hpp"
#include "runtime_host_commands.hpp"
#include "runtime_host_connections.hpp"
#include "runtime_management_receipts.hpp"
#include "runtime_storage.hpp"

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
    sql += kRuntimeManagementReceipts;
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
  revoked boolean NOT NULL DEFAULT false,
  -- BOOT-02 persisted monotonic version; 0 is not storable so the bridge gate always has a real
  -- current version. A reissue revokes the old row and inserts version+1 in the same transaction.
  version kiarabot.uint64 NOT NULL CHECK(version>0)
);
CREATE UNIQUE INDEX bridge_credentials_active ON kiarabot.bridge_credentials(resource_id)
  WHERE NOT revoked;
)sql";
    sql += R"sql(
-- P13a reversible-delete tombstone. The resource relation keeps the stable id/state; this row
-- records the operation identity and the restore/purge deadlines. The generic operations row cannot
-- hold repeated participants, so participant progress lives in kiarabot.operation_participants.
CREATE TABLE kiarabot.lifecycle_tombstones (
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL CHECK(resource_id<>''),
  operation_id text NOT NULL CHECK(operation_id<>''),
  state integer NOT NULL CHECK(state BETWEEN 0 AND 5),
  deleted_at bigint NOT NULL,
  restore_until bigint NOT NULL CHECK(restore_until>deleted_at),
  purge_after bigint NOT NULL CHECK(purge_after>restore_until),
  PRIMARY KEY(resource_kind,resource_id)
);
CREATE INDEX lifecycle_tombstones_purge ON kiarabot.lifecycle_tombstones(state,purge_after);
)sql";
    sql += kRuntimeEvents;
    sql += kRuntimeActions;
    sql += kRuntimeHostConnections;
    sql += kRuntimeHostAdmissionGates;
    sql += kRuntimeHostCommands;
    sql += kRuntimeStorage;
    sql += kRuntimeBootstrapDeliveries;
    break;
  case core::target::Service::Scheduler:
    relation("tasks", "owner,task_id");
    relation("occurrences", "occurrence_id");
    sql += R"sql(
ALTER TABLE kiarabot.tasks ADD UNIQUE(occurrence_id);
ALTER TABLE kiarabot.tasks ADD UNIQUE(owner,task_id,installation,occurrence_id);
CREATE INDEX tasks_due ON kiarabot.tasks(state,run_at);
CREATE INDEX tasks_owner ON kiarabot.tasks(owner,state);
CREATE INDEX occurrences_owner ON kiarabot.occurrences(owner);
CREATE INDEX occurrences_inflight ON kiarabot.occurrences(state,due_at);
CREATE TABLE kiarabot.scheduler_quota_counters (
  owner text PRIMARY KEY CHECK(owner<>''),
  instance_active integer NOT NULL CHECK(instance_active BETWEEN 0 AND 1024),
  installation_active integer NOT NULL CHECK(installation_active BETWEEN 0 AND 8192),
  updated_at bigint NOT NULL
);
ALTER TABLE kiarabot.occurrences ADD UNIQUE(owner,task_id);
ALTER TABLE kiarabot.occurrences ADD FOREIGN KEY(owner,task_id,installation,occurrence_id) REFERENCES kiarabot.tasks(owner,task_id,installation,occurrence_id);
)sql";
    break;
  case core::target::Service::Model:
    // P03 declaration-level intent relation extended in place by P11; see common.hpp.
    relation("model_intents", "owner,logical_call_id");
    sql += R"sql(
-- P11 Model-private configuration. Configuration version is the CAS/watermark; the provider
-- secret version lives in a separate relation so a rotation never rewrites this row.
CREATE TABLE kiarabot.model_configs (
  model_id text PRIMARY KEY CHECK(model_id<>''),
  alias text NOT NULL CHECK(alias<>'' AND octet_length(alias)<=128),
  provider_kind text NOT NULL CHECK(provider_kind='openai_chat_completions'),
  endpoint text NOT NULL CHECK(endpoint<>'' AND octet_length(endpoint)<=2048),
  provider_model text NOT NULL CHECK(provider_model<>'' AND octet_length(provider_model)<=256),
  timeout_ms integer NOT NULL CHECK(timeout_ms BETWEEN 1000 AND 30000),
  max_concurrency integer NOT NULL CHECK(max_concurrency BETWEEN 1 AND 64),
  lifecycle_state integer NOT NULL CHECK(lifecycle_state BETWEEN 0 AND 5),
  config_version kiarabot.uint64 NOT NULL CHECK(config_version>0),
  created_at bigint NOT NULL,
  observed_at bigint NOT NULL
);
ALTER TABLE kiarabot.model_configs ADD UNIQUE(alias);
-- Provider credential versions: XChaCha20-Poly1305 sealed with a deployment-injected root key.
-- Root key material is never stored in this database; old versions are retained for in-flight calls.
CREATE TABLE kiarabot.encrypted_provider_secrets (
  model_id text NOT NULL REFERENCES kiarabot.model_configs(model_id),
  secret_version kiarabot.uint64 NOT NULL CHECK(secret_version>0),
  root_key_version kiarabot.uint64 NOT NULL CHECK(root_key_version>0),
  aead integer NOT NULL CHECK(aead=1),
  nonce bytea NOT NULL CHECK(octet_length(nonce)=24),
  sealed_secret bytea NOT NULL CHECK(octet_length(sealed_secret) BETWEEN 17 AND 8192),
  created_at bigint NOT NULL,
  PRIMARY KEY(model_id,secret_version)
);
)sql";
    break;
  default:
    break;
  }
  if (owner == core::target::Service::Runtime || owner == core::target::Service::Scheduler ||
      owner == core::target::Service::Model) {
    sql += kOperationObservations;
  }
  return sql;
}
} // namespace kiarabot::infrastructure::postgres::schema
