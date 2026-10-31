// Capacity boundaries of the private runtime snapshot:
// - an oversized encoded payload fails commit with Unavailable and leaves the
//   durable state unchanged (the transaction may be retried after shrinking);
// - an oversized collection count fails commit the same way;
// - an oversized stored payload is reported as Corrupt on read;
// - every rejection is counted so capacity pressure is observable.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "kiarabot/infrastructure/snapshot_limits.hpp"
#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

namespace {
using kiarabot::core::RuntimeSnapshot;
using kiarabot::core::RuntimeTransaction;
using kiarabot::core::SessionStatus;
using kiarabot::core::StorageError;
using kiarabot::infrastructure::SqliteRuntimeStore;

void check(bool result, const std::source_location& location = std::source_location::current()) {
  if (!result) {
    throw std::runtime_error("Snapshot limit assertion failed at " +
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
    auto pattern = (std::filesystem::temp_directory_path() / "kiarabot-limits-XXXXXX").string();
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
  const int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
  sqlite3_close(database);
  check(result == SQLITE_OK);
}
void write_oversized_payload(const std::string& path, const std::string& payload) {
  sqlite3* database = nullptr;
  check(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(database, "UPDATE runtime_snapshot SET payload=? WHERE id=1", -1,
                           &statement, nullptr) == SQLITE_OK);
  check(sqlite3_bind_blob(statement, 1, payload.data(), static_cast<int>(payload.size()),
                          SQLITE_TRANSIENT) == SQLITE_OK);
  check(sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(database);
}
void bytes_limit(const std::string& path) {
  auto store = value(SqliteRuntimeStore::open(path));
  check(SqliteRuntimeStore::limits().bytes == kiarabot::infrastructure::kMaximumSnapshotBytes);
  check(SqliteRuntimeStore::limits().items == kiarabot::infrastructure::kMaximumSnapshotItems);
  check(store->rejected_snapshots().oversized_bytes == 0);
  {
    auto transaction = value(store->begin());
    transaction->snapshot().events["big"].record.serialized_envelope =
        std::string(kiarabot::infrastructure::kMaximumSnapshotBytes + 1, 'x');
    check(std::get<StorageError>(transaction->commit()) == StorageError::Unavailable);
    check(store->rejected_snapshots().oversized_bytes == 1);
    check(value(store->read()).events.empty());
    // The failed transaction still owns its snapshot and can be retried.
    transaction->snapshot().events.clear();
    check(std::holds_alternative<std::monostate>(transaction->commit()));
  }
  check(value(store->read()).events.empty());
}
void items_limit(const std::string& path) {
  auto store = value(SqliteRuntimeStore::open(path));
  {
    auto transaction = value(store->begin());
    auto& sessions = transaction->snapshot().sessions;
    sessions.reserve(kiarabot::infrastructure::kMaximumSnapshotItems + 1);
    for (std::size_t index = 0; index <= kiarabot::infrastructure::kMaximumSnapshotItems; ++index) {
      sessions.emplace(std::to_string(index), SessionStatus{});
    }
    check(std::get<StorageError>(transaction->commit()) == StorageError::Unavailable);
    check(store->rejected_snapshots().oversized_items == 1);
    check(store->rejected_snapshots().oversized_bytes == 0);
  }
  check(value(store->read()).sessions.empty());
  {
    auto transaction = value(store->begin());
    transaction->snapshot().sessions.emplace("small", SessionStatus{});
    check(std::holds_alternative<std::monostate>(transaction->commit()));
  }
  check(value(store->read()).sessions.size() == 1);
}
void oversized_stored_payload(const std::string& path) {
  auto store = value(SqliteRuntimeStore::open(path));
  {
    auto transaction = value(store->begin());
    transaction->snapshot().delivery_order = 1;
    check(std::holds_alternative<std::monostate>(transaction->commit()));
  }
  write_oversized_payload(path,
                          std::string(kiarabot::infrastructure::kMaximumSnapshotBytes + 1, 'x'));
  check(std::get<StorageError>(store->read()) == StorageError::Corrupt);
  check(store->rejected_snapshots().oversized_reads == 1);
  check(std::get<StorageError>(store->begin()) == StorageError::Corrupt);
  check(store->rejected_snapshots().oversized_reads == 2);
}
} // namespace

int main() {
  try {
    Directory directory;
    bytes_limit((directory.path / "bytes.db").string());
    items_limit((directory.path / "items.db").string());
    oversized_stored_payload((directory.path / "payload.db").string());
    // Counters belong to one store instance: a reopened store starts at zero.
    auto reopened = value(SqliteRuntimeStore::open((directory.path / "items.db").string()));
    check(reopened->rejected_snapshots().oversized_items == 0);
    check(value(reopened->read()).sessions.size() == 1);
    std::cout << "snapshot byte/item limits and rejection counters passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
