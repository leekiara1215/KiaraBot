#include "kiarabot/infrastructure/postgres/database.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>
#include <utility>

#include <pqxx/pqxx>
#include <pqxx/version>
#include <sys/stat.h>

#include "../schema/common.hpp"
#include "../schema/manifest.hpp"
#include "../schema/services.hpp"
#include "../schema/v1.hpp"
#include "session.hpp"

namespace kiarabot::infrastructure::postgres {
namespace {
namespace target = core::target;
static_assert(std::string_view(PQXX_VERSION) == "7.10.0");
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

class TrustFile final {
public:
  explicit TrustFile(const std::string& path)
      : descriptor_(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)) {}
  ~TrustFile() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }
  TrustFile(const TrustFile&) = delete;
  TrustFile& operator=(const TrustFile&) = delete;
  bool valid() const {
    struct stat metadata {};
    return descriptor_ >= 0 && ::fstat(descriptor_, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
           metadata.st_size > 0 && metadata.st_size <= 1024 * 1024;
  }

private:
  int descriptor_;
};

std::unique_ptr<pqxx::connection> connect(const DatabaseConfig& config) {
  // libpq's connect_timeout cannot bound opening a FIFO supplied as a trust store.
  // Reject special files without blocking before handing a regular PEM path to libpq.
  if (!TrustFile(config.root_certificate).valid()) {
    throw pqxx::broken_connection("database trust material unavailable");
  }
  auto connection = std::make_unique<pqxx::connection>(
      "host=" + quote_option(config.host) + " port=" + std::to_string(config.port) +
      " dbname=" + quote_option(config.database) + " user=" + quote_option(config.user) +
      " password=" + quote_option(config.password) +
      " sslrootcert=" + quote_option(config.root_certificate) +
      " sslmode=verify-full channel_binding=require gssencmode=disable connect_timeout=5 "
      "target_session_attrs=read-write"
      " options='-c statement_timeout=3000 -c lock_timeout=1000"
      " -c idle_in_transaction_session_timeout=5000 -c search_path=pg_catalog -c timezone=UTC -c "
      "client_encoding=UTF8'"
      " application_name=kiarabot");
  // Server notices are not an application diagnostic channel and may contain arbitrary text.
  connection->set_notice_handler([](pqxx::zview) {});
  if (connection->server_version() != 160004) {
    throw std::runtime_error("unsupported database version");
  }
  pqxx::read_transaction metadata(*connection);
  if (metadata.exec("SHOW server_encoding").one_row()[0].as<std::string>() != "UTF8") {
    throw std::runtime_error("unsupported database encoding");
  }
  metadata.commit();
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

bool layout_valid(pqxx::transaction_base& transaction, bool initialized) {
  const auto namespaces = transaction.exec(
      "SELECT nspname FROM pg_namespace WHERE nspname NOT IN ('public','information_schema') "
      "AND left(nspname,3)<>'pg_'");
  if (initialized ? namespaces.size() != 1 || namespaces[0][0].as<std::string>() != "kiarabot"
                  : !namespaces.empty()) {
    return false;
  }
  const auto public_objects = transaction.exec(
      "SELECT (SELECT count(*) FROM pg_class WHERE relnamespace='public'::regnamespace) + "
      "(SELECT count(*) FROM pg_proc WHERE pronamespace='public'::regnamespace) + "
      "(SELECT count(*) FROM pg_type WHERE typnamespace='public'::regnamespace)");
  return public_objects.one_row()[0].as<int>() == 0;
}

bool schema_valid(pqxx::transaction_base& transaction, const DatabaseConfig& config) {
  if (!layout_valid(transaction, true)) {
    return false;
  }
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
  const auto permissions = transaction.exec(
      "SELECT has_schema_privilege($1,'kiarabot','USAGE') AND "
      "NOT has_schema_privilege($1,'kiarabot','CREATE') AND "
      "NOT has_schema_privilege($1,'public','CREATE') AND "
      "has_table_privilege($1,'kiarabot.schema_versions','SELECT') AND "
      "NOT "
      "has_table_privilege($1,'kiarabot.schema_versions','INSERT,UPDATE,DELETE,"
      "TRUNCATE,REFERENCES,TRIGGER') AND NOT "
      "has_any_column_privilege($1,'kiarabot.schema_versions','INSERT,UPDATE,REFERENCES')",
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
  const auto descriptor = transaction.exec(schema::kDescriptorQuery);
  const auto owners = transaction.exec(
      "SELECT bool_and(pg_get_userbyid(c.relowner)=$1 AND c.relpersistence='p' AND NOT "
      "c.relrowsecurity AND NOT c.relforcerowsecurity) "
      "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='kiarabot'",
      pqxx::params{config.migration_role});
  const auto table_access = transaction.exec(
      "SELECT bool_and(has_table_privilege($1,c.oid,'SELECT') AND "
      "has_table_privilege($1,c.oid,'INSERT') "
      "AND has_table_privilege($1,c.oid,'UPDATE') AND has_table_privilege($1,c.oid,'DELETE') "
      "AND NOT has_table_privilege($1,c.oid,'TRUNCATE,REFERENCES,TRIGGER')) "
      "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='kiarabot' "
      "AND c.relkind='r' AND c.relname<>'schema_versions'",
      pqxx::params{config.runtime_role});
  const auto no_executable_objects =
      transaction.exec("SELECT (SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON "
                       "n.oid=p.pronamespace WHERE n.nspname='kiarabot') + "
                       "(SELECT count(*) FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid JOIN "
                       "pg_namespace n ON n.oid=c.relnamespace "
                       "WHERE n.nspname='kiarabot' AND NOT t.tgisinternal)");
  const auto objects = transaction.exec(
      "SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
      "WHERE n.nspname='kiarabot'");
  return records.size() == 1 && records[0][0].as<bool>() &&
         records[0][1].as<int>() == schema::version(config.service) &&
         records[0][2].as<std::string>() == service_name(config.service) &&
         permissions.one_row()[0].as<bool>() && schema_permissions.one_row()[0].as<bool>() &&
         objects.one_row()[0].as<int>() == schema::relation_count(config.service) &&
         descriptor.one_row()[0].as<std::string>() == schema::descriptor(config.service) &&
         owners.one_row()[0].as<bool>() && table_access.one_row()[0].as<bool>() &&
         no_executable_objects.one_row()[0].as<int>() == 0;
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
    if (state.starts_with("53") || state.starts_with("58") || state.starts_with("57")) {
      return failure(target::ErrorCode::UpstreamUnavailable);
    }
    return failure(target::ErrorCode::Internal);
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
} // namespace

struct Database::Impl {
  std::shared_ptr<detail::Session> session;
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
    if (!layout_valid(transaction, false)) {
      return failure(target::ErrorCode::Conflict);
    }
    transaction.exec("REVOKE ALL ON SCHEMA public FROM PUBLIC");
    transaction.exec("REVOKE ALL ON SCHEMA public FROM " +
                     transaction.quote_name(config.runtime_role));
    transaction.exec(schema::kCreate);
    transaction.exec(schema::common_sql());
    transaction.exec(schema::service_sql(config.service));
    transaction.exec("INSERT INTO kiarabot.schema_versions VALUES(true,$1,$2)",
                     pqxx::params{schema::version(config.service), service_name(config.service)});
    transaction.exec("GRANT USAGE ON SCHEMA kiarabot TO " +
                     transaction.quote_name(config.runtime_role));
    transaction.exec("GRANT SELECT,INSERT,UPDATE,DELETE ON ALL TABLES IN SCHEMA kiarabot TO " +
                     transaction.quote_name(config.runtime_role));
    transaction.exec("REVOKE INSERT,UPDATE,DELETE ON kiarabot.schema_versions FROM " +
                     transaction.quote_name(config.runtime_role));
    transaction.exec("GRANT USAGE ON ALL SEQUENCES IN SCHEMA kiarabot TO " +
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
    impl->session = std::make_shared<detail::Session>();
    impl->session->owner = config.service;
    impl->session->connection = connect(config);
    pqxx::work transaction(*impl->session->connection);
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
  auto begun = begin();
  if (const auto* rejected = std::get_if<target::Failure>(&begun)) {
    return *rejected;
  }
  auto work = std::move(std::get<std::unique_ptr<target::UnitOfWork>>(begun));
  auto sampled = work->current_time();
  work->rollback();
  if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
    return *rejected;
  }
  return std::monostate{};
}

core::target::Outcome<detail::CommonWorkAccess> detail::DatabaseAccess::begin(Database& database) {
  return begin_common_access(database.impl_->session);
}

core::target::Outcome<std::unique_ptr<core::target::UnitOfWork>> Database::begin() {
  return detail::begin_common(impl_->session);
}

} // namespace kiarabot::infrastructure::postgres
