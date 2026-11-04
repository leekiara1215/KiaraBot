// WS-12 real owned-PG evidence for the v1.2 observation feed journals: per-family durable
// epoch/head/low-water, commit-ordered append, typed gap on an evicted boundary and a rotated
// dataset epoch, cross-family cursor rejection and safe-payload validation. It runs against the
// disposable PG16.4 fixture through run_database_test.py and never connects to a user database.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pqxx/pqxx>
#include <sys/wait.h>

#include "../include/kiarabot/infrastructure/postgres/factories.hpp"
#include "../src/observation_feeds.hpp"
#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/observation_feed_identity.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"

extern char** environ;

namespace t = kiarabot::core::target;
namespace pg = kiarabot::infrastructure::postgres;
namespace f = kiarabot::infrastructure::postgres::detail;

namespace {
class TestFailure final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
void require(bool condition, const char* reason) {
  if (!condition) {
    throw TestFailure(reason);
  }
}
template <typename Value> Value take(t::Outcome<Value> result, const char* reason) {
  if (const auto* failure = std::get_if<t::Failure>(&result)) {
    throw TestFailure(std::string{reason} + " (code " +
                      std::to_string(static_cast<int>(failure->code)) + ")");
  }
  return std::move(std::get<Value>(result));
}
template <typename Value> bool rejects(const t::Outcome<Value>& result, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&result);
  return failure != nullptr && failure->code == code;
}
std::string quote(std::string_view value) {
  std::string output = "'";
  for (const char character : value) {
    if (character == '\'' || character == '\\') {
      output += '\\';
    }
    output += character;
  }
  return output + '\'';
}
std::string secret(const std::filesystem::path& file) {
  std::ifstream input(file);
  std::string value;
  std::getline(input, value);
  require(value.size() == 64, "fixture secret unavailable");
  return value;
}
std::string uuid(std::uint64_t value) {
  std::array<char, 40> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "00000000-0000-4000-8000-%012llu",
                static_cast<unsigned long long>(value));
  return std::string{buffer.data()};
}
t::TimePoint wall_clock() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
int spawn_initializer(const std::filesystem::path& executable, const std::filesystem::path& root,
                      std::uint16_t port, std::string_view service, std::string_view database) {
  const std::string prefix = "p03_" + std::string{service};
  const std::array<std::string, 10> arguments{
      executable.string(),
      "--initialize-empty",
      std::string{service},
      "localhost",
      std::to_string(port),
      std::string{database},
      prefix + "_migration",
      prefix + "_runtime",
      (root / "server.pem").string(),
      (root / ("password-" + prefix + "_migration")).string()};
  std::array<char*, 11> argv{};
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    argv[index] = const_cast<char*>(arguments[index].c_str());
  }
  pid_t process = 0;
  require(::posix_spawn(&process, executable.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
          "initializer process spawn failed");
  int status = 0;
  require(::waitpid(process, &status, 0) == process && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "empty Runtime database initialization failed");
  return WEXITSTATUS(status);
}
class RawWork final : public f::SqlWork {
public:
  explicit RawWork(pqxx::connection& connection) : connection_(connection) {}
  t::Status run(const std::function<t::Status(pqxx::transaction_base&)>& function) override {
    pqxx::work transaction(connection_);
    const auto status = function(transaction);
    if (std::holds_alternative<t::Failure>(status)) {
      transaction.abort();
      return status;
    }
    transaction.commit();
    return status;
  }

private:
  pqxx::connection& connection_;
};
t::ResourceRef account(std::uint64_t id) {
  return {t::ResourceKind::Account, t::ResourceId{uuid(id)}};
}
t::LogObservation log_entry(std::uint64_t entry, const t::ResourceRef& resource,
                            std::string severity = "INFO") {
  t::LogObservation log;
  log.owner = t::Service::Runtime;
  log.entry_id = "entry-" + std::to_string(entry);
  log.severity = std::move(severity);
  log.component = "runtime";
  log.method = "ListAccounts";
  log.event = "request.recorded";
  log.resource = resource;
  log.request_id = "request-" + std::to_string(entry);
  log.trace_id = "trace-" + std::to_string(entry);
  log.occurred_at = wall_clock();
  return log;
}
t::ObservationFeedAppend log_append(std::uint64_t entry, const t::ResourceRef& resource,
                                    std::string severity = "INFO") {
  return f::make_log_feed_append(log_entry(entry, resource, std::move(severity)));
}

