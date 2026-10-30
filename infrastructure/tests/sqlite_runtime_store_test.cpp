#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <latch>
#include <source_location>
#include <sqlite3.h>
#include <stdexcept>
#include <unistd.h>

#include "kiarabot/core/authorized_commands.hpp"
#include "kiarabot/core/runtime.h"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
void check(bool result, const std::source_location& location = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("SQLite runtime assertion failed at " +
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
    auto pattern = (std::filesystem::temp_directory_path() / "kiarabot-sqlite-XXXXXX").string();
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
void execute(const std::string& path, const char* sql) {
  sqlite3* database = nullptr;
  check(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
  sqlite3_close(database);
  check(result == SQLITE_OK);
}
} // namespace
int main() {
  using kiarabot::core::ActionResult;
  using kiarabot::core::DeliveryState;
  using kiarabot::core::EventResult;
  using kiarabot::core::Runtime;
  using kiarabot::core::RuntimeTimePoint;
  using kiarabot::core::StorageError;
  using kiarabot::infrastructure::SqliteRuntimeStore;
  try {
    Directory directory;
    const auto path = (directory.path / "runtime.db").string();
    const auto legacy_path = (directory.path / "legacy.db").string();
    execute(legacy_path, "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY,revision INTEGER "
                         "NOT NULL,payload BLOB NOT NULL); INSERT INTO runtime_snapshot "
                         "VALUES(1,0,'0 0 0 0 0 9 '); PRAGMA user_version=1");
    auto migrated = value(SqliteRuntimeStore::open(legacy_path));
    check(value(migrated->read()).delivery_order == 9);
    check(value(migrated->read()).audit.empty());
    auto migrated_again = value(SqliteRuntimeStore::open(legacy_path));
    check(value(migrated_again->read()).delivery_order == 9);
    const auto v2_path = (directory.path / "v2.db").string();
    execute(v2_path, "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY,revision INTEGER "
                     "NOT NULL,payload BLOB NOT NULL); INSERT INTO runtime_snapshot "
                     "VALUES(1,0,'0 0 1 \"key\" \"action\" \"key\" 1000000000 0 1000000000 \"\" "
                     "\"\" 0 0 7 0 0 '); PRAGMA user_version=2");
    auto from_v2 = value(SqliteRuntimeStore::open(v2_path));
    const auto old_action = value(from_v2->read()).actions.at("key");
    check(old_action.record.action_id == "action" && old_action.record.owner.empty());
    check(!old_action.dispatched && old_action.record.serialized_envelope.empty());
    check(value(from_v2->read()).delivery_order == 7);
    const auto v3_path = (directory.path / "v3.db").string();
    execute(v3_path,
            "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY,revision INTEGER "
            "NOT NULL,payload BLOB NOT NULL); INSERT INTO runtime_snapshot "
            "VALUES(1,0,'0 0 1 \"key\" \"action\" \"key\" 1000000000 \"owner\" \"payload\" "
            "\"fingerprint\" 0 1000000000 \"\" \"\" 1 0 0 8 0 0 '); PRAGMA user_version=3");
    auto from_v3 = value(SqliteRuntimeStore::open(v3_path));
    const auto v3_state = value(from_v3->read());
    check(v3_state.actions.at("key").record.owner == "owner");
    check(v3_state.actions.at("key").dispatched);
    check(v3_state.namespaces.empty() && v3_state.host_receipts.empty());
    check(v3_state.delivery_order == 8);
    const auto now = RuntimeTimePoint{} + std::chrono::hours(1);
    const auto v4_path = (directory.path / "v4.db").string();
    execute(
        v4_path,
        "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY,revision INTEGER NOT NULL,payload "
        "BLOB NOT NULL);"
        "INSERT INTO runtime_snapshot VALUES(1,0,'0 0 0 0 0 12 0 0 0 0 '); PRAGMA user_version=4");
    auto from_v4 = value(SqliteRuntimeStore::open(v4_path));
    check(value(from_v4->read()).delivery_order == 12 && value(from_v4->read()).tasks.empty());
    {
      auto store = value(SqliteRuntimeStore::open(path));
      Runtime runtime(*store, {}, [&] { return now; }, [] { return 0.5; });
      check(value(runtime.register_plugin("plugin", "v1", {"message.send"}, true)));
      check(value(runtime.plugin_started("plugin")));
      check(value(runtime.ingest({"event", "trace", "session", 1, true, std::string("a\0b", 3)},
                                 {"plugin"})) == EventResult::accepted);
      check(value(runtime.mark_plugin_dispatched("event", "plugin")));
      check(value(runtime.submit_action({"action", "key", now + std::chrono::minutes(1)})).result ==
            ActionResult::accepted);
      kiarabot::core::AuthorizedCommands commands(*store, {{"plugin", "maintain", "runtime"}},
                                                  [&] { return now; });
      kiarabot::core::CommandContext command{"plugin", "maintain", "runtime",           "request",
                                             "trace",  "command",  std::string(64, 'a')};
      check(std::holds_alternative<std::monostate>(commands.execute(
          command, [](auto&) -> kiarabot::core::PortStatus { return std::monostate{}; })));
      auto transaction = value(store->begin());
      transaction->snapshot().events.clear();
    }
    auto store = value(SqliteRuntimeStore::open(path));
    Runtime restarted(*store, {}, [&] { return now; }, [] { return 0.5; });
    auto snapshot = value(store->read());
    check(snapshot.events.at("event").record.serialized_envelope == std::string("a\0b", 3));
    check(snapshot.sessions.at("session").last_sequence == 1);
    check(snapshot.audit.size() == 1 && snapshot.audit.front().outcome == "OK");
    check(snapshot.commands.at("plugin").size() == 1);
    check(snapshot.plugins.at("plugin").permissions.at(0) == "message.send");
    check(snapshot.deliveries.at("event").at("plugin").state == DeliveryState::dispatched);
    check(value(restarted.recover_plugin_inflight("plugin")).count == 1);
    check(value(restarted.ingest({"event", "new", "session", 1, true})) == EventResult::duplicate);
    check(value(restarted.submit_action({"other", "key", now + std::chrono::minutes(1)})).result ==
          ActionResult::duplicate);
    auto another = value(SqliteRuntimeStore::open(path));
    auto first = value(store->begin());
    auto stale = value(another->begin());
    first->snapshot().delivery_order = 8;
    value(first->commit());
    stale->snapshot().delivery_order = 99;
    check(std::get<StorageError>(stale->commit()) == StorageError::Conflict);
    check(value(another->read()).delivery_order == 8);
    auto concurrent_a = value(store->begin());
    auto concurrent_b = value(another->begin());
    concurrent_a->snapshot().delivery_order = 11;
    concurrent_b->snapshot().delivery_order = 12;
    std::latch launch(1);
    auto result_a = std::async(std::launch::async, [&] {
      launch.wait();
      return concurrent_a->commit();
    });
    auto result_b = std::async(std::launch::async, [&] {
      launch.wait();
      return concurrent_b->commit();
    });
    launch.count_down();
    const auto a = result_a.get();
    const auto b = result_b.get();
    check(std::holds_alternative<std::monostate>(a) != std::holds_alternative<std::monostate>(b));
    const auto committed_order = value(store->read()).delivery_order;
    check(committed_order == 11 || committed_order == 12);
    sqlite3* lock = nullptr;
    check(sqlite3_open(path.c_str(), &lock) == SQLITE_OK);
    check(sqlite3_exec(lock, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    auto blocked = value(store->begin());
    blocked->snapshot().delivery_order = 20;
    check(std::get<StorageError>(blocked->commit()) == StorageError::Conflict);
    sqlite3_exec(lock, "ROLLBACK", nullptr, nullptr, nullptr);
    check(sqlite3_exec(lock, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == SQLITE_OK);
    check(std::get<StorageError>(store->read()) == StorageError::Conflict);
    check(std::get<StorageError>(store->begin()) == StorageError::Conflict);
    sqlite3_exec(lock, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(lock);
    check(value(store->read()).delivery_order == committed_order);
    execute(path, "UPDATE runtime_snapshot SET payload=x'00'");
    check(std::get<StorageError>(store->read()) == StorageError::Corrupt);
    check(std::get<StorageError>(SqliteRuntimeStore::open(path)) == StorageError::Corrupt);
    execute(path, "PRAGMA user_version=99");
    check(std::get<StorageError>(SqliteRuntimeStore::open(path)) == StorageError::Corrupt);
    check(std::get<StorageError>(SqliteRuntimeStore::open(
              (directory.path / "absent" / "db").string())) == StorageError::Unavailable);
    {
      Directory removed;
      auto removed_path = (removed.path / "runtime.db").string();
      auto disconnected = value(SqliteRuntimeStore::open(removed_path));
      auto transaction = value(disconnected->begin());
      transaction->snapshot().delivery_order = 42;
      std::filesystem::remove(removed_path);
      std::filesystem::remove(removed.path);
      check(std::holds_alternative<StorageError>(transaction->commit()));
    }
    std::cout << "SQLite migration, restart, rollback, conflict, corruption and filesystem failure "
                 "passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
