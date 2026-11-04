#pragma once

#include <string_view>

// BOOT-02 Runtime-owned controlled bootstrap delivery intents/receipts. No credential plaintext, no
// SHA-256 digest, no filesystem path, no owner UID/GID and no environment value may be stored here:
// the digest remains in bridge_credentials and the file/deployment facts stay at the Control sink.
// One row per issued version; a reissue keeps the previous row (state REVOKED) and appends the
// successor, so history stays auditable without ever storing secret material.
inline constexpr std::string_view kRuntimeBootstrapDeliveries = R"sql(
CREATE TABLE kiarabot.bootstrap_deliveries (
  delivery_id text PRIMARY KEY CHECK(delivery_id<>''),
  -- ResourceKind: 1=RESOURCE_ACCOUNT (adapter), 5=RESOURCE_INSTANCE (managed/externally managed
  -- plugin). No unspecified or catalog rows are ever stored.
  resource_kind integer NOT NULL CHECK(resource_kind IN (1,5)),
  resource_id text NOT NULL CHECK(resource_id<>''),
  recipient_id text NOT NULL CHECK(recipient_id<>'') CHECK(btrim(recipient_id)<>''),
  credential_version kiarabot.uint64 NOT NULL CHECK(credential_version>0),
  -- BootstrapDeliveryState: 0=PENDING, 1=DELIVERED, 2=UNKNOWN, 3=REVOKED.
  state integer NOT NULL CHECK(state BETWEEN 0 AND 3),
  operation_id text NOT NULL CHECK(operation_id<>''),
  created_at bigint NOT NULL,
  observed_at bigint NOT NULL CHECK(observed_at>=created_at),
  receipt_id text NOT NULL DEFAULT '' CHECK(receipt_id='' OR btrim(receipt_id)<>'')
);
CREATE INDEX bootstrap_deliveries_resource
  ON kiarabot.bootstrap_deliveries(resource_kind,resource_id,credential_version DESC);
)sql";
