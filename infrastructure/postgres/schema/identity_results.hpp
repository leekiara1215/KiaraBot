#pragma once

#include <string_view>

namespace kiarabot::infrastructure::postgres::schema {
// Immutable public success snapshots, available in every owner's ordinary receipt store.
// No foreign key to mutable live authority and no secret/verifier columns.
inline constexpr std::string_view kIdentityResults = R"sql(
CREATE TABLE kiarabot.result_registrations (
  result_id bigint PRIMARY KEY REFERENCES kiarabot.replay_results(id) ON DELETE CASCADE,
  principal_id text NOT NULL CHECK(principal_id<>''), kind integer NOT NULL CHECK(kind BETWEEN 0 AND 3),
  binding_kind integer CHECK(binding_kind BETWEEN 0 AND 15), binding_id text CHECK(binding_id<>''),
  version kiarabot.uint64 NOT NULL CHECK(version>0), state integer NOT NULL CHECK(state BETWEEN 0 AND 5),
  credential_id text NOT NULL CHECK(credential_id<>''), credential_version kiarabot.uint64 NOT NULL CHECK(credential_version>0),
  credential_subject text NOT NULL CHECK(credential_subject=principal_id),
  credential_binding_kind integer NOT NULL CHECK(credential_binding_kind BETWEEN 0 AND 15),
  credential_binding_id text NOT NULL CHECK(credential_binding_id<>''),
  credential_state integer NOT NULL CHECK(credential_state BETWEEN 0 AND 3),
  created_at bigint NOT NULL, expires_at bigint, checked_at bigint NOT NULL,
  CHECK((binding_kind IS NULL)=(binding_id IS NULL))
);
CREATE TABLE kiarabot.result_exact_assignments (
  result_id bigint PRIMARY KEY REFERENCES kiarabot.replay_results(id) ON DELETE CASCADE,
  id text NOT NULL CHECK(id<>''), subject text NOT NULL CHECK(subject<>''),
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  role_count bigint NOT NULL CHECK(role_count>=0), permission_count bigint NOT NULL CHECK(permission_count>=0),
  CHECK((role_count>0 AND permission_count=0) OR (role_count=0 AND permission_count>0))
);
CREATE TABLE kiarabot.result_exact_roles (
  result_id bigint NOT NULL REFERENCES kiarabot.result_exact_assignments(result_id) ON DELETE CASCADE,
  position bigint NOT NULL CHECK(position>=0), role integer NOT NULL CHECK(role BETWEEN 0 AND 2),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL CHECK(resource_id<>''),
  PRIMARY KEY(result_id,position)
);
CREATE TABLE kiarabot.result_exact_permissions (
  result_id bigint NOT NULL REFERENCES kiarabot.result_exact_assignments(result_id) ON DELETE CASCADE,
  position bigint NOT NULL CHECK(position>=0), permission integer NOT NULL CHECK(permission BETWEEN 1 AND 40),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL CHECK(resource_id<>''),
  PRIMARY KEY(result_id,position)
);
)sql";
} // namespace kiarabot::infrastructure::postgres::schema
