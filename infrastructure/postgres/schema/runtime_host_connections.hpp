#pragma once

#include <string_view>

// P08b Runtime-side authoritative HostBridge connection projection. Deployment certificate
// allowlists stay in trusted Runtime configuration; this relation only stores the current
// connection generation assigned at registration plus liveness/config-version facts.
// Empty-database schema only (P03 initializer); no migration.
inline constexpr std::string_view kRuntimeHostConnections = R"sql(
CREATE TABLE kiarabot.host_connections (
  host_id text PRIMARY KEY CHECK(host_id<>''),
  connection_generation kiarabot.uint64 NOT NULL CHECK(connection_generation>0),
  runtime_epoch text NOT NULL CHECK(runtime_epoch<>''),
  registered_at bigint NOT NULL,
  last_heartbeat_at bigint NOT NULL,
  online boolean NOT NULL,
  desired_config_version kiarabot.uint64 NOT NULL
);
)sql";