t::AuditEntry audit_entry(std::uint64_t value) {
  t::AuditEntry entry;
  entry.actor = t::SubjectId{uuid(9000 + value)};
  entry.resource = account(7001);
  entry.request_id = uuid(9100 + value);
  entry.trace_id = uuid(9200 + value);
  entry.method = "ListAccounts";
  entry.authorization_version = t::ResourceVersion{7};
  entry.occurred_at = wall_clock();
  return entry;
}
template <typename Factory>
t::ObservationFeedCursor feed_head(Factory& factory, t::ObservationFamily family,
                                   const std::vector<t::ResourceRef>& scope) {
  auto begun = take(factory.begin(), "feed head begin");
  t::ObservationFeedQuery query;
  query.family = family;
  query.scope = scope;
  auto head = take(begun->query_observation_changes(query), "feed head query");
  begun->rollback();
  return head.next_cursor;
}
template <typename Factory>
std::uint64_t feed_changes_since(Factory& factory, t::ObservationFamily family,
                                 const std::vector<t::ResourceRef>& scope,
                                 const t::ObservationFeedCursor& baseline) {
  auto begun = take(factory.begin(), "feed count begin");
  t::ObservationFeedQuery query;
  query.family = family;
  query.scope = scope;
  query.cursor = baseline;
  auto page = take(begun->query_observation_changes(query), "feed count page");
  begun->rollback();
  return page.changes.size();
}

