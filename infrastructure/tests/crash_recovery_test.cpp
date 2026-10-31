// Crash recovery evidence for the SQLite runtime store.
//
// A child process keeps committing large snapshots and is killed with SIGKILL
// at varying points (including inside a commit). The parent then reopens the
// same database, expects a decodable last-committed snapshot, SQLite
// PRAGMA integrity_check == "ok", and a successful new commit. This proves the
// journal/FULL-sync recovery path without relying on real power loss.
#include <csignal>
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

#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
using kiarabot::core::RuntimeTransaction;
using kiarabot::core::StorageError;
using kiarabot::infrastructure::SqliteRuntimeStore;

void check(bool result, const std::source_location& location = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("Crash recovery assertion failed at " +
                             std::to_string(location.line()));
  }
}
template <typename T> T value(kiarabot::core::StorageResult<T> result) {
  check(std::holds_alternative<T>(result));
  return std::get<T>(std::move(result));
}
class Directory {
public:
  Directory() {
    auto pattern = (std::filesystem::temp_directory_path() / "kiarabot-crash-XXXXXX").string();
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
std::string integrity_check(const std::string& path) {
  sqlite3* database = nullptr;
  check(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1, &statement, nullptr) ==
        SQLITE_OK);
  check(sqlite3_step(statement) == SQLITE_ROW);
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
  std::string result = text == nullptr ? "" : text;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}
int run_child(const std::string& path) {
  auto opened = SqliteRuntimeStore::open(path);
  if (std::holds_alternative<StorageError>(opened)) {
    return 2;
  }
  auto store = std::get<std::unique_ptr<SqliteRuntimeStore>>(std::move(opened));
  std::uint64_t sequence = value(store->read()).delivery_order;
  {
    auto transaction = value(store->begin());
    transaction->snapshot().delivery_order = sequence + 1;
    if (std::holds_alternative<StorageError>(transaction->commit())) {
      return 3;
    }
    ++sequence;
  }
  const char ready = 'R';
  if (::write(3, &ready, 1) != 1) {
    return 4;
  }
  // Keep committing multi-megabyte snapshots so SIGKILL can land inside a
  // commit window as well as between commits.
  const std::string payload(12U * 1024U * 1024U, 'x');
  for (;;) {
    auto transaction = value(store->begin());
    transaction->snapshot().delivery_order = ++sequence;
    transaction->snapshot().events["crash"].record.serialized_envelope = payload;
    if (std::holds_alternative<StorageError>(transaction->commit())) {
      return 5;
    }
  }
}
pid_t spawn_child(const char* executable, const std::string& path, int* read_descriptor) {
  int pipe_fds[2];
  check(::pipe(pipe_fds) == 0);
  const pid_t child = ::fork();
  check(child >= 0);
  if (child == 0) {
    ::close(pipe_fds[0]);
    if (::dup2(pipe_fds[1], 3) < 0) {
      ::_exit(126);
    }
    ::close(pipe_fds[1]);
    ::execl(executable, executable, "--child", path.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(pipe_fds[1]);
  *read_descriptor = pipe_fds[0];
  return child;
}
void wait_for_ready(int descriptor) {
  char ready = 0;
  ssize_t count = 0;
  do {
    count = ::read(descriptor, &ready, 1);
  } while (count < 0 && errno == EINTR);
  check(count == 1 && ready == 'R');
}
void reap(pid_t child) {
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    throw std::runtime_error("Crash recovery child was not SIGKILLed, wait status=" +
                             std::to_string(status));
  }
}
void recover(const std::string& path, std::uint64_t* highest_sequence) {
  auto store = value(SqliteRuntimeStore::open(path));
  const auto snapshot = value(store->read());
  check(snapshot.delivery_order >= *highest_sequence);
  check(snapshot.delivery_order >= 1);
  *highest_sequence = snapshot.delivery_order;
  check(integrity_check(path) == "ok");
  {
    auto transaction = value(store->begin());
    transaction->snapshot().delivery_order = snapshot.delivery_order + 1;
    check(std::holds_alternative<std::monostate>(transaction->commit()));
  }
  check(value(store->read()).delivery_order == snapshot.delivery_order + 1);
  *highest_sequence = snapshot.delivery_order + 1;
}
} // namespace

int main(int argc, char* argv[]) {
  if (argc == 3 && std::string_view(argv[1]) == "--child") {
    try {
      return run_child(argv[2]);
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 6;
    }
  }
  if (argc != 2) {
    std::cerr << "usage: crash_recovery_test <child-executable>\n";
    return 2;
  }
  try {
    Directory directory;
    const std::string path = (directory.path / "runtime.db").string();
    std::uint64_t highest_sequence = 0;
    // Short-to-long kill delays: some land between commits, some inside the
    // large write/fsync window.
    for (const useconds_t delay : {1000, 5000, 20000, 50000, 100000, 200000}) {
      int read_descriptor = -1;
      const pid_t child = spawn_child(argv[1], path, &read_descriptor);
      wait_for_ready(read_descriptor);
      ::close(read_descriptor);
      ::usleep(delay);
      check(::kill(child, SIGKILL) == 0);
      reap(child);
      recover(path, &highest_sequence);
    }
    check(integrity_check(path) == "ok");
    std::cout << "SIGKILL during commits recovered with intact snapshot and integrity_check ok\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
