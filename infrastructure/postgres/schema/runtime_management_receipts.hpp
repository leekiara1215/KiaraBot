#pragma once
#include <string_view>
namespace kiarabot::infrastructure::postgres::schema {
inline constexpr std::string_view kRuntimeManagementReceipts = R"sql(
CREATE TABLE kiarabot.runtime_management_receipts (
 result_id bigint PRIMARY KEY REFERENCES kiarabot.command_receipts(result_id) ON DELETE CASCADE,
 format_version integer NOT NULL CHECK(format_version=1),
 method text NOT NULL, result_kind integer NOT NULL,
 operation_owner integer NOT NULL CHECK(operation_owner=1),
 operation_id text NOT NULL CHECK(operation_id<>''),
 resource_kind integer NOT NULL CHECK(resource_kind IN (0,4)), resource_id text NOT NULL CHECK(resource_id<>''),
 CHECK ((method='kiarabot.v1.RuntimeService/DisableAccount' AND result_kind=6 AND resource_kind=0)
 OR (method IN ('kiarabot.v1.RuntimeService/BindAdapter','kiarabot.v1.RuntimeService/UnbindAdapter') AND result_kind=16 AND resource_kind=0)
 OR (method='kiarabot.v1.RuntimeService/SetDesiredState' AND result_kind=8 AND resource_kind=4)
 OR (method='kiarabot.v1.RuntimeService/UpdateConfig' AND result_kind=18 AND resource_kind=4)),
 state_version kiarabot.uint64, lifecycle integer CHECK(lifecycle BETWEEN 0 AND 5),
 desired integer CHECK(desired BETWEEN 0 AND 1), registered boolean,
 desired_config_version kiarabot.uint64, applied_config_version kiarabot.uint64,
 process_outcome integer CHECK(process_outcome BETWEEN 0 AND 3), observed_at bigint,
 fence_instance text, fence_epoch text, fence_execution text,
 fence_execution_generation kiarabot.uint64, fence_connection text, fence_connection_generation kiarabot.uint64,
 CHECK ((resource_kind=0 AND state_version IS NULL AND lifecycle IS NULL AND desired IS NULL
 AND registered IS NULL AND desired_config_version IS NULL AND applied_config_version IS NULL
 AND process_outcome IS NULL AND observed_at IS NULL)
 OR (resource_kind=4 AND state_version IS NOT NULL AND lifecycle IS NOT NULL AND desired IS NOT NULL
 AND registered IS NOT NULL AND desired_config_version IS NOT NULL AND applied_config_version IS NOT NULL
 AND process_outcome IS NOT NULL AND observed_at IS NOT NULL)),
 CHECK ((fence_instance IS NULL AND fence_epoch IS NULL AND fence_execution IS NULL
 AND fence_execution_generation IS NULL AND fence_connection IS NULL AND fence_connection_generation IS NULL)
 OR (resource_kind=4 AND registered AND fence_instance=resource_id AND fence_epoch IS NOT NULL
 AND fence_execution IS NOT NULL AND fence_execution_generation IS NOT NULL
 AND fence_connection IS NOT NULL AND fence_connection_generation IS NOT NULL)),
 CHECK (registered IS NULL OR registered=(fence_instance IS NOT NULL))
);
)sql";
} // namespace kiarabot::infrastructure::postgres::schema
