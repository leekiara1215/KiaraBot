#pragma once

#include <string_view>

// P08b-3 Runtime-side Host command outbox and observation inbox. Commands are written in the same
// UoW as the instance desired-state mutation; delivery is transport state on the same row.
// Observations are deduplicated by (host_id, connection_generation, sequence), never by
// caller-supplied time. Empty-database schema only (P03 initializer); no migration.
inline constexpr std::string_view kRuntimeHostCommands = R"sql(
CREATE TABLE kiarabot.host_commands (
  command_id text PRIMARY KEY CHECK(command_id<>''),
  host_id text NOT NULL CHECK(host_id<>''),
  instance_id text NOT NULL CHECK(instance_id<>''),
  installation_id text NOT NULL CHECK(installation_id<>''),
  kind integer NOT NULL CHECK(kind BETWEEN 1 AND 3),
  execution_generation kiarabot.uint64 NOT NULL,
  desired_version kiarabot.uint64 NOT NULL,
  desired_state integer NOT NULL CHECK(desired_state BETWEEN 0 AND 2),
  package_sha256 text NOT NULL,
  launch_profile text NOT NULL,
  arguments text[] NOT NULL DEFAULT '{}',
  environment_names text[] NOT NULL DEFAULT '{}',
  payload_sha256 text NOT NULL CHECK(length(payload_sha256)=64),
  state integer NOT NULL CHECK(state BETWEEN 0 AND 3),
  connection_generation kiarabot.uint64 NOT NULL DEFAULT 0,
  runtime_epoch text NOT NULL DEFAULT '',
  created_at bigint NOT NULL,
  updated_at bigint NOT NULL,
  dispatched_at bigint NOT NULL DEFAULT 0,
  receipted_at bigint NOT NULL DEFAULT 0,
  receipt_persisted boolean,
  receipt_outcome integer CHECK(receipt_outcome BETWEEN 0 AND 3),
  receipt_process_marker text
);
CREATE INDEX host_commands_pending ON kiarabot.host_commands(host_id,state,created_at);
CREATE INDEX host_commands_instance ON kiarabot.host_commands(instance_id,execution_generation);
CREATE TABLE kiarabot.host_observations (
  host_id text NOT NULL CHECK(host_id<>''),
  connection_generation kiarabot.uint64 NOT NULL CHECK(connection_generation>0),
  sequence kiarabot.uint64 NOT NULL CHECK(sequence>0),
  instance_id text NOT NULL CHECK(instance_id<>''),
  execution_generation kiarabot.uint64 NOT NULL,
  outcome integer NOT NULL CHECK(outcome BETWEEN 0 AND 3),
  process_marker text NOT NULL DEFAULT '',
  command_id text NOT NULL DEFAULT '',
  observed_at bigint NOT NULL,
  PRIMARY KEY(host_id,connection_generation,sequence)
);
CREATE INDEX host_observations_instance
  ON kiarabot.host_observations(instance_id,execution_generation,observed_at);
)sql";
