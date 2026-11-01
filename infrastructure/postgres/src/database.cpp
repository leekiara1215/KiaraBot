#include "kiarabot/infrastructure/postgres/database.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <utility>

#include <pqxx/pqxx>

#include "../schema/v1.hpp"

namespace kiarabot::infrastructure::postgres {
namespace {
namespace target = core::target;
constexpr int kWriterLockClass = 1262638164;
constexpr int kWriterLockId = 1;

std::string service_name(target::Service service) {
  switch (service) {
  case target::Service::Identity:
    return "identity";
  case target::Service::Runtime:
    return "runtime";
  case target::Service::Scheduler:
    return "scheduler";
  case target::Service::Model:
    return "model";
  default:
    return {};
  }
}

target::Failure failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}

bool identifier(const std::string& value) {
  return !value.empty() && value.size() <= 63 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_';
         });
}

bool valid_config(const DatabaseConfig& config, bool migration) {
  // libpq consults these for unspecified options, even when a DSN supplies host/password.
  // Reject ambient connection profiles rather than mutating global process environment.
  for (const auto* variable : {"PGSERVICE", "PGSERVICEFILE", "PGHOSTADDR", "PGOPTIONS", "PGSSLCERT",
                               "PGSSLKEY", "PGPASSFILE"}) {
    if (std::getenv(variable) != nullptr) {
      return false;
    }
  }
  return !service_name(config.service).empty() && !config.host.empty() &&
         config.host.front() != '/' && config.host.find(',') == std::string::npos &&
         config.host.find('\0') == std::string::npos && config.port != 0 &&
         identifier(config.database) && identifier(config.migration_role) &&
         identifier(config.runtime_role) && config.migration_role != config.runtime_role &&
         config.user == (migration ? config.migration_role : config.runtime_role) &&
         !config.password.empty() && config.password.find('\0') == std::string::npos &&
         !config.root_certificate.empty() &&
         config.root_certificate.find('\0') == std::string::npos;
}

std::string quote_option(std::string_view value) {
  std::string quoted = "'";
  for (char character : value) {
    if (character == '\\' || character == '\'') {
      quoted += '\\';
    }
    quoted += character;
  }
  quoted += '\'';
  return quoted;
}

std::unique_ptr<pqxx::connection> connect(const DatabaseConfig& config) {
  auto connection = std::make_unique<pqxx::connection>(
      "host=" + quote_option(config.host) + " port=" + std::to_string(config.port) +
      " dbname=" + quote_option(config.database) + " user=" + quote_option(config.user) +
      " password=" + quote_option(config.password) +
      " sslrootcert=" + quote_option(config.root_certificate) +
      " sslmode=verify-full channel_binding=require gssencmode=disable connect_timeout=5"
      " options='-c statement_timeout=3000 -c lock_timeout=1000"
      " -c idle_in_transaction_session_timeout=5000 -c search_path=pg_catalog'"
      " application_name=kiarabot");
  if (connection->server_version() != 160004) {
    throw std::runtime_error("unsupported database version");
  }
  return connection;
}

bool roles_valid(pqxx::transaction_base& transaction, const DatabaseConfig& config) {
  const auto roles = transaction.exec(
      "SELECT rolname, rolsuper OR rolcreaterole OR rolcreatedb OR rolreplication OR rolbypassrls "
      "AS elevated, rolcanlogin FROM pg_roles WHERE rolname IN ($1,$2)",
      pqxx::params{config.migration_role, config.runtime_role});
  if (roles.size() != 2) {
    return false;
  }
  for (const auto& row : roles) {
    if (row["elevated"].as<bool>() || !row["rolcanlogin"].as<bool>()) {
      return false;
    }
  }
  // Membership in another role could permit SET ROLE or inherited DDL/cross-database access.
  const auto memberships =
      transaction.exec("SELECT count(*) FROM pg_auth_members m JOIN pg_roles r ON r.oid=m.member "
                       "WHERE r.rolname IN ($1,$2)",
                       pqxx::params{config.migration_role, config.runtime_role});
  const auto database = transaction.exec("SELECT pg_get_userbyid(datdba)=$1 AS owned, "
                                         "NOT has_database_privilege($2,oid,'CREATE') AND "
                                         "NOT has_database_privilege($2,oid,'TEMP') AS restricted "
                                         "FROM pg_database WHERE datname=current_database()",
                                         pqxx::params{config.migration_role, config.runtime_role});
  const auto grants =
      transaction.exec("SELECT count(*) FROM pg_database d, LATERAL aclexplode(COALESCE(d.datacl,"
                       "acldefault('d',d.datdba))) a WHERE d.datname=current_database() "
                       "AND a.privilege_type IN ('CONNECT','CREATE','TEMPORARY') "
                       "AND (a.grantee=0 OR pg_get_userbyid(a.grantee) NOT IN ($1,$2))",
                       pqxx::params{config.migration_role, config.runtime_role});
  return memberships.one_row()[0].as<int>() == 0 && database.one_row()["owned"].as<bool>() &&
         database.one_row()["restricted"].as<bool>() && grants.one_row()[0].as<int>() == 0;
}

