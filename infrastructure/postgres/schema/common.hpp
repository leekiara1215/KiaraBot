#pragma once

#include <string>
#include <string_view>

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr std::string_view kOperationColumns = R"sql(
  id text NOT NULL, owner integer NOT NULL CHECK(owner BETWEEN 0 AND 5),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL,
  version kiarabot.uint64 NOT NULL, state integer NOT NULL CHECK(state BETWEEN 0 AND 6),
  execution_outcome integer NOT NULL CHECK(execution_outcome BETWEEN 0 AND 3),
  blocked_reason integer CHECK(blocked_reason BETWEEN 0 AND 10), next_attempt bigint
)sql";
inline constexpr std::string_view kHandoffColumns = R"sql(
  source integer NOT NULL CHECK(source BETWEEN 0 AND 5), message text NOT NULL,
  operation_id text NOT NULL, operation_owner integer NOT NULL CHECK(operation_owner BETWEEN 0 AND 5),
  operation_resource_kind integer NOT NULL CHECK(operation_resource_kind BETWEEN 0 AND 15),
  operation_resource_id text NOT NULL, causation text,
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL,
  resource_version kiarabot.uint64 NOT NULL, actor text NOT NULL,
  authorization_version kiarabot.uint64 NOT NULL, payload_hash bytea NOT NULL CHECK(octet_length(payload_hash)=32),
  created_at bigint NOT NULL, valid_until bigint NOT NULL CHECK(valid_until>created_at)
)sql";

inline std::string common_sql() {
  std::string sql = R"sql(
CREATE DOMAIN kiarabot.uint64 AS numeric(20,0)
  CHECK(VALUE>=0 AND VALUE<=18446744073709551615);
CREATE TABLE kiarabot.replay_results (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  kind integer NOT NULL CHECK(kind BETWEEN 0 AND 12)
);
CREATE TABLE kiarabot.result_mutations (
  result_id bigint PRIMARY KEY REFERENCES kiarabot.replay_results(id) ON DELETE CASCADE,
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL, version kiarabot.uint64 NOT NULL
);
)sql";
  const auto snapshot = [&sql](std::string_view name, std::string_view columns) {
    sql +=
        "CREATE TABLE kiarabot.result_" + std::string(name) +
        " (result_id bigint PRIMARY KEY REFERENCES kiarabot.replay_results(id) ON DELETE CASCADE," +
        std::string(columns) + ");";
  };
  snapshot("operations", kOperationColumns);
  snapshot("principals", R"sql(
    id text NOT NULL, kind integer NOT NULL CHECK(kind BETWEEN 0 AND 3),
    binding_kind integer CHECK(binding_kind BETWEEN 0 AND 15), binding_id text,
    version kiarabot.uint64 NOT NULL, state integer NOT NULL CHECK(state BETWEEN 0 AND 5),
    CHECK((binding_kind IS NULL)=(binding_id IS NULL))
  )sql");
  snapshot("sessions", R"sql(
    id text NOT NULL, subject text NOT NULL, version kiarabot.uint64 NOT NULL,
    rotation kiarabot.uint64 NOT NULL, expires_at bigint NOT NULL, revoked boolean NOT NULL
  )sql");
  snapshot("session_metadata", R"sql(
    id text NOT NULL, subject text NOT NULL, created_at bigint NOT NULL, expires_at bigint NOT NULL,
    rotation kiarabot.uint64 NOT NULL, version kiarabot.uint64 NOT NULL, revoked boolean NOT NULL
  )sql");
  snapshot("grants", R"sql(
    id text NOT NULL, subject text NOT NULL, role integer CHECK(role BETWEEN 0 AND 2),
    version kiarabot.uint64 NOT NULL
  )sql");
  sql += R"sql(
