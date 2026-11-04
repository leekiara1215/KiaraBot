#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <source_location>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pqxx/pqxx>
#include <sys/wait.h>

#include "../schema/common.hpp"
#include "../schema/manifest.hpp"
#include "../schema/services.hpp"
#include "../schema/v1.hpp"
#include "../src/operation_observations.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

extern char** environ;

namespace pg = kiarabot::infrastructure::postgres;
namespace t = kiarabot::core::target;

namespace {
class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
const char* phase = "setup";
t::Service active_owner = t::Service::Runtime;

void require(bool condition, const char* reason) {
  if (!condition) {
    throw TestFailure(reason);
  }
}
template <typename Value>
Value take(t::Outcome<Value> result,
           const std::source_location location = std::source_location::current()) {
  if (const auto* failure = std::get_if<t::Failure>(&result)) {
    std::cerr << "P03 assertion: observation stable code " << static_cast<int>(failure->code)
              << " at line " << location.line() << '\n';
    throw TestFailure("unexpected observation storage failure");
  }
  return std::move(std::get<Value>(result));
}
template <typename Value> void expect(const t::Outcome<Value>& result, t::ErrorCode code) {
  require(std::holds_alternative<t::Failure>(result) && std::get<t::Failure>(result).code == code,
          "wrong observation failure");
}
std::string secret(const std::filesystem::path& file) {
  std::ifstream input(file);
  std::string value;
  std::getline(input, value);
  require(value.size() == 64, "fixture secret unavailable");
  return value;
}
std::string quote(std::string_view value) {
  std::string result = "'";
  for (const char character : value) {
    if (character == '\'' || character == '\\') {
      result += '\\';
    }
    result += character;
  }
  return result + '\'';
}
std::string connection_string(const pg::DatabaseConfig& config) {
  return "host=localhost port=" + std::to_string(config.port) +
         " dbname=" + quote(config.database) + " user=" + quote(config.user) +
         " password=" + quote(config.password) +
         " sslmode=verify-full channel_binding=require sslrootcert=" +
         quote(config.root_certificate);
}
pg::DatabaseConfig configuration(const std::filesystem::path& root, std::uint16_t port,
                                 t::Service owner, const std::string& name, bool migration) {
  const auto prefix = "p03_" + name;
  const auto role = prefix + (migration ? "_migration" : "_runtime");
  return {owner,
          "localhost",
          port,
          prefix,
          role,
          secret(root / ("password-" + role)),
          (root / "server.pem").string(),
          prefix + "_migration",
          prefix + "_runtime"};
}
void spawn(const std::filesystem::path& program, const std::vector<std::string>& arguments) {
  std::vector<char*> argv;
  for (const auto& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  pid_t child = 0;
  require(::posix_spawn(&child, program.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
          "owned child spawn failed");
  int status = 0;
  require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "owned child rejected");
}
t::ResourceKind kind(t::Service owner) {
  return owner == t::Service::Model       ? t::ResourceKind::Model
         : owner == t::Service::Scheduler ? t::ResourceKind::Task
                                          : t::ResourceKind::Account;
}
t::Operation operation(t::Service owner, const std::string& id,
                       std::string resource = "resource-a") {
  t::Operation result;
  result.ref = {owner, {id}, {kind(owner), {std::move(resource)}}};
  result.version = {1};
  return result;
}
template <typename Factory>
t::OperationObservationPage query(Factory& factory, t::OperationObservationQuery input) {
  input.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  auto work = take(factory.begin());
  auto result = take(work->query_operation_changes(input));
  work->rollback();
  return result;
}
template <typename Factory>
void rejected_query(Factory& factory, t::OperationObservationQuery input, t::ErrorCode code) {
  input.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  auto work = take(factory.begin());
  expect(work->query_operation_changes(input), code);
  work->rollback();
}
template <typename Factory>
t::OperationObservationCursor suite(Factory& factory, const pg::DatabaseConfig& config,
                                    const pg::DatabaseConfig& migration) {
  phase = "head-and-final-snapshot";
  t::OperationObservationQuery input;
  input.scope = {{kind(config.service), {"resource-a"}}};
  const auto initial = query(factory, input);
  require(initial.changes.empty() && initial.next_cursor.sequence == 0 &&
              !initial.next_cursor.epoch.empty(),
          "missing cursor must capture committed head only");
  auto first = operation(config.service, "operation-a");
  std::vector<t::OperationParticipant> members{{"host", t::OperationState::Running, {}, {}}};
  {
    auto work = take(factory.begin());
    take(work->operations().save(first, {}));
    take(work->save_operation_participants(first.ref.id, members));
    members[0].state = t::OperationState::Blocked;
    members[0].reason = t::ErrorCode::UpstreamUnavailable;
    take(work->save_operation_participants(first.ref.id, members));
    require(work->commit() == t::CommitResult::Committed, "final snapshot commit failed");
  }
  input.cursor = initial.next_cursor;
  auto page = query(factory, input);
  require(page.changes.size() == 1 && page.changes[0].operation.participants.size() == 1 &&
              page.changes[0].operation.participants[0].state == t::OperationState::Blocked &&
              page.next_cursor.sequence == 1,
          "one UoW must publish one final operation and participant snapshot");
  {
    auto work = take(factory.begin());
    take(work->save_operation_participants(first.ref.id, members));
    require(work->commit() == t::CommitResult::Committed, "no-op participant commit failed");
  }
  require(query(factory, input).changes.size() == 1, "no-op participant write emitted event");
  {
    auto work = take(factory.begin());
    take(work->operations().save(operation(config.service, "rolled-back"), {}));
    work->rollback();
  }
  require(query(factory, input).next_cursor.sequence == 1, "rollback advanced journal head");
  {
    auto work = take(factory.begin());
    const auto now = take(work->current_time());
    const t::ReceiptKey key{{"subject"}, "observation-test", first.ref.resource, "receipt-key"};
    const t::CommandReceipt receipt{key, {}, first, now, now + std::chrono::hours{24 * 7}};
    take(work->receipts().insert(receipt, {8}));
    require(work->commit() == t::CommitResult::Committed, "receipt snapshot commit failed");
    auto replay = take(factory.begin());
    require(take(replay->receipts().find(key)).has_value(), "receipt snapshot missing");
    take(replay->receipts().insert(receipt, {8}));
    require(replay->commit() == t::CommitResult::Committed, "receipt replay failed");
  }
  require(query(factory, input).next_cursor.sequence == 1,
          "receipt operation snapshots/replay must not emit live changes");
  {
    auto work = take(factory.begin());
    members[0].state = t::OperationState::Succeeded;
    members[0].reason.reset();
    take(work->save_operation_participants(first.ref.id, members));
    require(work->commit() == t::CommitResult::Committed, "participant-only commit failed");
  }
  page = query(factory, input);
  require(page.changes.size() == 2 && page.changes.back().operation.version.value == 1 &&
              page.changes.back().operation.participants[0].state == t::OperationState::Succeeded,
          "participant-only change must preserve operation version and publish progress");
  {
    auto work = take(factory.begin());
    for (const auto* id : {"operation-b", "operation-c", "operation-d"}) {
      take(work->operations().save(
          operation(config.service, id,
                    id == std::string{"operation-b"} ? "resource-b" : "resource-a"),
          {}));
    }
    require(work->commit() == t::CommitResult::Committed, "multi-operation commit failed");
  }
  auto paged = input;
  paged.page_size = 1;
  std::set<std::uint64_t> sequences;
  for (;;) {
    const auto current = query(factory, paged);
    require(current.changes.size() == 1, "one-entry page expected");
    require(sequences.insert(current.changes[0].cursor.sequence).second, "duplicate cursor");
    require(current.changes[0].operation.ref.resource == input.scope[0], "scope leaked row");
    require(current.changes[0].cursor.sequence > paged.cursor->sequence, "cursor moved backward");
    paged.cursor = current.next_cursor;
    if (!current.has_more) {
      break;
    }
  }
  require(sequences.size() == 4 && paged.cursor->sequence == 5, "paging skipped matching row");
  auto invalid = input;
  invalid.scope.clear();
  rejected_query(factory, invalid, t::ErrorCode::InvalidArgument);
  invalid = input;
  invalid.scope.push_back(input.scope[0]);
  rejected_query(factory, invalid, t::ErrorCode::InvalidArgument);
  invalid = input;
  invalid.scope[0].kind = t::ResourceKind::Catalog;
  rejected_query(factory, invalid, t::ErrorCode::InvalidArgument);
  invalid = input;
  invalid.page_size = 101;
  rejected_query(factory, invalid, t::ErrorCode::InvalidArgument);
  invalid = input;
  invalid.cursor->sequence = 6;
  rejected_query(factory, invalid, t::ErrorCode::Conflict);
  invalid = input;
  invalid.cursor->owner = t::Service::Identity;
  rejected_query(factory, invalid, t::ErrorCode::InvalidArgument);
  invalid = input;
  invalid.cursor->scope[0].id.value = "resource-b";
  rejected_query(factory, invalid, t::ErrorCode::InvalidArgument);
  {
    auto work = take(factory.begin());
    auto timed = input;
    timed.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
    expect(work->query_operation_changes(timed), t::ErrorCode::Timeout);
    work->rollback();
  }
  phase = "flush-rollback";
  pqxx::connection admin(connection_string(migration));
  {
    const auto before = query(factory, input).next_cursor.sequence;
    pqxx::work held(admin);
    held.exec("SELECT singleton FROM kiarabot.operation_observation_head FOR UPDATE");
    auto work = take(factory.begin());
    take(work->operations().save(operation(config.service, "flush-rollback"), {}));
    require(work->commit() == t::CommitResult::RolledBack,
            "journal lock failure must roll back business mutation");
    held.abort();
    auto inspect = take(factory.begin());
    require(!take(inspect->operations().find({"flush-rollback"})),
            "journal failure committed live operation");
    inspect->rollback();
    require(query(factory, input).next_cursor.sequence == before,
            "failed flush advanced journal head");
  }
  {
    auto work = take(factory.begin());
    phase = "independent-live-retention";
    auto expired = operation(config.service, "expired-live");
    expired.state = t::OperationState::Succeeded;
    expired.execution_outcome = t::ExecutionOutcome::KnownSucceeded;
    expired.retain_until = take(work->current_time());
    take(work->operations().save(expired, {}));
    require(work->commit() == t::CommitResult::Committed, "expired live commit failed");
    auto cleanup = take(factory.begin());
    if (config.service == t::Service::Model) {
      // Model has never exposed the older lifecycle pruning port. Keep that
      // fail-closed behavior and exercise physical cleanup in the owned DB only.
      expect(cleanup->prune_operations({}, 1000), t::ErrorCode::CapabilityNotSupported);
      cleanup->rollback();
      pqxx::work owned_cleanup(admin);
      require(owned_cleanup
                      .exec("DELETE FROM kiarabot.operations WHERE id='expired-live' "
                            "AND retain_until<=(extract(epoch FROM clock_timestamp())*1000000000)"
                            "::bigint RETURNING id")
                      .size() == 1,
              "owned Model live cleanup did not remove expired row");
      owned_cleanup.commit();
    } else {
      require(take(cleanup->prune_operations({}, 1000)) == 1, "live operation not pruned");
      require(cleanup->commit() == t::CommitResult::Committed, "live prune commit failed");
    }
    const auto retained = query(factory, input);
    require(retained.changes.back().operation.ref.id.value == "expired-live",
            "live cleanup destroyed independent observation snapshot");
  }
  {
    phase = "retention-capacity-and-epoch";
    pqxx::work policy(admin);
    policy.exec("UPDATE kiarabot.operation_observation_head SET max_events=2 WHERE singleton");
    policy.commit();
    auto work = take(factory.begin());
    take(work->operations().save(operation(config.service, "capacity"), {}));
    require(work->commit() == t::CommitResult::Committed, "capacity transaction failed");
    pqxx::read_transaction inspect(admin);
    require(inspect.exec("SELECT count(*) FROM kiarabot.operation_observations")
                    .one_row()[0]
                    .as<int>() == 2,
            "event capacity did not evict oldest prefix");
  }
  rejected_query(factory, input, t::ErrorCode::Conflict);
  input.cursor.reset();
  const auto before_expiry = query(factory, input);
  {
    pqxx::work expiry(admin);
    expiry.exec("UPDATE kiarabot.operation_observations SET observed_at="
                "(extract(epoch FROM clock_timestamp())*1000000000)::bigint-601000000000");
    expiry.commit();
  }
  invalid = input;
  invalid.cursor = before_expiry.next_cursor;
  --invalid.cursor->sequence;
  rejected_query(factory, invalid, t::ErrorCode::Conflict);
  {
    auto work = take(factory.begin());
    require(take(work->prune_operation_changes(1000)) == 2, "expired prefix not pruned");
    require(work->commit() == t::CommitResult::Committed, "expiry maintenance failed");
  }
  {
    pqxx::work policy(admin);
    policy.exec("UPDATE kiarabot.operation_observation_head SET max_events=65536,max_bytes=1024 "
                "WHERE singleton");
    policy.commit();
    auto work = take(factory.begin());
    take(work->operations().save(operation(config.service, "bytes-a"), {}));
    take(work->operations().save(operation(config.service, "bytes-b"), {}));
    require(work->commit() == t::CommitResult::Committed, "byte capacity transaction failed");
    pqxx::read_transaction inspect(admin);
    require(inspect
                .exec("SELECT sum(encoded_bytes)<=1024 AND count(*)=1 "
                      "FROM kiarabot.operation_observations")
                .one_row()[0]
                .as<bool>(),
            "byte capacity did not evict prefix");
  }
  const auto old_dataset = query(factory, input).next_cursor;
  {
    pqxx::work rotate(admin);
    rotate.exec("UPDATE kiarabot.operation_observation_head SET "
                "epoch=pg_catalog.gen_random_uuid()::text,max_bytes=67108864 WHERE singleton");
    rotate.commit();
  }
  invalid = input;
  invalid.cursor = old_dataset;
  rejected_query(factory, invalid, t::ErrorCode::Conflict);
  const auto current = query(factory, input);
  require(current.next_cursor.epoch != old_dataset.epoch, "dataset epoch did not rotate");
  return current.next_cursor;
}

class BorrowedSql final : public pg::detail::SqlWork {
public:
  explicit BorrowedSql(pqxx::work& transaction) : transaction_(transaction) {}
  t::Status run(const std::function<t::Status(pqxx::transaction_base&)>& callback) override {
    return callback(transaction_);
  }

private:
  pqxx::work& transaction_;
};
void ordering(const pg::DatabaseConfig& migration) {
  pqxx::connection first(connection_string(migration));
  pqxx::connection observer(connection_string(migration));
  std::uint64_t before = 0;
  {
    pqxx::read_transaction inspect(observer);
    before = inspect.exec("SELECT sequence FROM kiarabot.operation_observation_head")
                 .one_row()[0]
                 .as<std::uint64_t>();
  }
  const auto insert = [&](pqxx::work& tx, const char* id) {
    tx.exec("INSERT INTO kiarabot.operations(id,owner,resource_kind,resource_id,version,state,"
            "execution_outcome) VALUES($1,$2,$3,'ordering-resource',1,0,0)",
            pqxx::params{id, static_cast<int>(migration.service),
                         static_cast<int>(kind(migration.service))});
    BorrowedSql work(tx);
    take(pg::detail::flush_operation_observations(work, migration.service, {id}));
  };
  pqxx::work pending(first);
  insert(pending, "ordering-a");
  std::promise<int> backend;
  auto process = backend.get_future();
  auto second = std::async(std::launch::async, [&] {
    pqxx::connection connection(connection_string(migration));
    pqxx::work tx(connection);
    tx.exec("SET LOCAL lock_timeout='2000ms'");
    backend.set_value(tx.exec("SELECT pg_backend_pid()").one_row()[0].as<int>());
    insert(tx, "ordering-b");
    tx.commit();
  });
  const auto pid = process.get();
  bool blocked = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (!blocked && std::chrono::steady_clock::now() < deadline) {
    pqxx::read_transaction inspect(observer);
    blocked = inspect
                  .exec("SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid=$1 AND NOT granted)",
                        pqxx::params{pid})
                  .one_row()[0]
                  .as<bool>();
    require(inspect.exec("SELECT sequence FROM kiarabot.operation_observation_head")
                    .one_row()[0]
                    .as<std::uint64_t>() == before,
            "uncommitted append advanced visible head");
    if (!blocked) {
      std::this_thread::yield();
    }
  }
  // Release A even on a failed assertion, so the test cannot hang a waiting child.
  pending.commit();
  second.get();
  require(blocked, "second append never reached the real PG head lock barrier");
  pqxx::read_transaction inspect(observer);
  const auto rows = inspect.exec("SELECT id,sequence FROM kiarabot.operation_observations "
                                 "WHERE id IN ('ordering-a','ordering-b') ORDER BY sequence");
  require(rows.size() == 2 && rows[0][0].as<std::string>() == "ordering-a" &&
              rows[0][1].as<std::uint64_t>() == before + 1 &&
              rows[1][0].as<std::string>() == "ordering-b" &&
              rows[1][1].as<std::uint64_t>() == before + 2,
          "journal sequence is not committed append order");
}

void uint64_boundary(const pg::DatabaseConfig& runtime, const pg::DatabaseConfig& migration) {
  auto database = take(pg::Database::open(runtime));
  {
    pqxx::connection connection(connection_string(migration));
    pqxx::work set_head(connection);
    set_head.exec("UPDATE kiarabot.operation_observation_head SET sequence=$1 WHERE singleton",
                  pqxx::params{std::numeric_limits<std::uint64_t>::max() - 1});
    set_head.commit();
  }
  auto work = take(database->begin());
  take(work->operations().save(operation(runtime.service, "last-sequence"), {}));
  require(work->commit() == t::CommitResult::Committed, "uint64 maximum append failed");
  t::OperationObservationQuery input;
  input.scope = {{kind(runtime.service), {"resource-a"}}};
  const auto checkpoint = query(*database, input).next_cursor;
  require(checkpoint.sequence == std::numeric_limits<std::uint64_t>::max(),
          "journal truncated unsigned sequence");
  auto overflow = take(database->begin());
  take(overflow->operations().save(operation(runtime.service, "overflow"), {}));
  require(overflow->commit() == t::CommitResult::RolledBack, "overflow did not roll back");
  auto inspect = take(database->begin());
  require(!take(inspect->operations().find({"overflow"})), "overflow left a business row");
  inspect->rollback();
  require(query(*database, input).next_cursor.sequence == checkpoint.sequence,
          "overflow wrapped journal head");
}

void run(const std::filesystem::path& root, std::uint16_t port,
         const std::filesystem::path& initializer, const std::filesystem::path& self,
         const std::string& proxy, bool describe, const std::vector<std::string>& verify) {
  require(root.filename().string().starts_with("kiarabot-p03-") &&
              std::filesystem::is_regular_file(root / "p03-owned-marker"),
          "non-owned fixture");
  pg::DatabaseConfig administrator{t::Service::Runtime,
                                   "localhost",
                                   port,
                                   "postgres",
                                   "p03_admin",
                                   secret(root / "password"),
                                   (root / "server.pem").string(),
                                   {},
                                   {}};
  pqxx::connection deployment(connection_string(administrator));
  require(deployment.server_version() == 160004, "PG16.4 required");
  {
    pqxx::read_transaction identity(deployment);
    const auto actual = std::filesystem::canonical(
        identity.exec("SHOW data_directory").one_row()[0].as<std::string>());
    require(!std::filesystem::is_symlink(root / "data") &&
                actual == std::filesystem::canonical(root / "data") && actual.parent_path() == root,
            "fixture backend directory mismatch");
  }
  const std::array owners{t::Service::Runtime, t::Service::Scheduler, t::Service::Model};
  const std::array<std::string, 3> names{"runtime", "scheduler", "model"};
  for (std::size_t index = 0; index < owners.size(); ++index) {
    active_owner = owners[index];
    if (!verify.empty() && verify[0] != names[index]) {
      continue;
    }
    const auto migration = configuration(root, port, owners[index], names[index], true);
    const auto runtime = configuration(root, port, owners[index], names[index], false);
    if (!verify.empty()) {
      phase = "child-reopen";
      auto database = take(pg::Database::open(runtime));
      t::OperationObservationQuery input;
      input.scope = {{kind(owners[index]), {"resource-a"}}};
      const auto checkpoint = query(*database, input).next_cursor;
      require(checkpoint.epoch == verify[1] && checkpoint.sequence == std::stoull(verify[2]),
              "owner process restart changed durable epoch/head");
      return;
    }
    {
      pqxx::nontransaction ddl(deployment);
      for (const auto& role : {migration.migration_role, migration.runtime_role}) {
        ddl.exec("CREATE ROLE " + ddl.quote_name(role) +
                 " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                 "NOBYPASSRLS PASSWORD " +
                 ddl.quote(secret(root / ("password-" + role))));
      }
      ddl.exec("CREATE DATABASE " + ddl.quote_name(migration.database) + " OWNER " +
               ddl.quote_name(migration.migration_role));
      ddl.exec("REVOKE ALL ON DATABASE " + ddl.quote_name(migration.database) + " FROM PUBLIC");
      ddl.exec("GRANT CONNECT ON DATABASE " + ddl.quote_name(migration.database) + " TO " +
               ddl.quote_name(migration.runtime_role));
    }
    if (describe) {
      phase = "describe-schema";
      // Owned test-only empty database: compute, never override, production manifests.
      pqxx::connection connection(connection_string(migration));
      pqxx::work schema(connection);
      schema.exec(pg::schema::kCreate);
      schema.exec(pg::schema::common_sql());
      schema.exec(pg::schema::service_sql(owners[index]));
      const auto digest = schema.exec(pg::schema::kDescriptorQuery).one_row()[0].as<std::string>();
      const auto count = schema
                             .exec("SELECT count(*) FROM pg_class c JOIN pg_namespace n "
                                   "ON n.oid=c.relnamespace WHERE n.nspname='kiarabot'")
                             .one_row()[0]
                             .as<int>();
      std::cout << "observation-manifest " << names[index] << ' ' << digest << ' ' << count << '\n';
      schema.abort();
      continue;
    }
    spawn(initializer, {initializer.string(), "--initialize-empty", names[index], "localhost",
                        std::to_string(port), migration.database, migration.migration_role,
                        migration.runtime_role, migration.root_certificate,
                        (root / ("password-" + migration.migration_role)).string()});
    t::OperationObservationCursor checkpoint;
    if (owners[index] == t::Service::Runtime) {
      auto factory = take(pg::open_runtime(runtime));
      checkpoint = suite(*factory, runtime, migration);
    } else if (owners[index] == t::Service::Scheduler) {
      auto factory = take(pg::open_scheduler(runtime));
      checkpoint = suite(*factory, runtime, migration);
    } else {
      auto factory = take(pg::open_model(runtime));
      checkpoint = suite(*factory, runtime, migration);
    }
    spawn(self,
          {self.string(), root.string(), std::to_string(port), initializer.string(), proxy,
           "--verify-reopen", names[index], checkpoint.epoch, std::to_string(checkpoint.sequence)});
    phase = "commit-order-barrier";
    ordering(migration);
    phase = "uint64-boundary";
    uint64_boundary(runtime, migration);
  }
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 5 || argc == 6 || argc == 9, "owned fixture arguments required");
    const auto port = std::stoul(argv[2]);
    require(port > 0 && port <= 65535, "invalid fixture port");
    const bool describe = argc == 6 && std::string{argv[5]} == "--describe-schema";
    std::vector<std::string> verify;
    if (argc == 9) {
      require(std::string{argv[5]} == "--verify-reopen", "invalid test mode");
      verify = {argv[6], argv[7], argv[8]};
    }
    run(std::filesystem::canonical(argv[1]), static_cast<std::uint16_t>(port),
        std::filesystem::canonical(argv[3]), std::filesystem::canonical(argv[0]), argv[4], describe,
        verify);
    return 0;
  } catch (const TestFailure& error) {
    std::cerr << "P03 assertion: observation phase " << phase << " owner "
              << static_cast<int>(active_owner) << '\n';
    std::cerr << "P03 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "P03 assertion: observation phase " << phase << " owner "
              << static_cast<int>(active_owner) << '\n';
    std::cerr << "P03 assertion: observation dependency failure (details redacted)\n";
    return 1;
  }
}