bool schema_valid(pqxx::transaction_base& transaction, const DatabaseConfig& config) {
  const auto object =
      transaction.exec("SELECT c.relkind='r' AND pg_get_userbyid(c.relowner)=$1 AS owned "
                       "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
                       "WHERE n.nspname='kiarabot' AND c.relname='schema_versions'",
                       pqxx::params{config.migration_role});
  if (object.size() != 1 || !object[0]["owned"].as<bool>()) {
    return false;
  }
  const auto columns =
      transaction.exec("SELECT a.attname, format_type(a.atttypid,a.atttypmod), a.attnotnull "
                       "FROM pg_attribute a WHERE a.attrelid='kiarabot.schema_versions'::regclass "
                       "AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum");
  if (columns.size() != 3 || columns[0][0].as<std::string>() != "singleton" ||
      columns[0][1].as<std::string>() != "boolean" ||
      columns[1][0].as<std::string>() != "version" ||
      columns[1][1].as<std::string>() != "integer" ||
      columns[2][0].as<std::string>() != "service" || columns[2][1].as<std::string>() != "text") {
    return false;
  }
  for (const auto& column : columns) {
    if (!column[2].as<bool>()) {
      return false;
    }
  }
  const auto records =
      transaction.exec("SELECT singleton,version,service FROM kiarabot.schema_versions");
  const auto permissions =
      transaction.exec("SELECT has_schema_privilege($1,'kiarabot','USAGE') AND "
                       "NOT has_schema_privilege($1,'kiarabot','CREATE') AND "
                       "NOT has_schema_privilege($1,'public','CREATE') AND "
                       "has_table_privilege($1,'kiarabot.schema_versions','SELECT') AND "
                       "NOT "
                       "has_table_privilege($1,'kiarabot.schema_versions','INSERT,UPDATE,DELETE,"
                       "TRUNCATE,REFERENCES,TRIGGER')",
                       pqxx::params{config.runtime_role});
  const auto schema_permissions = transaction.exec(
      "SELECT pg_get_userbyid(nspowner)=$1 AND "
      "NOT EXISTS(SELECT 1 FROM aclexplode(COALESCE(nspacl,acldefault('n',nspowner))) a "
      "WHERE a.grantee=0) FROM pg_namespace WHERE nspname='kiarabot'",
      pqxx::params{config.migration_role});
  const auto constraints = transaction.exec(
      "SELECT contype, pg_get_constraintdef(oid) FROM pg_constraint "
      "WHERE conrelid='kiarabot.schema_versions'::regclass ORDER BY contype, conname");
  constexpr std::array<std::string_view, 4> kConstraintDefinitions{
      "CHECK ((service = ANY (ARRAY['identity'::text, 'runtime'::text, 'scheduler'::text, "
      "'model'::text])))",
      "CHECK (singleton)", "CHECK ((version > 0))", "PRIMARY KEY (singleton)"};
  if (constraints.size() != kConstraintDefinitions.size()) {
    return false;
  }
  for (std::size_t index = 0; index < kConstraintDefinitions.size(); ++index) {
    if (constraints[index][1].as<std::string>() != kConstraintDefinitions[index]) {
      return false;
    }
  }
  const auto objects = transaction.exec(
      "SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
      "WHERE n.nspname='kiarabot'");
  return records.size() == 1 && records[0][0].as<bool>() &&
         records[0][1].as<int>() == schema::kVersion &&
         records[0][2].as<std::string>() == service_name(config.service) &&
         permissions.one_row()[0].as<bool>() && schema_permissions.one_row()[0].as<bool>() &&
         objects.one_row()[0].as<int>() == 2;
}

