#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>
#include <vector>

#include <sys/wait.h>

extern char** environ;

#include <pqxx/pqxx>

#include "kiarabot/core/application/target/scheduler_service.hpp"
#include "kiarabot/core/ports/target/scheduler_ports.hpp"
#include "kiarabot/infrastructure/postgres/factories.hpp"

namespace pg = kiarabot::infrastructure::postgres;
namespace target = kiarabot::core::target;

namespace {

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, const char* message) {
  if (!condition) {
    throw TestFailure(message);
  }
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

std::string uuid(std::uint8_t first) {
  std::string result(36, '0');
  for (const int index : {8, 13, 18, 23}) {
    result[static_cast<std::size_t>(index)] = '-';
  }
  result[0] = static_cast<char>('a' + (first % 6));
  result[14] = '4';
  result[19] = '8';
  return result;
}

int initialize_process(const std::filesystem::path& executable, const pg::DatabaseConfig& config,
                       const std::filesystem::path& password_file) {
  const std::array<std::string, 10> arguments{
      executable.string(),   "--initialize-empty",        "scheduler",
      config.host,           std::to_string(config.port), config.database,
      config.migration_role, config.runtime_role,         config.root_certificate,
      password_file.string()};
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
          "scheduler database initialization failed");
  return 0;
}

class Zones final : public target::SchedulerTimeZonePort {
public:
  bool available() const override {
    return true;
  }
  target::Outcome<target::TimePoint> resolve(const target::LocalCivilTime&) override {
    return resolved;
  }
  target::TimePoint resolved;
};

class Ids final : public target::SchedulerIdPort {
public:
  target::Outcome<target::MessageId> occurrence_id() override {
    return target::MessageId{uuid(static_cast<std::uint8_t>(1 + (counter++ % 40)))};
  }
  target::Outcome<target::OperationId> operation_id() override {
    return target::OperationId{uuid(static_cast<std::uint8_t>(30 + (counter++ % 20)))};
  }
  int counter = 0;
};

target::SchedulerCommandContext context() {
  return target::SchedulerCommandContext{
      target::InstanceId{uuid(1)}, target::InstallationId{uuid(2)}, uuid(3), uuid(4),
      target::TimePoint{std::chrono::seconds{100000}}, std::string(16, 'k')};
}

target::TaskCreationInput input(std::string task_id, target::TimePoint run_at) {
  target::TaskCreationInput value;
  value.task_id = std::move(task_id);
  value.target = target::ConversationKey{target::ResourceId{uuid(5)}, target::ResourceId{uuid(6)},
                                          "conversation", target::ConversationKey::Kind::Private};
  value.payload = {std::byte{1}};
  value.run_at = run_at;
  return value;
}

