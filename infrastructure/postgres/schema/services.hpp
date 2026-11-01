#pragma once

#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::infrastructure::postgres::schema {

inline std::string service_sql(core::target::Service owner) {
  std::string sql;
  const auto relation = [&sql](std::string_view table, std::string_view keys) {
    // Same typed column definitions as immutable results, distinct authoritative relations.
    // LIKE does not copy FK/indexes; their owner-local constraints are declared below.
    sql += "CREATE TABLE kiarabot." + std::string(table) + " (LIKE kiarabot.result_" +
           std::string(table) + " INCLUDING CONSTRAINTS);";
    sql += "ALTER TABLE kiarabot." + std::string(table) + " DROP COLUMN result_id;";
    sql += "ALTER TABLE kiarabot." + std::string(table) + " ADD PRIMARY KEY(" + std::string(keys) +
           ");";
  };
  switch (owner) {
  case core::target::Service::Identity:
    relation("principals", "id");
    relation("sessions", "id");
    relation("grants", "id");
    sql += R"sql(
ALTER TABLE kiarabot.sessions ADD FOREIGN KEY(subject) REFERENCES kiarabot.principals(id);
ALTER TABLE kiarabot.grants ADD FOREIGN KEY(subject) REFERENCES kiarabot.principals(id);
CREATE TABLE kiarabot.grant_scopes (
  grant_id text NOT NULL REFERENCES kiarabot.grants(id) ON DELETE CASCADE,
  position integer NOT NULL CHECK(position>=0), resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15),
  resource_id text NOT NULL, permission text NOT NULL, PRIMARY KEY(grant_id,position)
);
CREATE TABLE kiarabot.authorizations (
  subject text PRIMARY KEY REFERENCES kiarabot.principals(id), version kiarabot.uint64 NOT NULL,
  invalidation_dirty boolean NOT NULL
);
)sql";
    break;
  case core::target::Service::Runtime:
    relation("accounts", "id");
    relation("installations", "id");
    relation("instances", "id");
    sql += R"sql(
ALTER TABLE kiarabot.accounts ADD UNIQUE(platform,platform_account_id);
ALTER TABLE kiarabot.installations ADD UNIQUE(id,plugin_type);
ALTER TABLE kiarabot.instances ADD FOREIGN KEY(installation,plugin_type) REFERENCES kiarabot.installations(id,plugin_type);
ALTER TABLE kiarabot.instances ADD CHECK(fence_instance IS NULL OR fence_instance=id);
)sql";
    break;
  case core::target::Service::Scheduler:
    relation("tasks", "owner,task_id");
    relation("occurrences", "occurrence_id");
    sql += R"sql(
ALTER TABLE kiarabot.tasks ADD UNIQUE(occurrence_id);
ALTER TABLE kiarabot.tasks ADD UNIQUE(owner,task_id,installation,occurrence_id);
ALTER TABLE kiarabot.occurrences ADD UNIQUE(owner,task_id);
ALTER TABLE kiarabot.occurrences ADD FOREIGN KEY(owner,task_id,installation,occurrence_id) REFERENCES kiarabot.tasks(owner,task_id,installation,occurrence_id);
)sql";
    break;
  case core::target::Service::Model:
    relation("model_intents", "owner,logical_call_id");
    break;
  default:
    break;
  }
  return sql;
}
} // namespace kiarabot::infrastructure::postgres::schema
