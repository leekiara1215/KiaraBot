#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <latch>
#include <source_location>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "../src/runtime_snapshot_codec.hpp"
#include "kiarabot/core/application/authorized_commands.hpp"
#include "kiarabot/core/application/runtime.h"
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
void write_snapshot(const std::string& path, const std::string& payload, int version) {
  sqlite3* database = nullptr;
  check(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  check(sqlite3_exec(database,
                     "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY,revision INTEGER NOT "
                     "NULL,payload BLOB NOT NULL)",
                     nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(database, "INSERT INTO runtime_snapshot VALUES(1,0,?)", -1, &statement,
                           nullptr) == SQLITE_OK);
  check(sqlite3_bind_blob(statement, 1, payload.data(), static_cast<int>(payload.size()),
                          SQLITE_TRANSIENT) == SQLITE_OK);
  check(sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  const std::string pragma = "PRAGMA user_version=" + std::to_string(version);
  check(sqlite3_exec(database, pragma.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
}
int user_version(const std::string& path) {
  sqlite3* database = nullptr;
  check(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK);
  check(sqlite3_step(statement) == SQLITE_ROW);
  const int version = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return version;
}
kiarabot::v1::EventEnvelope message_envelope(std::string event_id, std::uint64_t sequence,
                                             int type) {
  kiarabot::v1::EventEnvelope result;
  result.set_event_id(std::move(event_id));
  result.set_trace_id("trace");
  result.set_schema_version("kiarabot.v1");
  result.mutable_occurred_at()->set_seconds(1000);
  result.set_delivery_mode(kiarabot::v1::AT_LEAST_ONCE);
  result.set_sequence(sequence);
  result.mutable_source()->set_adapter_id("adapter");
  result.mutable_source()->set_account_id("account");
  result.mutable_subject()->set_conversation_id("123");
  result.mutable_subject()->set_user_id("user");
  auto* payload = result.mutable_message_received();
  payload->set_message_id("message");
  payload->set_sender_id("user");
  payload->mutable_conversation()->set_id("123");
  payload->mutable_conversation()->set_account_id("account");
  payload->mutable_conversation()->set_type(static_cast<kiarabot::v1::ConversationRef::Type>(type));
  return result;
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
    // v5 -> v6: session ordering keys gain the conversation type. The stored envelope is
    // decoded to derive the typed key. One legacy key covered a group and a private
    // conversation with the same numeric id, so both typed watermarks inherit it; a
    // watermark with no surviving event cannot be attributed and is dropped.
    const auto v5_path = (directory.path / "v5.db").string();
    {
      const std::string legacy_conversation = "7:adapter7:account3:123";
      const std::string legacy_status = "7:adapter7:account0:";
      kiarabot::core::RuntimeSnapshot legacy;
      const auto group = message_envelope("group-event", 5, kiarabot::v1::ConversationRef::GROUP);
      const auto private_conversation =
          message_envelope("private-event", 5, kiarabot::v1::ConversationRef::PRIVATE);
      kiarabot::v1::EventEnvelope status;
      status.set_event_id("status-event");
      status.set_trace_id("trace");
      status.set_schema_version("kiarabot.v1");
      status.mutable_occurred_at()->set_seconds(1000);
      status.set_delivery_mode(kiarabot::v1::AT_LEAST_ONCE);
      status.set_sequence(2);
      status.mutable_source()->set_adapter_id("adapter");
      status.mutable_source()->set_account_id("account");
      status.mutable_adapter_status_changed()->set_adapter_id("adapter");
      status.mutable_adapter_status_changed()->set_state(kiarabot::v1::HEALTHY);
      auto event_entry = [&](std::uint64_t sequence, const std::string& session_key,
                             const kiarabot::v1::EventEnvelope& envelope) {
        return kiarabot::core::EventEntry{{envelope.event_id(), "trace", session_key, sequence,
                                           true, envelope.SerializeAsString()},
                                          kiarabot::core::DeliveryState::persisted,
                                          0,
                                          RuntimeTimePoint{},
                                          now,
                                          true};
      };
      legacy.events.emplace("group-event", event_entry(5, legacy_conversation, group));
      legacy.events.emplace("private-event",
                            event_entry(5, legacy_conversation, private_conversation));
      legacy.events.emplace("status-event", event_entry(2, legacy_status, status));
      legacy.deliveries["group-event"]["plugin"] = {"group-event",
                                                    "plugin",
                                                    legacy_conversation,
                                                    "trace",
                                                    1,
                                                    true,
                                                    kiarabot::core::DeliveryState::persisted,
                                                    0,
                                                    RuntimeTimePoint{},
                                                    now};
      legacy.deliveries["private-event"]["plugin"] = {"private-event",
                                                      "plugin",
                                                      legacy_conversation,
                                                      "trace",
                                                      2,
                                                      true,
                                                      kiarabot::core::DeliveryState::persisted,
                                                      0,
                                                      RuntimeTimePoint{},
                                                      now};
      legacy.sessions[legacy_conversation] = {5, true, true};
      legacy.sessions[legacy_status] = {2, true, false};
      legacy.sessions["legacy-watermark-without-event"] = {3, true, false};
      legacy.delivery_order = 2;
      write_snapshot(v5_path, kiarabot::infrastructure::detail::encode(legacy, 5), 5);
    }
    auto from_v5 = value(SqliteRuntimeStore::open(v5_path));
    check(user_version(v5_path) == 6);
    const auto v5_state = value(from_v5->read());
    const std::string group_key = "7:adapter7:account3:1231:2";
    const std::string private_key = "7:adapter7:account3:1231:1";
    const std::string status_key = "7:adapter7:account0:1:0";
    check(v5_state.sessions.size() == 3);
    check(v5_state.sessions.at(group_key).last_sequence == 5 &&
          v5_state.sessions.at(group_key).has_sequence && v5_state.sessions.at(group_key).degraded);
    check(v5_state.sessions.at(private_key).last_sequence == 5 &&
          v5_state.sessions.at(private_key).has_sequence);
    check(v5_state.sessions.at(status_key).last_sequence == 2 &&
          v5_state.sessions.at(status_key).has_sequence);
    check(v5_state.events.at("group-event").record.session_key == group_key);
    check(v5_state.events.at("private-event").record.session_key == private_key);
    check(v5_state.events.at("status-event").record.session_key == status_key);
    check(v5_state.deliveries.at("group-event").at("plugin").session_key == group_key);
    check(v5_state.deliveries.at("private-event").at("plugin").session_key == private_key);
    auto reopened_v5 = value(SqliteRuntimeStore::open(v5_path));
    check(user_version(v5_path) == 6 && value(reopened_v5->read()).sessions.size() == 3);
    {
      auto store = value(SqliteRuntimeStore::open(path));
      Runtime runtime(
          *store, {}, [&] { return now; }, [] { return 0.5; });
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
    Runtime restarted(
        *store, {}, [&] { return now; }, [] { return 0.5; });
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
