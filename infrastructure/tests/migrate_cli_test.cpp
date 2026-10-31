// Process-level tests for the `kiarabot-migrate` entry point.
//
// The tests drive the built executable exactly as an operator or deployment
// script would: exit codes, stdout/stderr markers, idempotent re-runs, legacy
// fixture migration, corrupt/unknown schema rejection, lock-contention retry
// and two concurrent migrators.
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
using kiarabot::core::StorageError;
using kiarabot::infrastructure::SqliteRuntimeStore;

void check(bool result, const std::source_location& location = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("Migrate CLI assertion failed at " +
                             std::to_string(location.line()));
  }
}
class Directory {
public:
  Directory() {
    auto pattern = (std::filesystem::temp_directory_path() / "kiarabot-migrate-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) {
      throw std::runtime_error("Temporary directory failed");
    }
    path = pattern;
  }
  ~Directory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};
class Sqlite {
public:
  explicit Sqlite(const std::string& path) {
    check(sqlite3_open(path.c_str(), &database_) == SQLITE_OK);
  }
  ~Sqlite() {
    if (database_ != nullptr) {
      sqlite3_close(database_);
    }
  }
  Sqlite(const Sqlite&) = delete;
  Sqlite& operator=(const Sqlite&) = delete;
  sqlite3* get() const {
    return database_;
  }
  void execute(const std::string& sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
      const std::string text = message == nullptr ? "unknown error" : message;
      sqlite3_free(message);
      throw std::runtime_error("SQLite exec failed: " + text);
    }
  }
  std::string text(const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    std::string result;
    if (sqlite3_step(statement) == SQLITE_ROW) {
      const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
      result = value == nullptr ? "" : value;
    }
    sqlite3_finalize(statement);
    return result;
  }
  int integer(const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    int result = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) {
      result = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    return result;
  }

private:
  sqlite3* database_{nullptr};
};
class Process {
public:
  static Process start(const std::string& executable, const std::vector<std::string>& arguments) {
    int pipe_fds[2];
    check(::pipe(pipe_fds) == 0);
    const pid_t child = ::fork();
    check(child >= 0);
    if (child == 0) {
      ::close(pipe_fds[0]);
      if (::dup2(pipe_fds[1], 1) < 0 || ::dup2(pipe_fds[1], 2) < 0) {
        ::_exit(126);
      }
      ::close(pipe_fds[1]);
      std::vector<std::string> owned{executable};
      for (const auto& argument : arguments) {
        owned.push_back(argument);
      }
      std::vector<char*> argv;
      for (auto& argument : owned) {
        argv.push_back(argument.data());
      }
      argv.push_back(nullptr);
      ::execv(executable.c_str(), argv.data());
      ::_exit(127);
    }
    ::close(pipe_fds[1]);
    return Process{child, pipe_fds[0]};
  }
  struct Outcome {
    int exit_code{0};
    std::string output;
  };
  Outcome finish() {
    std::string output;
    char buffer[4096];
    for (;;) {
      const ssize_t count = ::read(read_descriptor_, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        break;
      }
      output.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(read_descriptor_);
    read_descriptor_ = -1;
    int status = 0;
    while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {
    }
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
  }

private:
  Process(pid_t child, int read_descriptor) : child_(child), read_descriptor_(read_descriptor) {}
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  pid_t child_;
  int read_descriptor_;
};
Process::Outcome run(const std::string& executable, const std::vector<std::string>& arguments) {
  return Process::start(executable, arguments).finish();
}
void create_schema(const std::string& path, const std::string& payload, int version) {
  Sqlite database(path);
  database.execute(
      "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY,revision INTEGER NOT NULL,payload BLOB "
      "NOT NULL)");
  database.execute("INSERT INTO runtime_snapshot VALUES(1,0,'" + payload + "')");
  database.execute("PRAGMA user_version=" + std::to_string(version));
}
} // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: migrate_cli_test <kiarabot-migrate>\n";
    return 2;
  }
  const std::string migrate = argv[1];
  try {
    Directory directory;
    const auto database = [&](const std::string& name) {
      return (directory.path / name).string();
    };

    check(run(migrate, {}).exit_code == 2);
    check(run(migrate, {":memory:"}).exit_code == 2);
    check(run(migrate, {"file:" + database("uri.db")}).exit_code == 2);

    // Fresh database: creates schema v5, reports completed; repeated run is a
    // no-op success.
    const std::string fresh = database("fresh.db");
    auto first = run(migrate, {fresh});
    check(first.exit_code == 0);
    check(first.output.find("operation=migrate outcome=completed schema=5") != std::string::npos);
    {
      Sqlite check_database(fresh);
      check(check_database.integer("PRAGMA user_version") == 5);
    }
    check(run(migrate, {fresh}).exit_code == 0);

    // Legacy v1 fixture migrates through the same transaction.
    const std::string legacy = database("legacy.db");
    create_schema(legacy, "0 0 0 0 0 9 ", 1);
    check(run(migrate, {legacy}).exit_code == 0);
    {
      auto store = std::get<std::unique_ptr<SqliteRuntimeStore>>(SqliteRuntimeStore::open(legacy));
      auto snapshot = std::get<kiarabot::core::RuntimeSnapshot>(store->read());
      check(snapshot.delivery_order == 9);
    }

    // Unknown schema and corrupt payload fail without rewriting the database.
    const std::string unknown = database("unknown.db");
    create_schema(unknown, "0 0 0 0 0 0 ", 99);
    auto rejected = run(migrate, {unknown});
    check(rejected.exit_code == 1);
    check(rejected.output.find("operation=migrate outcome=failed") != std::string::npos);
    {
      Sqlite check_database(unknown);
      check(check_database.integer("PRAGMA user_version") == 99);
    }
    const std::string corrupt = database("corrupt.db");
    check(run(migrate, {corrupt}).exit_code == 0);
    {
      Sqlite damage(corrupt);
      damage.execute("UPDATE runtime_snapshot SET payload=x'00' WHERE id=1");
    }
    auto failed = run(migrate, {corrupt});
    check(failed.exit_code == 1);
    {
      Sqlite check_database(corrupt);
      check(check_database.integer("PRAGMA user_version") == 5);
      check(check_database.integer("SELECT length(payload) FROM runtime_snapshot") == 1);
    }

    // A concurrent write transaction blocks BEGIN IMMEDIATE; the CLI retries
    // until the lock is released instead of failing immediately.
    const std::string locked = database("locked.db");
    check(run(migrate, {locked}).exit_code == 0);
    {
      Sqlite holder(locked);
      holder.execute("BEGIN IMMEDIATE");
      auto pending = Process::start(migrate, {locked});
      ::usleep(1500000);
      holder.execute("ROLLBACK");
      auto outcome = pending.finish();
      check(outcome.exit_code == 0);
    }

    // Two instances started together serialize on the migration lock.
    const std::string concurrent = database("concurrent.db");
    auto left = Process::start(migrate, {concurrent});
    auto right = Process::start(migrate, {concurrent});
    const auto left_result = left.finish();
    const auto right_result = right.finish();
    check(left_result.exit_code == 0 && right_result.exit_code == 0);
    {
      Sqlite check_database(concurrent);
      check(check_database.integer("PRAGMA user_version") == 5);
      check(check_database.text("PRAGMA integrity_check") == "ok");
    }

    // A path whose parent directory is missing is a dependency failure, not a
    // usage error.
    check(run(migrate, {(directory.path / "missing" / "runtime.db").string()}).exit_code == 1);

    std::cout << "migrate CLI argument, idempotency, migration, corruption, lock and concurrency "
                 "paths passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
