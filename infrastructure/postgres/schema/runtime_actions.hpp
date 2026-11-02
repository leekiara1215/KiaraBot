#pragma once

#include <string_view>

// P07 Runtime authoritative Action records plus persistent quota/policy/evidence relations.
// Empty-database schema only (no migration), same policy as the P03 explicit initializer.
// Six relations are frozen by doc/22:29 and the P07 contract proposal §5.1:
//   actions, action_idempotency, quota_windows, daily_budgets, adapter_result_evidence,
//   action_policies.
// Row-level conditional writes own dispatch/result/cancel/expiry races; accepted quota occupancy
// is never refunded by cancel/failure/expiry (conservative budget). Evidence is append-only.
inline constexpr std::string_view kRuntimeActions = R"sql(
CREATE TABLE kiarabot.actions (
  action_id text PRIMARY KEY CHECK(action_id<>''),
  owner_instance text NOT NULL CHECK(owner_instance<>''),
  account_id text NOT NULL CHECK(account_id<>''),
  adapter_id text NOT NULL CHECK(adapter_id<>''),
  dispatched_to_instance text,
  binding_version kiarabot.uint64 NOT NULL,
  kind smallint NOT NULL CHECK(kind BETWEEN 1 AND 6),
  capability text NOT NULL CHECK(capability<>''),
  idempotency_key text NOT NULL CHECK(idempotency_key<>''),
  request_hash bytea NOT NULL CHECK(octet_length(request_hash)=32),
  serialized_envelope bytea NOT NULL CHECK(octet_length(serialized_envelope)<=65536),
  target_conversation text NOT NULL CHECK(target_conversation<>''),
  target_conversation_kind smallint NOT NULL CHECK(target_conversation_kind BETWEEN 0 AND 2),
  deadline bigint NOT NULL,
  created_at bigint NOT NULL,
  updated_at bigint NOT NULL,
  state smallint NOT NULL CHECK(state BETWEEN 0 AND 4),
  execution_outcome smallint NOT NULL CHECK(execution_outcome BETWEEN 0 AND 3),
  dispatch_state smallint NOT NULL CHECK(dispatch_state BETWEEN 0 AND 1),
  dispatch_attempt_id text,
  dispatch_sequence integer NOT NULL CHECK(dispatch_sequence>=0),
  dispatched_at bigint,
  platform_message_id text,
  error_code smallint CHECK(error_code BETWEEN 0 AND 10),
  completed_at bigint,
  retain_until bigint NOT NULL,
  UNIQUE(owner_instance,idempotency_key),
  UNIQUE(dispatch_attempt_id)
);
CREATE INDEX actions_dispatch ON kiarabot.actions(adapter_id,dispatched_to_instance,state,deadline);
CREATE INDEX actions_retention ON kiarabot.actions(state,retain_until);

CREATE TABLE kiarabot.action_idempotency (
  owner_instance text NOT NULL,
  idempotency_key text NOT NULL,
  request_hash bytea NOT NULL CHECK(octet_length(request_hash)=32),
  action_id text NOT NULL UNIQUE REFERENCES kiarabot.actions(action_id) ON DELETE CASCADE,
  created_at bigint NOT NULL,
  PRIMARY KEY(owner_instance,idempotency_key)
);

CREATE TABLE kiarabot.quota_windows (
  action_id text NOT NULL REFERENCES kiarabot.actions(action_id) ON DELETE CASCADE,
  kind smallint NOT NULL CHECK(kind BETWEEN 1 AND 6),
  scope_kind smallint NOT NULL CHECK(scope_kind BETWEEN 0 AND 2),
  scope_id text NOT NULL,
  accepted_at bigint NOT NULL,
  PRIMARY KEY(action_id,scope_kind)
);
CREATE INDEX quota_windows_scan ON kiarabot.quota_windows(kind,scope_kind,scope_id,accepted_at);

CREATE TABLE kiarabot.daily_budgets (
  kind smallint NOT NULL CHECK(kind BETWEEN 1 AND 6),
  account_id text NOT NULL,
  utc_day bigint NOT NULL,
  used integer NOT NULL CHECK(used>=0),
  PRIMARY KEY(kind,account_id,utc_day)
);

CREATE TABLE kiarabot.adapter_result_evidence (
  action_id text NOT NULL REFERENCES kiarabot.actions(action_id) ON DELETE CASCADE,
  dispatch_attempt_id text NOT NULL,
  evidence_kind smallint NOT NULL CHECK(evidence_kind BETWEEN 0 AND 3),
  execution_outcome smallint NOT NULL CHECK(execution_outcome BETWEEN 0 AND 3),
  adapter_id text NOT NULL,
  adapter_instance text NOT NULL,
  observed_at bigint NOT NULL,
  received_at bigint NOT NULL,
  error_code smallint CHECK(error_code BETWEEN 0 AND 10),
  platform_reference text CHECK(platform_reference IS NULL OR length(platform_reference)<=256),
  retain_until bigint NOT NULL,
  PRIMARY KEY(action_id,dispatch_attempt_id,evidence_kind)
);
CREATE INDEX evidence_retention ON kiarabot.adapter_result_evidence(retain_until);

CREATE TABLE kiarabot.action_policies (
  account_id text PRIMARY KEY CHECK(account_id<>''),
  mute_enabled boolean NOT NULL,
  kick_enabled boolean NOT NULL,
  allowed_group_ids text[] NOT NULL DEFAULT '{}',
  mute_daily_budget integer NOT NULL CHECK(mute_daily_budget>=0),
  kick_daily_budget integer NOT NULL CHECK(kick_daily_budget>=0),
  version kiarabot.uint64 NOT NULL,
  updated_at bigint NOT NULL
);
)sql";
