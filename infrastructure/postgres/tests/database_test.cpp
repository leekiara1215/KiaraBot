#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <variant>

#include <pqxx/pqxx>
#include <sys/wait.h>

extern char** environ;

#include "common_work_checks.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"
#include "typed_work_checks.hpp"

namespace {
namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

template <typename Value>
void expect_error(const target::Outcome<Value>& result, target::ErrorCode code) {
  require(std::holds_alternative<target::Failure>(result), "expected failure");
  require(std::get<target::Failure>(result).code == code, "wrong stable failure code");
}

std::string quote(std::string_view value) {
  std::string output = "'";
  for (char character : value) {
    if (character == '\'' || character == '\\') {
      output += '\\';
    }
    output += character;
  }
  return output + '\'';
}

std::string connection_string(const pg::DatabaseConfig& config) {
  return "host=" + quote(config.host) + " port=" + std::to_string(config.port) +
         " dbname=" + quote(config.database) + " user=" + quote(config.user) +
         " password=" + quote(config.password) +
         " sslmode=verify-full channel_binding=require "
         "connect_timeout=3 sslrootcert=" +
         quote(config.root_certificate);
}

int initialize_process(const std::filesystem::path& executable, const pg::DatabaseConfig& config,
                       const std::filesystem::path& password_file) {
  const std::array<std::string, 10> arguments{
      executable.string(),
      "--initialize-empty",
      config.service == target::Service::Identity    ? "identity"
      : config.service == target::Service::Runtime   ? "runtime"
      : config.service == target::Service::Scheduler ? "scheduler"
                                                     : "model",
      config.host,
      std::to_string(config.port),
      config.database,
      config.migration_role,
      config.runtime_role,
      config.root_certificate,
      password_file.string()};
  std::array<char*, 11> argv{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    argv[index] = const_cast<char*>(arguments[index].c_str());
  }
  pid_t process = 0;
  require(::posix_spawn(&process, executable.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
          "initializer process spawn failed");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int status = 0;
  while (::waitpid(process, &status, WNOHANG) == 0) {
    if (std::chrono::steady_clock::now() > deadline) {
      ::kill(process, SIGKILL);
      ::waitpid(process, &status, 0);
      throw TestFailure("initializer process timeout");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(WIFEXITED(status), "initializer terminated abnormally");
  return WEXITSTATUS(status);
}

// All mutations below belong to the already verified disposable backend and roll back together.
void check_identity_security_schema(const pg::DatabaseConfig& config) {
  pqxx::connection connection(connection_string(config));
  pqxx::work transaction(connection);
  const auto rejects = [&](const std::string& sql, const std::string& state) {
    bool rejected = false;
    try {
      pqxx::subtransaction attempt(transaction);
      attempt.exec(sql);
      attempt.exec("SET CONSTRAINTS ALL IMMEDIATE");
      attempt.commit();
    } catch (const pqxx::sql_error& error) {
      rejected = error.sqlstate() == state;
    }
    require(rejected, "Identity security schema accepted invalid relation");
  };
  require(transaction
              .exec("SELECT state=0 AND version=1 AND administrator IS NULL AND "
                    "consumed_at IS NULL FROM kiarabot.administrator_bootstrap WHERE "
                    "singleton")
              .one_row()[0]
              .as<bool>(),
          "explicit initial bootstrap marker incorrect");
  transaction.exec("INSERT INTO kiarabot.principals VALUES('security-owner',0,NULL,NULL,1,0)");
  transaction.exec("INSERT INTO kiarabot.credential_heads VALUES('security-credential',"
                   "'security-owner',1,NULL)");
  transaction.exec("INSERT INTO kiarabot.credential_versions VALUES('security-credential',1,"
                   "'security-owner',0,0,137,NULL,NULL,15,'catalog')");
  transaction.exec("INSERT INTO kiarabot.credential_pending VALUES('security-credential',1,0,1)");
  transaction.exec("SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED");
  rejects("UPDATE kiarabot.credential_heads SET version=2 WHERE id='security-credential'", "23503");
  rejects("UPDATE kiarabot.credential_versions SET expires_at=731 WHERE "
          "id='security-credential'",
          "23514");
  rejects("INSERT INTO kiarabot.credential_passwords VALUES('security-credential',1,1,'phc',1)",
          "23503");
  transaction.exec("INSERT INTO kiarabot.credential_heads VALUES('security-certificate',"
                   "'security-owner',1,NULL)");
  transaction.exec("INSERT INTO kiarabot.credential_versions VALUES('security-certificate',1,"
                   "'security-owner',3,1,137,NULL,731,15,'catalog')");
  transaction.exec(
      "INSERT INTO kiarabot.credential_certificates VALUES('security-certificate',"
      "1,3,'security-owner',decode(repeat('ab',32),'hex'),'registered-san',0,100,1000)");
  transaction.exec("UPDATE kiarabot.credential_heads SET leaf_fingerprint=decode(repeat('ab',32),"
                   "'hex') WHERE id='security-certificate'");
  transaction.exec("SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED");
  rejects("UPDATE kiarabot.credential_heads SET leaf_fingerprint=decode(repeat('ac',32),'hex') "
          "WHERE id='security-certificate'",
          "23503");
  transaction.exec("INSERT INTO kiarabot.sessions VALUES('security-session','security-owner',1,"
                   "18446744073709551615,1000000,false)");
  transaction.exec(
      "INSERT INTO kiarabot.session_security VALUES('security-session',"
      "'security-owner','security-credential',1,'access',decode(repeat('00',32),'hex'),"
      "'refresh',decode(repeat('ff',32),'hex'),137,731,1000000,"
      "18446744073709551615,1,false)");
  transaction.exec("SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED");
  rejects("UPDATE kiarabot.sessions SET rotation=1 WHERE id='security-session'", "23503");
  rejects("UPDATE kiarabot.sessions SET expires_at=1000001 WHERE id='security-session'", "23503");
  rejects("UPDATE kiarabot.sessions SET revoked=true WHERE id='security-session'", "23503");
  rejects("UPDATE kiarabot.session_security SET version=2 WHERE id='security-session'", "23503");
  transaction.exec("INSERT INTO kiarabot.grants VALUES('security-grant','security-owner',NULL,1)");
  transaction.exec("INSERT INTO kiarabot.exact_assignments VALUES('security-grant',"
                   "'security-owner',1)");
  transaction.exec(
      "INSERT INTO kiarabot.exact_permissions VALUES('security-grant',40,15,'catalog')");
  transaction.exec("SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED");
  rejects("UPDATE kiarabot.grants SET version=2 WHERE id='security-grant'", "23503");
  rejects("INSERT INTO kiarabot.exact_permissions VALUES('security-grant',41,15,'catalog')",
          "23514");
  transaction.exec(
      "INSERT INTO kiarabot.secret_receipts VALUES(1,'control-peer','security-session',"
      "'receipt','security-owner','security-session','security-credential',1,1,2,"
      "'commitment',decode(repeat('00',32),'hex'),137,731,'sealed',1,"
      "decode('00','hex'),decode('00ff','hex'))");
  require(transaction
              .exec("SELECT created_at=137 AND retain_until=731 AND "
                    "octet_length(ciphertext)=2 FROM kiarabot.secret_receipts")
              .one_row()[0]
              .as<bool>(),
          "AEAD original nanoseconds/binary bytes changed");
  rejects("UPDATE kiarabot.secret_receipts SET retain_until=86400000000138", "23514");
  rejects("UPDATE kiarabot.secret_receipts SET result_rotation=1", "23514");
  transaction.exec("INSERT INTO kiarabot.consumed_refresh VALUES('refresh',"
                   "decode(repeat('ff',32),'hex'),'security-session','security-owner',"
                   "'security-credential',1,1,1000,1,'control-peer','receipt',137,731)");
  transaction.exec("SET CONSTRAINTS ALL IMMEDIATE; SET CONSTRAINTS ALL DEFERRED");
  rejects("UPDATE kiarabot.consumed_refresh SET control_peer='other-peer'", "23503");
  rejects("UPDATE kiarabot.consumed_refresh SET recovery_until=1001", "23514");
  rejects("UPDATE kiarabot.administrator_bootstrap SET state=1", "23514");
  transaction.exec("DELETE FROM kiarabot.secret_receipts");
  require(
      transaction.exec("SELECT count(*) FROM kiarabot.consumed_refresh").one_row()[0].as<int>() ==
          0,
      "receipt cleanup retained orphan consumed verifier");
  // No test data, verifiers or grant state escape this transaction.
}

void run(const std::filesystem::path& root, std::uint16_t port,
         const std::filesystem::path& initializer, std::uint16_t proxy_port) {
  require(root.filename().string().starts_with("kiarabot-p03-"), "non-fixture root");
  require(std::filesystem::is_regular_file(root / "p03-owned-marker"), "missing fixture marker");
  std::ifstream password_file(root / "password");
  std::string password;
  std::getline(password_file, password);
  require(password.size() == 64, "invalid fixture secret");
  const auto role_password = [&](const std::string& role) {
    std::ifstream input(root / ("password-" + role));
    std::string value;
    std::getline(input, value);
    require(value.size() == 64 && value != password, "distinct role fixture secret required");
    return value;
  };
  pg::DatabaseConfig admin{
      target::Service::Runtime,       "localhost", port, "postgres", "p03_admin", password,
      (root / "server.pem").string(), "",          ""};
  pqxx::connection administrator(connection_string(admin));
  require(administrator.server_version() == 160004, "wrong server version");
  {
    pqxx::read_transaction identity(administrator);
    const auto actual = std::filesystem::canonical(
        identity.exec("SHOW data_directory").one_row()[0].as<std::string>());
    require(!std::filesystem::is_symlink(root / "data") &&
                actual == std::filesystem::canonical(root / "data") && actual.parent_path() == root,
            "fixture backend directory mismatch");
  }
  std::array services{target::Service::Identity, target::Service::Runtime,
                      target::Service::Scheduler, target::Service::Model};
  std::array<std::string, 4> names{"identity", "runtime", "scheduler", "model"};
  std::array<pg::DatabaseConfig, 4> configs;
  for (std::size_t index = 0; index < services.size(); ++index) {
    const auto migration_role = "p03_" + names[index] + "_migration";
    const auto runtime_role = "p03_" + names[index] + "_runtime";
    const auto database = "p03_" + names[index];
    pqxx::nontransaction deployment(administrator);
    for (const auto& role : {migration_role, runtime_role}) {
      deployment.exec("CREATE ROLE " + deployment.quote_name(role) +
                      " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                      "NOBYPASSRLS PASSWORD " +
                      deployment.quote(role_password(role)));
    }
    deployment.exec("CREATE DATABASE " + deployment.quote_name(database) + " OWNER " +
                    deployment.quote_name(migration_role));
    deployment.exec("REVOKE ALL ON DATABASE " + deployment.quote_name(database) + " FROM PUBLIC");
    deployment.exec("GRANT CONNECT ON DATABASE " + deployment.quote_name(database) + " TO " +
                    deployment.quote_name(runtime_role));
    configs[index] = {services[index],
                      "localhost",
                      port,
                      database,
                      migration_role,
                      role_password(migration_role),
                      (root / "server.pem").string(),
                      migration_role,
                      runtime_role};
  }
  for (auto config : configs) {
    auto runtime = config;
    runtime.user = config.runtime_role;
    runtime.password = role_password(config.runtime_role);
    require(runtime.password != config.password, "runtime reused migration secret");
    expect_error(pg::Database::open(runtime), target::ErrorCode::Internal);
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work nonempty(migration);
      nonempty.exec("CREATE SCHEMA pga"); // SQL LIKE 'pg_%' would incorrectly hide this schema.
      nonempty.commit();
    }
    expect_error(pg::initialize_empty(config), target::ErrorCode::Conflict);
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work cleanup(migration);
      cleanup.exec("DROP SCHEMA pga");
      cleanup.commit();
    }
    // Real PostgreSQL DDL failure after CREATE SCHEMA must roll the entire initializer back.
    // Fault injection objects live in pg_catalog, owned only by the disposable test superuser.
    auto fault_admin = config;
    fault_admin.user = "p03_admin";
    fault_admin.password = password;
    {
      pqxx::connection injector(connection_string(fault_admin));
      pqxx::work fault(injector);
      fault.exec("CREATE FUNCTION pg_catalog.p03_fail_table() RETURNS event_trigger "
                 "LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'fixture injection'; END $$");
      fault.exec("CREATE EVENT TRIGGER p03_fail_table ON ddl_command_start "
                 "WHEN TAG IN ('CREATE TABLE') EXECUTE FUNCTION pg_catalog.p03_fail_table()");
      fault.commit();
    }
    expect_error(pg::initialize_empty(config), target::ErrorCode::Internal);
    {
      pqxx::connection injector(connection_string(fault_admin));
      pqxx::work inspect(injector);
      require(inspect.exec("SELECT count(*) FROM pg_namespace WHERE nspname='kiarabot'")
                      .one_row()[0]
                      .as<int>() == 0,
              "failed initialization left partial schema");
      inspect.exec("DROP EVENT TRIGGER p03_fail_table");
      inspect.exec("DROP FUNCTION pg_catalog.p03_fail_table()");
      inspect.commit();
    }
    require(initialize_process(initializer, config, root / ("password-" + config.migration_role)) ==
                0,
            "explicit initialization failed");
    require(initialize_process(initializer, config, root / ("password-" + config.migration_role)) ==
                1,
            "repeated CLI initialization was not rejected");
    expect_error(pg::initialize_empty(config), target::ErrorCode::Conflict);
    {
      pqxx::nontransaction readonly(administrator);
      readonly.exec("ALTER DATABASE " + readonly.quote_name(config.database) +
                    " SET default_transaction_read_only=on");
    }
    expect_error(pg::Database::open(runtime), target::ErrorCode::UpstreamUnavailable);
    {
      pqxx::nontransaction writable(administrator);
      writable.exec("ALTER DATABASE " + writable.quote_name(config.database) +
                    " RESET default_transaction_read_only");
    }
    {
      pqxx::connection migration(connection_string(config));
      pqxx::read_transaction inspect(migration);
      require(inspect.exec("SELECT version FROM kiarabot.schema_versions").one_row()[0].as<int>() ==
                  (config.service == target::Service::Identity ? 5 : 4),
              "per-service schema version incorrect");
    }
    if (config.service == target::Service::Identity) {
      check_identity_security_schema(config);
    }
    pg::test::check_typed(runtime);
    if (config.service == target::Service::Identity) {
      pg::test::check_unknown_commit(runtime, root, proxy_port);
    }
    auto opened = pg::Database::open(runtime);
    require(std::holds_alternative<std::unique_ptr<pg::Database>>(opened),
            "runtime validation failed");
    auto database = std::move(std::get<std::unique_ptr<pg::Database>>(opened));
    require(std::holds_alternative<std::monostate>(database->check_writer()),
            "writer lock missing");
    pg::test::check_common(*database, config);
    expect_error(pg::Database::open(runtime), target::ErrorCode::Conflict);
    database.reset();
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work extra(migration);
      extra.exec("CREATE SCHEMA pga");
      extra.commit();
    }
    expect_error(pg::Database::open(runtime), target::ErrorCode::Internal);
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work cleanup(migration);
      cleanup.exec("DROP SCHEMA pga");
      cleanup.commit();
    }
    auto validated = pg::Database::open(runtime);
    require(std::holds_alternative<std::unique_ptr<pg::Database>>(validated),
            "revalidation failed");
    database = std::move(std::get<std::unique_ptr<pg::Database>>(validated));
    {
      auto impersonated = runtime;
      impersonated.user = config.migration_role;
      bool denied_role_switch = false;
      try {
        pqxx::connection forbidden(connection_string(impersonated));
      } catch (const pqxx::broken_connection&) {
        denied_role_switch = true;
      }
      require(denied_role_switch, "runtime secret authenticated as migration identity");
      pqxx::connection restricted(connection_string(runtime));
      for (const auto& sql : {"CREATE TABLE kiarabot.forbidden(id integer)",
                              "CREATE TABLE public.forbidden(id integer)",
                              "CREATE SCHEMA forbidden", "CREATE TEMP TABLE forbidden(id integer)",
                              "UPDATE kiarabot.schema_versions SET version=2"}) {
        bool denied = false;
        try {
          pqxx::work transaction(restricted);
          transaction.exec(sql);
          transaction.commit();
        } catch (const pqxx::insufficient_privilege&) {
          denied = true;
        }
        require(denied, "runtime DDL/metadata mutation was allowed");
      }
    }
    for (const auto& other : configs) {
      if (other.database == config.database) {
        continue;
      }
      for (const auto& user : {config.migration_role, config.runtime_role}) {
        auto cross = config;
        cross.database = other.database;
        cross.user = user;
        cross.password = role_password(user);
        bool denied = false;
        try {
          pqxx::connection forbidden(connection_string(cross));
        } catch (const pqxx::broken_connection&) {
          denied = true;
        }
        require(denied, "cross database connection allowed");
      }
    }
    database.reset();
    auto restarted = pg::Database::open(runtime);
    require(std::holds_alternative<std::unique_ptr<pg::Database>>(restarted),
            "restart lock not released");
    auto writer = std::move(std::get<std::unique_ptr<pg::Database>>(restarted));
    auto active_result = writer->begin();
    require(std::holds_alternative<std::unique_ptr<target::UnitOfWork>>(active_result),
            "old active UoW begin failed");
    auto active = std::move(std::get<std::unique_ptr<target::UnitOfWork>>(active_result));
    target::Operation uncommitted{
        {config.service, {"killed-write"}, {target::ResourceKind::Instance, {"resource"}}},
        {1},
        target::OperationState::Accepted,
        target::ExecutionOutcome::NotStarted,
        {},
        {}};
    require(std::holds_alternative<std::monostate>(active->operations().save(uncommitted, {})),
            "old active write failed");
    {
      pqxx::work terminate(administrator);
      const auto killed = terminate.exec(
          "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname=$1 AND usename=$2",
          pqxx::params{config.database, config.runtime_role});
      require(killed.size() == 1, "unexpected writer sessions");
      terminate.commit();
    }
    require(active->commit() != target::CommitResult::Committed,
            "killed writer committed old transaction");
    expect_error(writer->check_writer(), target::ErrorCode::UpstreamUnavailable);
    expect_error(writer->check_writer(), target::ErrorCode::UpstreamUnavailable);
    writer.reset();
    require(std::holds_alternative<std::unique_ptr<pg::Database>>(pg::Database::open(runtime)),
            "new writer could not recover after terminated session");
    pg::test::check_dependency_fault(config, runtime);
    pg::test::check_deadline(runtime);
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work altered(migration);
      altered.exec("DROP INDEX kiarabot.outbox_due");
      altered.exec("CREATE INDEX outbox_due ON kiarabot.outbox(next_attempt_at)");
      altered.commit();
    }
    expect_error(pg::Database::open(runtime), target::ErrorCode::Internal);
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work restore(migration);
      restore.exec("DROP INDEX kiarabot.outbox_due");
      restore.exec("CREATE INDEX outbox_due ON kiarabot.outbox(state,next_attempt_at,lease_until)");
      restore.commit();
    }
    require(std::holds_alternative<std::unique_ptr<pg::Database>>(pg::Database::open(runtime)),
            "restored schema did not validate");
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work corrupt(migration);
      corrupt.exec("UPDATE kiarabot.schema_versions SET version=999");
      corrupt.commit();
    }
    expect_error(pg::Database::open(runtime), target::ErrorCode::Internal);
    {
      pqxx::connection migration(connection_string(config));
      pqxx::work partial(migration);
      partial.exec("DELETE FROM kiarabot.schema_versions");
      partial.commit();
    }
    expect_error(pg::Database::open(runtime), target::ErrorCode::Internal);
    expect_error(pg::initialize_empty(config), target::ErrorCode::Conflict);
  }
  auto invalid = configs[0];
  invalid.user = invalid.runtime_role;
  invalid.password = role_password(invalid.runtime_role);
  invalid.host = "/tmp";
  expect_error(pg::Database::open(invalid), target::ErrorCode::InvalidArgument);
  invalid.host = "127.0.0.1"; // Certificate has DNS:localhost only.
  expect_error(pg::Database::open(invalid), target::ErrorCode::UpstreamUnavailable);
  invalid.host = "localhost";
  invalid.root_certificate = (root / "password-fifo").string();
  const auto trust_start = std::chrono::steady_clock::now();
  expect_error(pg::Database::open(invalid), target::ErrorCode::UpstreamUnavailable);
  require(std::chrono::steady_clock::now() - trust_start < std::chrono::seconds(1),
          "FIFO trust store blocked connection");
  invalid.root_certificate = (root / "missing-ca.pem").string();
  expect_error(pg::Database::open(invalid), target::ErrorCode::UpstreamUnavailable);
  std::cout << "P03 storage foundation: four private databases, TLS, permissions, schema, locks, "
               "restart passed\n";
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 5, "fixture arguments required");
    const auto number = std::stoul(argv[2]);
    require(number > 0 && number <= 65535, "invalid fixture port");
    const auto proxy_number = std::stoul(argv[4]);
    require(proxy_number > 0 && proxy_number <= 65535, "invalid proxy port");
    run(std::filesystem::canonical(argv[1]), static_cast<std::uint16_t>(number),
        std::filesystem::canonical(argv[3]), static_cast<std::uint16_t>(proxy_number));
    return 0;
  } catch (const pqxx::sql_error& error) {
    std::cerr << "P03 assertion: unexpected test SQLSTATE " << error.sqlstate() << '\n';
    return 1;
  } catch (const TestFailure& error) {
    std::cerr << "P03 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    // Library/SQL exception text may contain secrets. Assertions identify fixed test stages only.
    std::cerr << "P03 storage foundation test failed (details redacted)\n";
    return 1;
  }
}
