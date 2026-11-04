#pragma once

#include <string_view>

namespace kiarabot::infrastructure::postgres::schema {

inline constexpr std::string_view kOperationObservations = R"sql(
CREATE TABLE kiarabot.operation_observation_head (
  singleton boolean PRIMARY KEY CHECK(singleton),
  epoch text NOT NULL CHECK(octet_length(epoch) BETWEEN 1 AND 128),
  sequence kiarabot.uint64 NOT NULL DEFAULT 0,
  low_water kiarabot.uint64 NOT NULL DEFAULT 0 CHECK(low_water<=sequence),
  observed_at bigint NOT NULL DEFAULT 0,
  max_events integer NOT NULL DEFAULT 65536 CHECK(max_events BETWEEN 1 AND 65536),
  max_bytes bigint NOT NULL DEFAULT 67108864 CHECK(max_bytes BETWEEN 1024 AND 67108864)
);
INSERT INTO kiarabot.operation_observation_head(singleton,epoch)
  VALUES(true,pg_catalog.gen_random_uuid()::text);
CREATE TABLE kiarabot.operation_observations (
  sequence kiarabot.uint64 PRIMARY KEY CHECK(sequence>0),
  id text NOT NULL CHECK(octet_length(id) BETWEEN 1 AND 256),
  owner integer NOT NULL CHECK(owner IN (1,3,4)),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL CHECK(octet_length(resource_id) BETWEEN 1 AND 256),
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  state integer NOT NULL CHECK(state BETWEEN 0 AND 6),
  execution_outcome integer NOT NULL CHECK(execution_outcome BETWEEN 0 AND 3),
  blocked_reason integer CHECK(blocked_reason BETWEEN 0 AND 10),
  next_attempt bigint,
  observed_at bigint NOT NULL CHECK(observed_at>0),
  encoded_bytes integer NOT NULL CHECK(encoded_bytes BETWEEN 1 AND 65536)
);
CREATE INDEX operation_observations_scope ON
  kiarabot.operation_observations(resource_kind,resource_id,sequence);
CREATE INDEX operation_observations_expiry ON kiarabot.operation_observations(observed_at);
CREATE TABLE kiarabot.operation_observation_participants (
  sequence kiarabot.uint64 NOT NULL REFERENCES kiarabot.operation_observations(sequence)
    ON DELETE CASCADE,
  participant text NOT NULL CHECK(octet_length(participant) BETWEEN 1 AND 128),
  state integer NOT NULL CHECK(state BETWEEN 0 AND 6),
  reason integer CHECK(reason BETWEEN 0 AND 10),
  next_attempt bigint,
  PRIMARY KEY(sequence,participant)
);
)sql";

} // namespace kiarabot::infrastructure::postgres::schema
