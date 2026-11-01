#include <algorithm>
#include <utility>

#include "identity_access.hpp"
#include "identity_security.hpp"
#include "kiarabot/core/domain/target/identity_policy.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;
using identity::conflict;
using identity::invalid;
using identity::query;
using identity::require_stored;
using identity::valid_id;
using identity::valid_next_version;

bool valid_assignment_text(const target::ExactAssignment& assignment) {
  if (!valid_id(assignment.id.value) || !valid_id(assignment.subject.value)) {
    return false;
  }
  for (const auto& role : assignment.roles) {
    if (!valid_id(role.resource.id.value)) {
      return false;
    }
  }
  for (const auto& permission : assignment.explicit_permissions) {
    if (!valid_id(permission.resource.id.value)) {
      return false;
    }
  }
  return true;
}
target::AssignmentStateRecord read_assignment(pqxx::transaction_base& transaction,
                                              const pqxx::row& row) {
  target::AssignmentStateRecord record{{{row["id"].as<std::string>()},
                                        {row["subject"].as<std::string>()},
                                        {},
                                        {},
                                        {row["version"].as<std::uint64_t>()}},
                                       static_cast<target::AssignmentState>(row["state"].as<int>()),
                                       optional_time(row, "revoked_at")};
  auto& assignment = record.assignment;
  for (const auto& role : transaction.exec(
           "SELECT role,resource_kind,resource_id FROM kiarabot.exact_roles WHERE assignment=$1 "
           "ORDER BY resource_kind,resource_id,role",
           pqxx::params{assignment.id.value})) {
    assignment.roles.push_back({static_cast<target::FixedRole>(role["role"].as<int>()),
                                {static_cast<target::ResourceKind>(role["resource_kind"].as<int>()),
                                 {role["resource_id"].as<std::string>()}}});
  }
  for (const auto& permission : transaction.exec("SELECT permission,resource_kind,resource_id FROM "
                                                 "kiarabot.exact_permissions WHERE assignment=$1 "
                                                 "ORDER BY resource_kind,resource_id,permission",
                                                 pqxx::params{assignment.id.value})) {
    assignment.explicit_permissions.push_back(
        {static_cast<target::Permission>(permission["permission"].as<int>()),
         {static_cast<target::ResourceKind>(permission["resource_kind"].as<int>()),
          {permission["resource_id"].as<std::string>()}}});
  }
  const auto base = transaction.exec("SELECT subject,version FROM kiarabot.grants WHERE id=$1",
                                     pqxx::params{assignment.id.value});
  require_stored(base.size() == 1 &&
                 base.one_row()["subject"].as<std::string>() == assignment.subject.value &&
                 base.one_row()["version"].as<std::uint64_t>() == assignment.version.value);
  require_stored(valid_assignment_text(assignment) &&
                 std::holds_alternative<std::monostate>(target::validate_assignment_state(record)));
  return record;
}
bool same_scope(const target::ExactAssignment& left, const target::ExactAssignment& right) {
  return std::is_permutation(left.roles.begin(), left.roles.end(), right.roles.begin(),
                             right.roles.end()) &&
         std::is_permutation(left.explicit_permissions.begin(), left.explicit_permissions.end(),
                             right.explicit_permissions.begin(), right.explicit_permissions.end());
}
class Assignments final : public target::IdentityExactAssignmentRepository {
public:
  explicit Assignments(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::vector<target::ExactAssignment>>
  for_subject(const target::SubjectId& subject) override {
    return query<std::vector<target::ExactAssignment>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::vector<target::ExactAssignment>> {
          if (!valid_id(subject.value)) {
            return invalid();
          }
          const auto rows = transaction.exec("SELECT * FROM kiarabot.exact_assignments "
                                             "WHERE subject=$1 AND state=0 ORDER BY id",
                                             pqxx::params{subject.value});
          std::vector<target::ExactAssignment> result;
          for (const auto& row : rows) {
            result.push_back(read_assignment(transaction, row).assignment);
          }
          require_stored(std::holds_alternative<std::monostate>(
              target::validate_assignment_collection(result, subject)));
          return result;
        });
  }
  target::Outcome<std::optional<target::AssignmentStateRecord>>
  current(const target::AssignmentId& id) override {
    return query<std::optional<target::AssignmentStateRecord>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::AssignmentStateRecord>> {
          if (!valid_id(id.value)) {
            return invalid();
          }
          const auto rows = transaction.exec("SELECT * FROM kiarabot.exact_assignments WHERE id=$1",
                                             pqxx::params{id.value});
          if (rows.empty()) {
            return std::optional<target::AssignmentStateRecord>{};
          }
          return std::optional<target::AssignmentStateRecord>(
              read_assignment(transaction, rows.one_row()));
        });
  }
  target::Status save(const target::ExactAssignment& assignment,
                      std::optional<target::ResourceVersion> expected) override {
    return save_state({assignment, target::AssignmentState::Active, {}}, expected);
  }
  target::Status save_state(const target::AssignmentStateRecord& record,
                            std::optional<target::ResourceVersion> expected) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      const auto& assignment = record.assignment;
      if (!valid_assignment_text(assignment) || !valid_next_version(assignment.version, expected) ||
          (!expected &&
           (assignment.version.value != 1 || record.state != target::AssignmentState::Active))) {
        return invalid();
      }
      const auto validation = target::validate_exact_assignment(assignment);
      if (const auto* failure = std::get_if<target::Failure>(&validation)) {
        return *failure;
      }
      const auto state_validation = target::validate_assignment_state(record);
      if (const auto* failure = std::get_if<target::Failure>(&state_validation)) {
        return *failure;
      }
      const auto old =
          transaction.exec("SELECT * FROM kiarabot.exact_assignments WHERE id=$1 FOR UPDATE",
                           pqxx::params{assignment.id.value});
      if (old.empty()) {
        if (expected) {
          return conflict();
        }
      } else {
        const auto previous = read_assignment(transaction, old.one_row());
        if (!expected || previous.assignment.version != *expected ||
            previous.assignment.subject != assignment.subject ||
            previous.state == target::AssignmentState::Revoked ||
            (record.state == target::AssignmentState::Revoked &&
             !same_scope(previous.assignment, assignment))) {
          return conflict();
        }
      }
      if (record.revoked_at &&
          !transaction
               .exec(
                   "SELECT $1::bigint<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
                   pqxx::params{timestamp(*record.revoked_at)})
               .one_row()[0]
               .as<bool>()) {
        return invalid();
      }
      const auto projected = target::to_resource_grant_metadata(assignment);
      if (const auto* failure = std::get_if<target::Failure>(&projected)) {
        return *failure;
      }
      const auto& metadata = std::get<target::ResourceGrant>(projected);
      if (!store_live(transaction, RecordKind::Grant, target::ReplayResult(metadata), expected)) {
        return conflict();
      }
      pqxx::params key{assignment.id.value, assignment.subject.value, assignment.version.value,
                       static_cast<int>(record.state),
                       record.revoked_at ? std::optional<std::string>(timestamp(*record.revoked_at))
                                         : std::nullopt};
      pqxx::result rows;
      if (expected) {
        key.append(expected->value);
        rows = transaction.exec(
            "UPDATE kiarabot.exact_assignments SET version=$3,state=$4,revoked_at=$5 WHERE id=$1 "
            "AND subject=$2 AND version=$6 AND state=0 RETURNING id",
            key);
      } else {
        rows = transaction.exec("INSERT INTO kiarabot.exact_assignments VALUES($1,$2,$3,$4,$5) "
                                "ON CONFLICT DO NOTHING RETURNING id",
                                key);
      }
      if (rows.empty()) {
        return conflict();
      }
      if (record.state == target::AssignmentState::Revoked) {
        return std::monostate{};
      }
      transaction.exec("DELETE FROM kiarabot.exact_roles WHERE assignment=$1",
                       pqxx::params{assignment.id.value});
      transaction.exec("DELETE FROM kiarabot.exact_permissions WHERE assignment=$1",
                       pqxx::params{assignment.id.value});
      for (const auto& role : assignment.roles) {
        transaction.exec("INSERT INTO kiarabot.exact_roles VALUES($1,$2,$3,$4)",
                         pqxx::params{assignment.id.value, static_cast<int>(role.role),
                                      static_cast<int>(role.resource.kind),
                                      role.resource.id.value});
      }
      for (const auto& permission : assignment.explicit_permissions) {
        transaction.exec("INSERT INTO kiarabot.exact_permissions VALUES($1,$2,$3,$4)",
                         pqxx::params{assignment.id.value, static_cast<int>(permission.permission),
                                      static_cast<int>(permission.resource.kind),
                                      permission.resource.id.value});
      }
      return std::monostate{};
    });
  }

private:
  SqlWork& sql_;
};
} // namespace
std::unique_ptr<target::IdentityExactAssignmentRepository>
exact_assignment_repository(SqlWork& sql) {
  return std::make_unique<Assignments>(sql);
}
} // namespace kiarabot::infrastructure::postgres::detail
