#pragma once

#include <string_view>

// HOST-03 Runtime-owned Host admission gate. One durable row per registered Host; the Runtime
// rejects dispatching/migrating new work for that Host while admission is CLOSED. The row is
// authoritative Runtime state (Host SQLite owns the independent maintenance operation), so the
// server can keep refusing after a Runtime restart instead of silently reopening admission.
// Numeric admission encoding: 1 = OPEN, 2 = CLOSED (matches RuntimeSetHostAdmissionGateRequest).
inline constexpr std::string_view kRuntimeHostAdmissionGates = R"sql(
CREATE TABLE kiarabot.host_admission_gates (
  host_id text PRIMARY KEY REFERENCES kiarabot.hosts(id) CHECK(host_id<>''),
  maintenance_version kiarabot.uint64 NOT NULL CHECK(maintenance_version>0),
  gate_version kiarabot.uint64 NOT NULL CHECK(gate_version>0),
  admission integer NOT NULL CHECK(admission IN (1,2)),
  operation_id text NOT NULL CHECK(operation_id<>''),
  updated_at bigint NOT NULL
);
)sql";
