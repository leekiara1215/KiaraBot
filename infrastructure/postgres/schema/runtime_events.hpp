#pragma once

#include <string_view>

// P06 Runtime event inbox and delivery ledger. Explicit empty-database schema v6; there is no
// migration path from v5 (development databases must be re-initialized, docs/architecture.md
// explicit init). Five tables are frozen by docs/architecture.md:29 and the P06 contract freeze:
//   events, conversation_watermarks, delivery_targets, delivery_attempts, ordered_delivery_heads.
// delivery_targets.ordinal is the stable per-acceptance ordering used by ordered groups; the
// ordered head table only tracks the current head ordinal so a later insert can never jump a
// still-pending earlier target.
inline constexpr std::string_view kRuntimeEvents = R"sql(
CREATE TABLE kiarabot.events (
  adapter text NOT NULL, event_id text NOT NULL,
  fingerprint bytea NOT NULL CHECK(octet_length(fingerprint)=32),
  conversation_key text NOT NULL, payload_kind smallint NOT NULL CHECK(payload_kind BETWEEN 0 AND 7),
  has_conversation boolean NOT NULL,
  sequence kiarabot.uint64, has_sequence boolean NOT NULL,
  serialized_envelope bytea NOT NULL CHECK(octet_length(serialized_envelope)<=1048576),
  occurred_at bigint NOT NULL, retain_until bigint,
  state smallint NOT NULL CHECK(state BETWEEN 0 AND 1),
  received_at bigint NOT NULL,
  PRIMARY KEY(adapter,event_id),
  CHECK((sequence IS NULL)=NOT has_sequence),
  CHECK((has_conversation AND length(conversation_key)>0) OR NOT has_conversation)
);
CREATE INDEX events_retention ON kiarabot.events(state,retain_until);

CREATE TABLE kiarabot.conversation_watermarks (
  conversation_key text PRIMARY KEY,
  has_sequence boolean NOT NULL, last_sequence kiarabot.uint64, degraded boolean NOT NULL,
  updated_at bigint NOT NULL,
  CHECK((last_sequence IS NULL)=NOT has_sequence)
);

CREATE TABLE kiarabot.delivery_targets (
  adapter text NOT NULL, event_id text NOT NULL, instance text NOT NULL,
  ordered boolean NOT NULL, trace_id text NOT NULL,
  state smallint NOT NULL CHECK(state BETWEEN 0 AND 4),
  attempts integer NOT NULL CHECK(attempts BETWEEN 0 AND 1000),
  next_attempt_at bigint NOT NULL, current_attempt text,
  dead_letter_reason smallint, updated_at bigint NOT NULL, retain_until bigint,
  ordinal bigint GENERATED ALWAYS AS IDENTITY UNIQUE,
  PRIMARY KEY(adapter,event_id,instance),
  FOREIGN KEY(adapter,event_id) REFERENCES kiarabot.events(adapter,event_id) ON DELETE CASCADE,
  CHECK(state<>3 OR current_attempt IS NOT NULL),
  CHECK(state<>4 OR dead_letter_reason IS NOT NULL)
);
CREATE INDEX delivery_targets_due ON kiarabot.delivery_targets(instance,state,next_attempt_at);
CREATE INDEX delivery_targets_retention ON kiarabot.delivery_targets(state,retain_until);

CREATE TABLE kiarabot.delivery_attempts (
  attempt_id text PRIMARY KEY,
  adapter text NOT NULL, event_id text NOT NULL, instance text NOT NULL,
  runtime_epoch text NOT NULL, execution_id text NOT NULL,
  execution_generation kiarabot.uint64 NOT NULL CHECK(execution_generation>0),
  connection_id text NOT NULL,
  connection_generation kiarabot.uint64 NOT NULL CHECK(connection_generation>0),
  started_at bigint NOT NULL, lease_until bigint NOT NULL CHECK(lease_until>started_at),
  attempt_number integer NOT NULL CHECK(attempt_number>0),
  state smallint NOT NULL CHECK(state BETWEEN 0 AND 3),
  UNIQUE(adapter,event_id,instance,attempt_number),
  FOREIGN KEY(adapter,event_id,instance)
    REFERENCES kiarabot.delivery_targets(adapter,event_id,instance) ON DELETE CASCADE
);
CREATE INDEX delivery_attempts_lease ON kiarabot.delivery_attempts(state,lease_until);

CREATE TABLE kiarabot.ordered_delivery_heads (
  instance text NOT NULL, conversation_key text NOT NULL,
  head_ordinal bigint, updated_at bigint NOT NULL,
  PRIMARY KEY(instance,conversation_key)
);
)sql";