void run(std::uint16_t port, const std::filesystem::path& root,
         const std::filesystem::path& initializer) {
  std::ifstream admin_password_file(root / "password");
  std::string admin_password;
  std::getline(admin_password_file, admin_password);
  require(admin_password.size() == 64, "fixture administrator secret unavailable");
  const auto role_password = [&](const std::string& role) {
    std::ifstream input(root / ("password-" + role));
    std::string value;
    std::getline(input, value);
    require(value.size() == 64, "fixture role secret unavailable");
    return value;
  };
  {
    pqxx::connection administrator(
        "host=localhost port=" + std::to_string(port) +
        " dbname=postgres user=p03_admin password=" + quote(admin_password) +
        " sslmode=verify-full channel_binding=require sslrootcert=" +
        quote((root / "server.pem").string()));
    {
      pqxx::read_transaction guard(administrator);
      const auto actual = std::filesystem::canonical(
          guard.exec("SHOW data_directory").one_row()[0].as<std::string>());
      require(!std::filesystem::is_symlink(root / "data") &&
                  actual == std::filesystem::canonical(root / "data") &&
                  actual.parent_path() == root,
              "fixture backend directory mismatch");
    }
    pqxx::nontransaction deployment(administrator);
    for (const auto& role : {"p03_scheduler_migration", "p03_scheduler_runtime"}) {
      deployment.exec("CREATE ROLE " + deployment.quote_name(role) +
                      " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION "
                      "NOBYPASSRLS PASSWORD " +
                      deployment.quote(role_password(role)));
    }
    deployment.exec("CREATE DATABASE p03_scheduler OWNER p03_scheduler_migration");
    deployment.exec("REVOKE ALL ON DATABASE p03_scheduler FROM PUBLIC");
    deployment.exec("GRANT CONNECT ON DATABASE p03_scheduler TO p03_scheduler_runtime");
  }
  pg::DatabaseConfig config{target::Service::Scheduler,
                            "localhost",
                            port,
                            "p03_scheduler",
                            "p03_scheduler_runtime",
                            role_password("p03_scheduler_runtime"),
                            (root / "server.pem").string(),
                            "p03_scheduler_migration",
                            "p03_scheduler_runtime"};
  initialize_process(initializer, config, root / "password-p03_scheduler_migration");
  auto opened = pg::open_scheduler(config);
  require(!std::holds_alternative<target::Failure>(opened), "scheduler factory open failed");
  auto factory = std::move(std::get<std::unique_ptr<target::SchedulerUnitOfWorkFactory>>(opened));
  Zones zones;
  Ids ids;
  target::SchedulerServiceDependencies dependencies{*factory, ids, zones};
  target::SchedulerService service{dependencies};
  const auto ctx = context();
  const auto db_now = [&]() {
    auto work = factory->begin();
    require(!std::holds_alternative<target::Failure>(work), "scheduler UoW begin failed");
    auto uow = std::move(std::get<std::unique_ptr<target::SchedulerUnitOfWork>>(work));
    auto sample = uow->current_time();
    uow->rollback();
    require(!std::holds_alternative<target::Failure>(sample), "database time unavailable");
    return std::get<target::TimePoint>(sample);
  };
  const auto now = db_now();
  auto created = service.create_task(ctx, input("task-1", now + std::chrono::seconds{60}));
  if (!std::holds_alternative<target::ScheduledTask>(created)) {
    const auto* rejected = std::get_if<target::Failure>(&created);
    std::cerr << "P10 assertion: create code="
              << (rejected ? static_cast<int>(rejected->code) : -1) << " acceptance="
              << (rejected ? static_cast<int>(rejected->acceptance) : -1) << '\n';
    throw TestFailure("create task failed");
  }
  auto replay = service.create_task(ctx, input("task-1", now + std::chrono::seconds{60}));
  require(std::holds_alternative<target::ScheduledTask>(replay), "idempotent create failed");
  auto past = service.create_task(ctx, input("task-past", now - std::chrono::seconds{60}));
  require(std::holds_alternative<target::Failure>(past), "past schedule must fail");
  require(std::get<target::Failure>(past).code == target::ErrorCode::InvalidArgument,
          "past schedule is INVALID_ARGUMENT");
  auto activated = service.due_sweep(now + std::chrono::seconds{61}, 10);
  if (!std::holds_alternative<std::size_t>(activated)) {
    const auto* rejected = std::get_if<target::Failure>(&activated);
  }
  require(std::holds_alternative<std::size_t>(activated), "due sweep failed");
  require(std::get<std::size_t>(activated) == 1, "only one due task activates");
  auto task = service.get_task(ctx, "task-1");
  require(std::holds_alternative<target::ScheduledTask>(task) &&
              std::get<target::ScheduledTask>(task).state == target::TaskState::Running,
          "due task becomes RUNNING");
  auto cancelled = service.cancel_task(ctx, "task-1");
  require(std::holds_alternative<target::Failure>(cancelled) &&
              std::get<target::Failure>(cancelled).code == target::ErrorCode::Conflict,
          "cancel after handoff intent is CONFLICT");
  auto claims = service.claim_triggers(now + std::chrono::seconds{61}, 10,
                                       std::chrono::seconds{30});
  require(std::holds_alternative<std::vector<target::SchedulerTriggerClaim>>(claims),
          "outbox claim failed");
  const auto claimed = std::get<std::vector<target::SchedulerTriggerClaim>>(claims);
  require(claimed.size() == 1, "one trigger claim");
  require(claimed.front().occurrence.attempts == 1, "claim persists attempt observation");
  require(claimed.front().occurrence.created_at.time_since_epoch().count() > 0,
          "occurrence created_at is persisted");
  require(claimed.front().occurrence.updated_at >= claimed.front().occurrence.created_at,
          "occurrence updated_at is persisted");
  auto occurrence = service.confirm_trigger_accepted(claimed.front().claim, "event-1",
                                                     now + std::chrono::seconds{62});
  require(std::holds_alternative<target::Occurrence>(occurrence) &&
              std::get<target::Occurrence>(occurrence).state == target::OperationState::Succeeded,
          "confirmed occurrence is SUCCEEDED");
  auto completed = service.get_task(ctx, "task-1");
  require(std::holds_alternative<target::ScheduledTask>(completed) &&
              std::get<target::ScheduledTask>(completed).state == target::TaskState::Completed,
          "confirmed task is COMPLETED");
  const auto completed_task = std::get<target::ScheduledTask>(completed);
  require(completed_task.created_at.time_since_epoch().count() > 0 &&
              completed_task.updated_at >= completed_task.created_at,
          "task observation timestamps are persisted");
  // A pre-handoff cancel records the cancel observation flag on the task row.
  auto cancellable = service.create_task(ctx, input("task-cancel", now + std::chrono::seconds{60}));
  require(std::holds_alternative<target::ScheduledTask>(cancellable), "cancellable create");
  auto cancelled_task = service.cancel_task(ctx, "task-cancel");
  require(std::holds_alternative<target::ScheduledTask>(cancelled_task) &&
              std::get<target::ScheduledTask>(cancelled_task).cancel_requested,
          "cancel_requested observation is persisted");
  auto retried = service.confirm_trigger_accepted(claimed.front().claim, "event-1",
                                                  now + std::chrono::seconds{63});
  require(std::holds_alternative<target::Occurrence>(retried),
          "confirmation loss retry is idempotent");
  // Expiry path: a second due task whose handoff is never confirmed becomes EXPIRED/FAILED.
  auto second = service.create_task(ctx, input("task-expire", now + std::chrono::seconds{60}));
  require(std::holds_alternative<target::ScheduledTask>(second), "second create failed");
  require(std::get<std::size_t>(service.due_sweep(now + std::chrono::seconds{61}, 10)) == 1,
          "second due activation");
  auto second_claim = service.claim_triggers(now + std::chrono::seconds{61}, 10,
                                             std::chrono::seconds{30});
  require(std::get<std::vector<target::SchedulerTriggerClaim>>(second_claim).size() == 1,
          "second claim");
  auto expired = service.expire_stale_triggers(now + std::chrono::seconds{61} + std::chrono::hours{24 * 7 + 1}, 10);
  require(std::holds_alternative<std::size_t>(expired) && std::get<std::size_t>(expired) == 1,
          "stale trigger expires exactly once");
  auto failed = service.get_task(ctx, "task-expire");
  require(std::holds_alternative<target::ScheduledTask>(failed) &&
              std::get<target::ScheduledTask>(failed).state == target::TaskState::Failed,
          "expired task is FAILED");
  // Restart recovery: reopen the factory and confirm durable authority survives.
  factory.reset();
  auto reopened = pg::open_scheduler(config);
  require(!std::holds_alternative<target::Failure>(reopened), "scheduler reopen failed");
  auto reopened_factory =
      std::move(std::get<std::unique_ptr<target::SchedulerUnitOfWorkFactory>>(reopened));
  target::SchedulerServiceDependencies restarted{*reopened_factory, ids, zones};
  target::SchedulerService restarted_service{restarted};
  auto persisted = restarted_service.get_task(ctx, "task-1");
  require(std::holds_alternative<target::ScheduledTask>(persisted) &&
              std::get<target::ScheduledTask>(persisted).state == target::TaskState::Completed,
          "completed task survives restart");
  // Schema version is bumped for the Scheduler private database.
  pqxx::connection inspection(
      "host=localhost port=" + std::to_string(port) + " dbname=p03_scheduler user=p03_scheduler_runtime password=" +
      quote(role_password("p03_scheduler_runtime")) +
      " sslmode=verify-full channel_binding=require sslrootcert=" +
      quote((root / "server.pem").string()));
  pqxx::nontransaction schema_check(inspection);
  const auto version = schema_check.exec("SELECT version FROM kiarabot.schema_versions")
                           .one_row()[0]
                           .as<int>();
  require(version == 8, "scheduler schema version must be 8");
  const auto columns = schema_check.exec(
      "SELECT count(*) FROM information_schema.columns WHERE table_schema='kiarabot' AND "
      "((table_name='tasks' AND column_name IN ('cancel_requested','reason','created_at','"
      "updated_at')) OR (table_name='occurrences' AND column_name IN ('attempts','last_error',"
      "'updated_at')))");
  require(columns.one_row()[0].as<int>() == 7, "v8 observation columns present");
  const auto quota = schema_check.exec(
      "SELECT to_regclass('kiarabot.scheduler_quota_counters') IS NOT NULL");
  require(quota.one_row()[0].as<bool>(), "scheduler quote counters relation present");
  const auto indexes = schema_check.exec(
      "SELECT count(*) FROM pg_indexes WHERE schemaname='kiarabot' AND indexname IN "
      "('tasks_due','tasks_owner','occurrences_owner','occurrences_inflight')");
  require(indexes.one_row()[0].as<int>() == 4, "due/owner indexes present");
  std::cout << "scheduler application fixtures only\n";
}

} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 5, "fixture arguments required");
    const auto port = std::stoul(argv[2]);
    require(port > 0 && port <= 65535, "invalid fixture port");
    run(static_cast<std::uint16_t>(port), std::filesystem::canonical(argv[1]),
        std::filesystem::canonical(argv[3]));
    return 0;
  } catch (const TestFailure& error) {
    std::cerr << "P10 assertion: " << error.what() << '\n';
    return 1;
  } catch (const std::exception&) {
    std::cerr << "P10 Scheduler PostgreSQL test failed (details redacted)\n";
    return 1;
  }
}