CREATE TABLE kiarabot.result_grant_scopes (
  result_id bigint NOT NULL REFERENCES kiarabot.result_grants(result_id) ON DELETE CASCADE,
  position integer NOT NULL CHECK(position>=0), resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL, permission text NOT NULL, PRIMARY KEY(result_id,position)
);
)sql";
  snapshot("accounts", R"sql(
    id text NOT NULL, platform text NOT NULL, platform_account_id text NOT NULL,
    display_name text NOT NULL, version kiarabot.uint64 NOT NULL,
    state integer NOT NULL CHECK(state BETWEEN 0 AND 5)
  )sql");
  snapshot("installations", R"sql(
    id text NOT NULL, plugin_type text NOT NULL, version kiarabot.uint64 NOT NULL,
    state integer NOT NULL CHECK(state BETWEEN 0 AND 5)
  )sql");
  snapshot("instances", R"sql(
    id text NOT NULL, installation text NOT NULL, plugin_type text NOT NULL,
    desired_version kiarabot.uint64 NOT NULL, applied_config_version kiarabot.uint64 NOT NULL,
    fence_instance text, runtime_epoch text, execution_id text, execution_generation kiarabot.uint64,
    connection_id text, connection_generation kiarabot.uint64,
    version kiarabot.uint64 NOT NULL, state integer NOT NULL CHECK(state BETWEEN 0 AND 5), host text,
    CHECK((fence_instance IS NULL AND runtime_epoch IS NULL AND execution_id IS NULL AND
      execution_generation IS NULL AND connection_id IS NULL AND connection_generation IS NULL) OR
      (fence_instance IS NOT NULL AND runtime_epoch IS NOT NULL AND execution_id IS NOT NULL AND
      execution_generation IS NOT NULL AND connection_id IS NOT NULL AND connection_generation IS NOT NULL))
  )sql");
  snapshot("tasks", R"sql(
    owner text NOT NULL, task_id text NOT NULL, installation text NOT NULL,
    adapter text NOT NULL, account text NOT NULL, conversation_id text NOT NULL,
    conversation_kind integer NOT NULL CHECK(conversation_kind BETWEEN 0 AND 2),
    payload bytea NOT NULL CHECK(octet_length(payload)<=65536), run_at bigint NOT NULL,
    state integer NOT NULL CHECK(state BETWEEN 0 AND 5), version kiarabot.uint64 NOT NULL,
    occurrence_id text NOT NULL
  )sql");
  snapshot("occurrences",
           "owner text NOT NULL, installation text NOT NULL, task_id text NOT NULL,"
           "task_version kiarabot.uint64 NOT NULL, occurrence_id text NOT NULL, due_at bigint "
           "NOT NULL,"
           "version kiarabot.uint64 NOT NULL,state integer NOT NULL CHECK(state BETWEEN 0 AND 6),"
           "accepted_event_id text, acknowledged_at bigint," +
               std::string(kHandoffColumns));
  snapshot("model_intents", R"sql(
    owner text NOT NULL, logical_call_id text NOT NULL, model text NOT NULL,
    config_version kiarabot.uint64 NOT NULL, request_hash bytea NOT NULL CHECK(octet_length(request_hash)=32),
    state integer NOT NULL CHECK(state BETWEEN 0 AND 3), version kiarabot.uint64 NOT NULL,
    created_at bigint NOT NULL, observed_at bigint NOT NULL
  )sql");
  sql += R"sql(
CREATE TABLE kiarabot.command_receipts (
  subject text NOT NULL, method text NOT NULL, resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL, idempotency_key text NOT NULL,
  fingerprint bytea NOT NULL CHECK(octet_length(fingerprint)=32),
  result_id bigint NOT NULL UNIQUE REFERENCES kiarabot.replay_results(id),
  created_at bigint NOT NULL, retain_until bigint NOT NULL CHECK(retain_until>=created_at+604800000000000),
  PRIMARY KEY(subject,method,resource_kind,resource_id,idempotency_key)
);
CREATE INDEX command_receipts_expiry ON kiarabot.command_receipts(retain_until);
CREATE TABLE kiarabot.audit_entries (
  id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  actor text NOT NULL, resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL,
  request_id text NOT NULL, trace_id text NOT NULL, method text NOT NULL,
  rejection integer CHECK(rejection BETWEEN 0 AND 10), authorization_version kiarabot.uint64 NOT NULL,
  occurred_at bigint NOT NULL
);
)sql";
  sql +=
      "CREATE TABLE kiarabot.operations (" + std::string(kOperationColumns) + ",PRIMARY KEY(id));";
  sql += "CREATE TABLE kiarabot.outbox (" + std::string(kHandoffColumns) + R"sql(,
    payload_kind integer NOT NULL CHECK(payload_kind BETWEEN 0 AND 15), payload_id text NOT NULL,
    payload_version kiarabot.uint64 NOT NULL, lane integer NOT NULL CHECK(lane BETWEEN 0 AND 1),
    state integer NOT NULL CHECK(state BETWEEN 0 AND 4), attempts integer NOT NULL CHECK(attempts BETWEEN 0 AND 100),
    next_attempt_at bigint NOT NULL, claim_token text, lease_until bigint,
    PRIMARY KEY(source,message), CHECK((claim_token IS NULL)=(lease_until IS NULL)),
    CHECK(state<>1 OR (claim_token IS NOT NULL AND lease_until>created_at))
  );
CREATE INDEX outbox_due ON kiarabot.outbox(state,next_attempt_at,lease_until);
CREATE UNIQUE INDEX outbox_claim_tokens ON kiarabot.outbox(claim_token) WHERE claim_token IS NOT NULL;
)sql";
  sql += "CREATE TABLE kiarabot.inbox (" + std::string(kHandoffColumns) + R"sql(,
    received_at bigint NOT NULL, retain_until bigint NOT NULL,
    result_id bigint NOT NULL UNIQUE REFERENCES kiarabot.replay_results(id), PRIMARY KEY(source,message),
    CHECK(retain_until>=received_at+604800000000000 AND retain_until>=valid_until+604800000000000)
  );
CREATE INDEX inbox_expiry ON kiarabot.inbox(retain_until);
)sql";
  return sql;
}
} // namespace kiarabot::infrastructure::postgres::schema
