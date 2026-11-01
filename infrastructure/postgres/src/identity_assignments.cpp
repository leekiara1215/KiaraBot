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
bool same_metadata(const target::ResourceGrant& left, const target::ResourceGrant& right) {
  if (left.id != right.id || left.subject != right.subject || left.role != right.role ||
      left.version != right.version || left.scopes.size() != right.scopes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.scopes.size(); ++index) {
    if (left.scopes[index].resource != right.scopes[index].resource ||
        left.scopes[index].permission != right.scopes[index].permission) {
      return false;
    }
  }
  return true;
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
          const auto rows =
              transaction.exec("SELECT id,subject,version FROM kiarabot.exact_assignments "
                               "WHERE subject=$1 ORDER BY id",
                               pqxx::params{subject.value});
          std::vector<target::ExactAssignment> result;
          for (const auto& row : rows) {
            target::ExactAssignment assignment{{row["id"].template as<std::string>()},
                                               {row["subject"].template as<std::string>()},
                                               {},
                                               {},
                                               {row["version"].template as<std::uint64_t>()}};
            for (const auto& role : transaction.exec("SELECT role,resource_kind,resource_id FROM "
                                                     "kiarabot.exact_roles WHERE assignment=$1 "
                                                     "ORDER BY resource_kind,resource_id,role",
                                                     pqxx::params{assignment.id.value})) {
              assignment.roles.push_back(
                  {static_cast<target::FixedRole>(role["role"].template as<int>()),
                   {static_cast<target::ResourceKind>(role["resource_kind"].template as<int>()),
                    {role["resource_id"].template as<std::string>()}}});
            }
            for (const auto& permission :
                 transaction.exec("SELECT permission,resource_kind,resource_id "
                                  "FROM kiarabot.exact_permissions WHERE assignment=$1 ORDER BY "
                                  "resource_kind,resource_id,permission",
                                  pqxx::params{assignment.id.value})) {
              assignment.explicit_permissions.push_back(
                  {static_cast<target::Permission>(permission["permission"].template as<int>()),
                   {static_cast<target::ResourceKind>(
                        permission["resource_kind"].template as<int>()),
                    {permission["resource_id"].template as<std::string>()}}});
            }
            require_stored(valid_assignment_text(assignment));
            result.push_back(std::move(assignment));
          }
          require_stored(std::holds_alternative<std::monostate>(
              target::validate_assignment_collection(result, subject)));
          return result;
        });
  }
  target::Status save(const target::ExactAssignment& assignment,
                      std::optional<target::ResourceVersion> expected) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      if (!valid_assignment_text(assignment) || !valid_next_version(assignment.version, expected)) {
        return invalid();
      }
      const auto validation = target::validate_exact_assignment(assignment);
      if (const auto* failure = std::get_if<target::Failure>(&validation)) {
        return *failure;
      }
      const auto projected = target::to_resource_grant_metadata(assignment);
      if (const auto* failure = std::get_if<target::Failure>(&projected)) {
        return *failure;
      }
      const auto& metadata = std::get<target::ResourceGrant>(projected);
      if (!store_live(transaction, RecordKind::Grant, target::ReplayResult(metadata), expected)) {
        const auto stored = load_live(transaction, RecordKind::Grant,
                                      std::vector<std::string>{assignment.id.value});
        if (!stored || !same_metadata(std::get<target::ResourceGrant>(*stored), metadata)) {
          return conflict();
        }
      }
      pqxx::params key{assignment.id.value, assignment.subject.value, assignment.version.value};
      pqxx::result rows;
      if (expected) {
        key.append(expected->value);
        rows = transaction.exec("UPDATE kiarabot.exact_assignments SET version=$3 WHERE id=$1 "
                                "AND subject=$2 AND version=$4 RETURNING id",
                                key);
      } else {
        rows = transaction.exec("INSERT INTO kiarabot.exact_assignments VALUES($1,$2,$3) "
                                "ON CONFLICT DO NOTHING RETURNING id",
                                key);
      }
      if (rows.empty()) {
        return conflict();
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