void run(std::uint16_t port, const std::filesystem::path& root,
         const std::filesystem::path& initializer) {
  std::ifstream admin_file(root / "password");
  std::string admin_password;
  std::getline(admin_file, admin_password);
  require(admin_password.size() == 64, "fixture administrator secret unavailable");
  pqxx::connection administrator(
      "host=localhost port=" + std::to_string(port) + " dbname=postgres user=p03_admin password=" +
      quote(admin_password) + " sslmode=verify-full channel_binding=require sslrootcert=" +
      quote((root / "server.pem").string()));
  {
    // The compiled driver must prove it owns the actual backend directory before any DDL.
    pqxx::read_transaction verify(administrator);
    const auto actual = std::filesystem::canonical(
        verify.exec("SHOW data_directory").one_row()[0].as<std::string>());
    require(!std::filesystem::is_symlink(root / "data") &&
                actual == std::filesystem::canonical(root / "data") && actual.parent_path() == root,
            "fixture backend directory mismatch");
  }
  const auto migration_password = secret(root / "password-p03_runtime_migration");
  {
    pqxx::nontransaction deployment(administrator);
    for (const auto& role : {"p03_runtime_migration", "p03_runtime_runtime"}) {
      deployment.exec("CREATE ROLE " + deployment.quote_name(role) +
                      " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                      "NOBYPASSRLS PASSWORD " +
                      deployment.quote(secret(root / ("password-" + std::string{role}))));
    }
    deployment.exec("CREATE DATABASE p03_runtime OWNER p03_runtime_migration");
    deployment.exec("REVOKE ALL ON DATABASE p03_runtime FROM PUBLIC");
    deployment.exec("GRANT CONNECT ON DATABASE p03_runtime TO p03_runtime_runtime");
  }
  spawn_initializer(initializer, root, port, "runtime", "p03_runtime");
  // The Scheduler owner keeps its private database/schema; the task feed append is exercised on a
  // real Scheduler UoW, not on the Runtime schema.
  {
    pqxx::nontransaction deployment(administrator);
    for (const auto& role : {"p03_scheduler_migration", "p03_scheduler_runtime"}) {
      deployment.exec("CREATE ROLE " + deployment.quote_name(role) +
                      " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                      "NOBYPASSRLS PASSWORD " +
                      deployment.quote(secret(root / ("password-" + std::string{role}))));
    }
    deployment.exec("CREATE DATABASE p03_scheduler OWNER p03_scheduler_migration");
    deployment.exec("REVOKE ALL ON DATABASE p03_scheduler FROM PUBLIC");
    deployment.exec("GRANT CONNECT ON DATABASE p03_scheduler TO p03_scheduler_runtime");
  }
  spawn_initializer(initializer, root, port, "scheduler", "p03_scheduler");
  pqxx::connection migration("host=localhost port=" + std::to_string(port) +
                             " dbname=p03_runtime user="
                             "p03_runtime_migration password=" +
                             quote(migration_password) +
                             " sslmode=verify-full channel_binding=require sslrootcert=" +
                             quote((root / "server.pem").string()));
  // The registered manifest version (I17/R23/S13) already creates the feed journals through the
  // initializer; the driver only proves the real schema/append/query semantics on top of it.
  RawWork work(migration);
  const auto account_a = account(7001);
  const auto account_b = account(7002);
  const auto task_resource = t::ResourceRef{t::ResourceKind::Task, t::ResourceId{uuid(7003)}};
  const auto log_scope =
      std::vector<t::ResourceRef>{{t::ResourceKind::Catalog, t::ResourceId{"control"}}};
  const auto account_scope = std::vector<t::ResourceRef>{account_a};
  const auto task_scope = std::vector<t::ResourceRef>{task_resource};

  require(std::holds_alternative<std::monostate>(
              f::append_observation_changes(work, t::Service::Runtime, t::ObservationFamily::Logs,
                                            {log_append(1, account_a), log_append(2, account_b)})),
          "log append failed");
  t::MessageObservation message;
  message.message_id = uuid(7101);
  message.conversation_id = uuid(7102);
  message.account_id = account_a.id.value;
  message.received_at = wall_clock();
  require(std::holds_alternative<std::monostate>(f::append_observation_changes(
              work, t::Service::Runtime, t::ObservationFamily::Messages,
              {f::make_message_feed_append(message, account_a)})),
          "message append failed");
  t::TaskObservation task;
  task.task_id = task_resource.id.value;
  task.owner_instance_id = uuid(7201);
  task.state = t::TaskFeedState::Completed;
  task.version = 3;
  task.updated_at = wall_clock();
  task.cancel_requested = false;
  require(std::holds_alternative<std::monostate>(f::append_observation_changes(
              work, t::Service::Scheduler, t::ObservationFamily::Tasks,
              {f::make_task_feed_append(task, task_resource)})),
          "task append failed");

  t::ObservationFeedQuery log_query;
  log_query.family = t::ObservationFamily::Logs;
  log_query.scope = log_scope;
  const auto head =
      take(f::query_observation_changes(work, t::Service::Runtime, log_query), "log head query");
  require(head.changes.empty() && head.next_cursor.sequence == 2 &&
              !head.next_cursor.epoch.empty() && head.next_cursor.owner == t::Service::Runtime,
          "log head capture");
  // A resumed client replays from its own processed boundary (0 here), not from the head
  // returned by an empty-cursor capture.
  log_query.cursor = head.next_cursor;
  log_query.cursor->sequence = 0;
  const auto first_page =
      take(f::query_observation_changes(work, t::Service::Runtime, log_query), "log page query");
  if (!(first_page.changes.size() == 2 && !first_page.has_more &&
        first_page.next_cursor.sequence == 2)) {
    throw TestFailure("log page shape: changes=" + std::to_string(first_page.changes.size()) +
                      " more=" + std::to_string(first_page.has_more ? 1 : 0) +
                      " next=" + std::to_string(first_page.next_cursor.sequence) +
                      " head=" + std::to_string(head.next_cursor.sequence) +
                      " cursor=" + std::to_string(log_query.cursor->sequence));
  }
  require(std::get<t::LogObservation>(first_page.changes[0].payload).entry_id == "entry-1" &&
              std::get<t::LogObservation>(first_page.changes[1].payload).entry_id == "entry-2",
          "log page contents");
  const auto next_query = [&] {
    auto query = log_query;
    query.cursor = first_page.next_cursor;
    return query;
  }();
  const auto empty_page =
      take(f::query_observation_changes(work, t::Service::Runtime, next_query), "log idle query");
  require(empty_page.changes.empty() && empty_page.next_cursor.sequence == 2,
          "log idle page keeps the committed boundary");

  t::ObservationFeedQuery message_query;
  message_query.family = t::ObservationFamily::Messages;
  message_query.scope = account_scope;
  const auto message_head = take(
      f::query_observation_changes(work, t::Service::Runtime, message_query), "message head query");
  message_query.cursor = message_head.next_cursor;
  message_query.cursor->sequence = 0;
  const auto message_page = take(
      f::query_observation_changes(work, t::Service::Runtime, message_query), "message page query");
  require(message_page.changes.size() == 1 &&
              std::get<t::MessageObservation>(message_page.changes[0].payload).message_id ==
                  message.message_id,
          "message scope page");
  t::ObservationFeedQuery unseen_scope;
  unseen_scope.family = t::ObservationFamily::Messages;
  unseen_scope.scope = std::vector<t::ResourceRef>{account_b};
  unseen_scope.cursor = take(f::query_observation_changes(work, t::Service::Runtime, unseen_scope),
                             "unseen message head")
                            .next_cursor;
  unseen_scope.cursor->sequence = 0;
  const auto unseen = take(f::query_observation_changes(work, t::Service::Runtime, unseen_scope),
                           "unseen message scope");
  require(unseen.changes.empty() && unseen.next_cursor.sequence >= 1,
          "unseen scope cannot observe another account's message");
  t::ObservationFeedQuery task_query;
  task_query.family = t::ObservationFamily::Tasks;
  task_query.scope = task_scope;
  task_query.cursor =
      take(f::query_observation_changes(work, t::Service::Scheduler, task_query), "task head query")
          .next_cursor;
  task_query.cursor->sequence = 0;
  const auto task_page = take(f::query_observation_changes(work, t::Service::Scheduler, task_query),
                              "task page query");
  require(task_page.changes.size() == 1 &&
              std::get<t::TaskObservation>(task_page.changes[0].payload).state ==
                  t::TaskFeedState::Completed,
          "task terminal state is observable");

  // Cross-family cursor and rotated dataset epoch are typed gaps, never empty successes.
  auto cross_family = log_query;
  cross_family.cursor->family = t::ObservationFamily::Messages;
  require(rejects(f::query_observation_changes(work, t::Service::Runtime, cross_family),
                  t::ErrorCode::InvalidArgument),
          "cross-family cursor rejected");
  require(std::holds_alternative<std::monostate>(
              f::rotate_observation_feed_epoch(work, t::ObservationFamily::Logs)),
          "epoch rotation failed");
  require(rejects(f::query_observation_changes(work, t::Service::Runtime, log_query),
                  t::ErrorCode::Conflict),
          "rotated epoch cursor is a typed gap");
  auto rotated = log_query;
  rotated.cursor.reset();
  const auto rotated_head =
      take(f::query_observation_changes(work, t::Service::Runtime, rotated), "rotated head");
  require(rotated_head.next_cursor.sequence == 0 &&
              rotated_head.next_cursor.epoch != head.next_cursor.epoch,
          "rotated epoch is fresh");

  // Unsafe payloads are rejected before any head mutation (transaction aborted as a whole).
  const auto before = take(f::query_observation_changes(work, t::Service::Runtime, rotated),
                           "pre-invalid append head");
  require(rejects(f::append_observation_changes(
                      work, t::Service::Runtime, t::ObservationFamily::Logs,
                      {log_append(3, account_a), log_append(4, account_a, "TRACE")}),
                  t::ErrorCode::InvalidArgument),
          "unsafe severity rejected");
  const auto after = take(f::query_observation_changes(work, t::Service::Runtime, rotated),
                          "post-invalid append head");
  require(after.next_cursor.sequence == before.next_cursor.sequence,
          "rejected append publishes nothing");

  // An evicted prefix (low-water) is a typed conflict even though the head is available.
  require(std::holds_alternative<std::monostate>(f::append_observation_changes(
              work, t::Service::Runtime, t::ObservationFamily::Logs,
              {log_append(10, account_a), log_append(11, account_b)})),
          "re-journal append failed");
  {
    pqxx::nontransaction advance(migration);
    advance.exec("UPDATE kiarabot.observation_feed_head SET low_water=sequence WHERE family=1");
  }
  auto low_query = rotated;
  low_query.cursor =
      take(f::query_observation_changes(work, t::Service::Runtime, low_query), "post-prune head")
          .next_cursor;
  low_query.cursor->sequence = 0;
  require(rejects(f::query_observation_changes(work, t::Service::Runtime, low_query),
                  t::ErrorCode::Conflict),
          "evicted boundary is a typed gap");

  // Real owner UoW wiring: same-transaction append for logs (via JournalRepository::append_audit
  // hook) and task metadata, with rollback publishing nothing and commit becoming visible.
  const pg::DatabaseConfig runtime_config{t::Service::Runtime,
                                          "localhost",
                                          port,
                                          "p03_runtime",
                                          "p03_runtime_runtime",
                                          secret(root / "password-p03_runtime_runtime"),
                                          (root / "server.pem").string(),
                                          "p03_runtime_migration",
                                          "p03_runtime_runtime"};
  auto runtime_factory = take(pg::open_runtime(runtime_config), "runtime factory");
  {
    // Rollback: the authoritative audit row and the feed append vanish together.
    const auto log_baseline = feed_head(*runtime_factory, t::ObservationFamily::Logs, log_scope);
    auto work = take(runtime_factory->begin(), "log rollback begin");
    require(std::holds_alternative<std::monostate>(work->journal().append_audit(audit_entry(1))),
            "audit append failed");
    work->rollback();
    require(feed_changes_since(*runtime_factory, t::ObservationFamily::Logs, log_scope,
                               log_baseline) == 0,
            "rolled back audit must not publish a log change");
    // Commit: the redacted log fact becomes visible exactly once.
    auto committed = take(runtime_factory->begin(), "log commit begin");
    require(
        std::holds_alternative<std::monostate>(committed->journal().append_audit(audit_entry(2))),
        "audit commit append failed");
    require(committed->commit() == t::CommitResult::Committed, "audit commit failed");
    require(feed_changes_since(*runtime_factory, t::ObservationFamily::Logs, log_scope,
                               log_baseline) == 1,
            "committed audit publishes one log change");
  }
  {
    // Scheduler task family: same-UoW typed append rollback/commit semantics.
    const pg::DatabaseConfig scheduler_config{t::Service::Scheduler,
                                              "localhost",
                                              port,
                                              "p03_scheduler",
                                              "p03_scheduler_runtime",
                                              secret(root / "password-p03_scheduler_runtime"),
                                              (root / "server.pem").string(),
                                              "p03_scheduler_migration",
                                              "p03_scheduler_runtime"};
    auto scheduler_factory = take(pg::open_scheduler(scheduler_config), "scheduler factory");
    t::TaskObservation task_fact;
    task_fact.task_id = uuid(7301);
    task_fact.owner_instance_id = uuid(7302);
    task_fact.state = t::TaskFeedState::Pending;
    task_fact.version = 1;
    task_fact.updated_at = wall_clock();
    t::ObservationFeedAppend task_append;
    task_append.resource = t::ResourceRef{t::ResourceKind::Task, t::ResourceId{task_fact.task_id}};
    task_append.observed_at = task_fact.updated_at;
    task_append.payload = task_fact;
    const auto task_scope_feed = std::vector<t::ResourceRef>{task_append.resource};
    const auto task_baseline =
        feed_head(*scheduler_factory, t::ObservationFamily::Tasks, task_scope_feed);
    {
      auto work = take(scheduler_factory->begin(), "task rollback begin");
      require(std::holds_alternative<std::monostate>(
                  work->append_observation_changes(t::ObservationFamily::Tasks, {task_append})),
              "task append failed");
      work->rollback();
    }
    require(feed_changes_since(*scheduler_factory, t::ObservationFamily::Tasks, task_scope_feed,
                               task_baseline) == 0,
            "rolled back task change must not be visible");
    {
      auto work = take(scheduler_factory->begin(), "task commit begin");
      require(std::holds_alternative<std::monostate>(
                  work->append_observation_changes(t::ObservationFamily::Tasks, {task_append})),
              "task commit append failed");
      require(work->commit() == t::CommitResult::Committed, "task commit failed");
    }
    require(feed_changes_since(*scheduler_factory, t::ObservationFamily::Tasks, task_scope_feed,
                               task_baseline) == 1,
            "committed task change is visible once");
  }
  {
    // Received-message family: one new accepted event appends exactly one self-contained fact and
    // a replayed acceptance with the same fingerprint never appends a second entry.
    const auto message_scope = std::vector<t::ResourceRef>{account(7001)};
    const auto message_baseline =
        feed_head(*runtime_factory, t::ObservationFamily::Messages, message_scope);
    auto work = take(runtime_factory->begin(), "message accept begin");
    t::EventRecord event;
    event.key = t::EventKey{t::ResourceId{uuid(7401)}, "platform-event-7401"};
    event.fingerprint = t::Sha256Digest{};
    event.fingerprint.bytes[0] = std::byte{42};
    event.conversation =
        t::ConversationKey{t::ResourceId{uuid(7401)}, account(7001).id,
                           "platform-conversation-7401", t::ConversationKey::Kind::Private};
    event.payload = t::EventPayloadKind::MessageReceived;
    event.has_conversation = true;
    event.serialized_envelope = "{}";
    event.occurred_at = wall_clock();
    auto accepted =
        take(work->events().accept(event, {}, t::EventCapacityLimits{}), "message accept");
    require(!accepted.duplicate, "first acceptance is not a duplicate");
    require(work->commit() == t::CommitResult::Committed, "message accept commit");
    {
      auto read = take(runtime_factory->begin(), "message query begin");
      t::ObservationFeedQuery query;
      query.family = t::ObservationFamily::Messages;
      query.scope = message_scope;
      query.cursor = message_baseline;
      auto page = take(read->query_observation_changes(query), "message query page");
      read->rollback();
      require(page.changes.size() == 1, "one accepted message publishes one metadata fact");
      const auto& message = std::get<t::MessageObservation>(page.changes[0].payload);
      require(
          message.message_id ==
                  t::message_feed_id(account(7001).id.value, uuid(7401), "platform-event-7401") &&
              message.conversation_id == t::conversation_feed_id(account(7001).id.value, uuid(7401),
                                                                 "platform-conversation-7401") &&
              message.account_id == account(7001).id.value,
          "internal message identity derives from the frozen namespace formula");
    }
    auto replay = take(runtime_factory->begin(), "message replay begin");
    auto duplicated =
        take(replay->events().accept(event, {}, t::EventCapacityLimits{}), "message replay accept");
    require(duplicated.duplicate, "same fingerprint acceptance is a duplicate");
    require(replay->commit() == t::CommitResult::Committed, "message replay commit");
    require(feed_changes_since(*runtime_factory, t::ObservationFamily::Messages, message_scope,
                               message_baseline) == 1,
            "replayed acceptance must not append a second message");
  }
  std::cout << "ws12 observation feed journal: ok\n";
}
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc >= 5, "owned fixture arguments required");
    const auto port = std::stoul(argv[2]);
    require(port > 0 && port <= 65535, "invalid fixture port");
    run(static_cast<std::uint16_t>(port), std::filesystem::canonical(argv[1]),
        std::filesystem::canonical(argv[3]));
    return 0;
  } catch (const TestFailure& error) {
    std::cerr << "WS12 feed assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "WS12 feed assertion: dependency failure (details redacted)\n";
    return 1;
  }
}
