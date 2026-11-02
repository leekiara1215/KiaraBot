#pragma once

#include <string_view>

// P09 Runtime private storage/namespace relations. Empty-database schema only (no migration),
// same policy as the P03 explicit initializer. The four frozen relations are namespaces,
// namespace_members, namespace_values and namespace_quota_counters; namespace_receipt_results is
// the Runtime-private typed payload side relation for command_receipts replays (the receipt row
// itself stays in the shared store).
inline constexpr std::string_view kRuntimeStorage = R"sql(
CREATE TABLE kiarabot.namespaces (
  namespace_id text PRIMARY KEY CHECK(namespace_id<>''),
  installation_id text NOT NULL CHECK(installation_id<>''),
  kind smallint NOT NULL CHECK(kind IN (0,1)),
  owner_instance text,
  disabled boolean NOT NULL DEFAULT false,
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  created_at bigint NOT NULL,
  updated_at bigint NOT NULL,
  CHECK((kind=0 AND owner_instance IS NOT NULL AND btrim(owner_instance)<>'')
        OR (kind=1 AND owner_instance IS NULL))
);
CREATE UNIQUE INDEX namespaces_private_owner ON kiarabot.namespaces(owner_instance)
  WHERE kind=0;
CREATE UNIQUE INDEX namespaces_shared_installation ON kiarabot.namespaces(installation_id)
  WHERE kind=1;

CREATE TABLE kiarabot.namespace_members (
  namespace_id text NOT NULL REFERENCES kiarabot.namespaces(namespace_id) ON DELETE CASCADE,
  instance_id text NOT NULL CHECK(instance_id<>''),
  can_read boolean NOT NULL,
  can_write boolean NOT NULL,
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  joined_at bigint NOT NULL,
  PRIMARY KEY(namespace_id,instance_id),
  CHECK(can_read OR can_write)
);

CREATE TABLE kiarabot.namespace_values (
  namespace_id text NOT NULL REFERENCES kiarabot.namespaces(namespace_id) ON DELETE CASCADE,
  key text NOT NULL CHECK(key<>'' AND octet_length(key)<=256),
  value bytea NOT NULL CHECK(octet_length(value)<=65536),
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  expires_at bigint,
  updated_at bigint NOT NULL,
  PRIMARY KEY(namespace_id,key),
  CHECK(expires_at IS NULL OR expires_at>0)
);
CREATE INDEX namespace_values_expiry ON kiarabot.namespace_values(namespace_id,expires_at)
  WHERE expires_at IS NOT NULL;

-- scope_kind 0 = namespace, 1 = installation aggregate. Counters are authoritative and adjusted
-- in the same UoW as the value row under the fixed lock order (namespace, then installation).
CREATE TABLE kiarabot.namespace_quota_counters (
  scope_kind smallint NOT NULL CHECK(scope_kind IN (0,1)),
  scope_id text NOT NULL CHECK(scope_id<>''),
  key_count integer NOT NULL CHECK(key_count>=0),
  byte_count bigint NOT NULL CHECK(byte_count>=0),
  updated_at bigint NOT NULL,
  PRIMARY KEY(scope_kind,scope_id)
);

-- Typed replay payload for command_receipts rows created by P09 storage/management writes. The
-- shared receipt store still owns uniqueness, fingerprint, retention and capacity primitives.
CREATE TABLE kiarabot.namespace_receipt_results (
  result_id bigint PRIMARY KEY REFERENCES kiarabot.replay_results(id) ON DELETE CASCADE,
  result_kind smallint NOT NULL CHECK(result_kind IN (0,1,2)),
  instance_id text NOT NULL DEFAULT '',
  installation_id text NOT NULL DEFAULT '',
  scope_kind smallint,
  scope_id text,
  key text,
  deleted boolean,
  version kiarabot.uint64,
  expires_at bigint,
  value bytea CHECK(value IS NULL OR octet_length(value)<=65536),
  namespace_id text,
  namespace_installation text,
  namespace_version kiarabot.uint64,
  namespace_disabled boolean,
  namespace_created_at bigint,
  namespace_updated_at bigint,
  members_version kiarabot.uint64,
  CHECK((result_kind=0 AND scope_kind IN (0,1) AND scope_id IS NOT NULL AND key IS NOT NULL
         AND deleted IS NOT NULL)
        OR (result_kind=1 AND namespace_id IS NOT NULL AND namespace_installation IS NOT NULL
            AND namespace_version IS NOT NULL AND namespace_disabled IS NOT NULL)
        OR (result_kind=2 AND members_version IS NOT NULL))
);
CREATE INDEX namespace_receipt_results_capacity
  ON kiarabot.namespace_receipt_results(instance_id,installation_id);
)sql";
