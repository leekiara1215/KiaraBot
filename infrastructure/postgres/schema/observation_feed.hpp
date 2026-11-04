#pragma once

#include <string_view>

namespace kiarabot::infrastructure::postgres::schema {

// v1.2 owner observation feed journals. One head row per (owner-local) feed family; the family
// number is the frozen ObservationFamily order (1 logs, 2 messages, 3 tasks). Each family keeps an
// independent durable epoch/head/low-water so cursors can never cross families or datasets.
// Append and business mutation share one transaction; commit order comes from the per-family head
// row lock, never from nextval allocation order or occurred_at ordering.
inline constexpr std::string_view kObservationFeeds = R"sql(
CREATE TABLE kiarabot.observation_feed_head (
  family integer PRIMARY KEY CHECK(family IN (1,2,3)),
  epoch text NOT NULL CHECK(octet_length(epoch) BETWEEN 1 AND 128),
  sequence kiarabot.uint64 NOT NULL DEFAULT 0,
  low_water kiarabot.uint64 NOT NULL DEFAULT 0 CHECK(low_water<=sequence),
  observed_at bigint NOT NULL DEFAULT 0,
  max_events integer NOT NULL DEFAULT 16384 CHECK(max_events BETWEEN 1 AND 16384),
  max_bytes bigint NOT NULL DEFAULT 16777216 CHECK(max_bytes BETWEEN 1024 AND 16777216)
);
INSERT INTO kiarabot.observation_feed_head(family,epoch) VALUES
  (1,pg_catalog.gen_random_uuid()::text),
  (2,pg_catalog.gen_random_uuid()::text),
  (3,pg_catalog.gen_random_uuid()::text);
CREATE TABLE kiarabot.observation_feed_changes (
  family integer NOT NULL CHECK(family IN (1,2,3)),
  sequence kiarabot.uint64 NOT NULL CHECK(sequence>0),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL CHECK(octet_length(resource_id) BETWEEN 1 AND 256),
  observed_at bigint NOT NULL CHECK(observed_at>0),
  payload text NOT NULL CHECK(octet_length(payload) BETWEEN 2 AND 4096),
  encoded_bytes integer NOT NULL CHECK(encoded_bytes BETWEEN 2 AND 4096),
  PRIMARY KEY(family,sequence)
);
CREATE INDEX observation_feed_changes_scope ON
  kiarabot.observation_feed_changes(family,resource_kind,resource_id,sequence);
CREATE INDEX observation_feed_changes_expiry ON
  kiarabot.observation_feed_changes(family,observed_at);
)sql";

} // namespace kiarabot::infrastructure::postgres::schema