target::Failure translate_exception() {
  try {
    throw;
  } catch (const pqxx::broken_connection&) {
    return failure(target::ErrorCode::UpstreamUnavailable);
  } catch (const pqxx::insufficient_privilege&) {
    return failure(target::ErrorCode::PermissionDenied);
  } catch (const pqxx::sql_error& error) {
    const auto state = error.sqlstate();
    if (state == "55P03" || state == "57014") {
      return failure(target::ErrorCode::Timeout);
    }
    return failure(target::ErrorCode::Internal);
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace

struct Database::Impl {
  std::unique_ptr<pqxx::connection> connection;
  bool disabled = false;
};

core::target::Status initialize_empty(const DatabaseConfig& config) {
  if (!valid_config(config, true)) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  try {
    auto connection = connect(config);
    pqxx::work transaction(*connection);
    if (!roles_valid(transaction, config)) {
      return failure(target::ErrorCode::PermissionDenied);
    }
    const auto locked = transaction.exec("SELECT pg_try_advisory_xact_lock($1,$2)",
                                         pqxx::params{kWriterLockClass, kWriterLockId});
    if (!locked.one_row()[0].as<bool>()) {
      return failure(target::ErrorCode::Conflict);
    }
    const auto existing = transaction.exec(
        "SELECT count(*) FROM pg_namespace WHERE nspname NOT IN ('public','information_schema') "
        "AND nspname NOT LIKE 'pg_%'");
    const auto public_objects = transaction.exec(
        "SELECT (SELECT count(*) FROM pg_class WHERE relnamespace='public'::regnamespace) + "
        "(SELECT count(*) FROM pg_proc WHERE pronamespace='public'::regnamespace) + "
        "(SELECT count(*) FROM pg_type WHERE typnamespace='public'::regnamespace)");
    if (existing.one_row()[0].as<int>() != 0 || public_objects.one_row()[0].as<int>() != 0) {
      return failure(target::ErrorCode::Conflict);
    }
    transaction.exec("REVOKE ALL ON SCHEMA public FROM PUBLIC");
    transaction.exec("REVOKE ALL ON SCHEMA public FROM " +
                     transaction.quote_name(config.runtime_role));
    transaction.exec(schema::kCreate);
    transaction.exec("INSERT INTO kiarabot.schema_versions VALUES(true,$1,$2)",
                     pqxx::params{schema::kVersion, service_name(config.service)});
    transaction.exec("GRANT USAGE ON SCHEMA kiarabot TO " +
                     transaction.quote_name(config.runtime_role));
    transaction.exec("GRANT SELECT ON kiarabot.schema_versions TO " +
                     transaction.quote_name(config.runtime_role));
    if (!schema_valid(transaction, config)) {
      return failure(target::ErrorCode::Internal);
    }
    try {
      transaction.commit();
    } catch (const pqxx::in_doubt_error&) {
      return target::Failure{target::ErrorCode::UpstreamUnavailable,
                             target::Acceptance::Unknown,
                             target::Recovery::Diagnose,
                             {},
                             {}};
    } catch (const pqxx::broken_connection&) {
      return target::Failure{target::ErrorCode::UpstreamUnavailable,
                             target::Acceptance::Unknown,
                             target::Recovery::Diagnose,
                             {},
                             {}};
    }
    return std::monostate{};
  } catch (...) {
    return translate_exception();
  }
}

Database::Database(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Database::~Database() = default;

core::target::Outcome<std::unique_ptr<Database>> Database::open(const DatabaseConfig& config) {
  if (!valid_config(config, false)) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  try {
    auto impl = std::make_unique<Impl>();
    impl->connection = connect(config);
    pqxx::work transaction(*impl->connection);
    if (!roles_valid(transaction, config)) {
      return failure(target::ErrorCode::PermissionDenied);
    }
    if (!schema_valid(transaction, config)) {
      return failure(target::ErrorCode::Internal);
    }
    const auto locked = transaction.exec("SELECT pg_try_advisory_lock($1,$2)",
                                         pqxx::params{kWriterLockClass, kWriterLockId});
    if (!locked.one_row()[0].as<bool>()) {
      return failure(target::ErrorCode::Conflict);
    }
    transaction.commit();
    return std::make_unique<Database>(std::move(impl));
  } catch (...) {
    return translate_exception();
  }
}

core::target::Status Database::check_writer() {
  if (impl_->disabled) {
    return failure(target::ErrorCode::UpstreamUnavailable);
  }
  try {
    pqxx::read_transaction transaction(*impl_->connection);
    const auto locks = transaction.exec(
        "SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND pid=pg_backend_pid() "
        "AND database=(SELECT oid FROM pg_database WHERE datname=current_database()) "
        "AND classid=$1::oid AND objid=$2::oid AND objsubid=2 AND granted",
        pqxx::params{kWriterLockClass, kWriterLockId});
    if (locks.one_row()[0].as<int>() != 1) {
      impl_->disabled = true;
      return failure(target::ErrorCode::UpstreamUnavailable);
    }
    transaction.commit();
    return std::monostate{};
  } catch (...) {
    impl_->disabled = true;
    return translate_exception();
  }
}

} // namespace kiarabot::infrastructure::postgres
