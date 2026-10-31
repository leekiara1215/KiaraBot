#include "kiarabot/infrastructure/sqlite_runtime_store.hpp"

#include <atomic>
#include <limits>
#include <mutex>
#include <sqlite3.h>
#include <stdexcept>

#include "runtime_snapshot_codec.hpp"

namespace kiarabot::infrastructure {
namespace {
core::StorageError database_error(sqlite3* database) {
  const int code = sqlite3_errcode(database);
  if (code == SQLITE_BUSY || code == SQLITE_LOCKED) {
    return core::StorageError::Conflict;
  }
  if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB || code == SQLITE_SCHEMA ||
      code == SQLITE_ERROR) {
    return core::StorageError::Corrupt;
  }
  return core::StorageError::Unavailable;
}
struct DatabaseFailure {
  core::StorageError error;
};
class Statement {
public:
  Statement(sqlite3* database, const char* sql) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw DatabaseFailure{database_error(database)};
    }
  }
  ~Statement() {
    sqlite3_finalize(statement_);
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt* get() const {
    return statement_;
  }

private:
  sqlite3_stmt* statement_{nullptr};
};
} // namespace
struct SqliteRuntimeStore::State {
  sqlite3* database{nullptr};
  mutable std::mutex mutex;
  mutable std::atomic<std::uint64_t> oversized_bytes{0};
  mutable std::atomic<std::uint64_t> oversized_items{0};
  mutable std::atomic<std::uint64_t> oversized_reads{0};
  ~State() {
    if (database) {
      sqlite3_close(database);
    }
  }
  core::StorageResult<std::pair<core::RuntimeSnapshot, sqlite3_int64>> load() const {
    try {
      Statement query(database, "SELECT revision,payload FROM runtime_snapshot WHERE id=1");
      const int step = sqlite3_step(query.get());
      if (step != SQLITE_ROW) {
        return step == SQLITE_DONE ? core::StorageError::Corrupt : database_error(database);
      }
      if (sqlite3_column_type(query.get(), 0) != SQLITE_INTEGER ||
          sqlite3_column_type(query.get(), 1) != SQLITE_BLOB) {
        return core::StorageError::Corrupt;
      }
      const auto revision = sqlite3_column_int64(query.get(), 0);
      const int count = sqlite3_column_bytes(query.get(), 1);
      if (count > static_cast<int>(kMaximumSnapshotBytes)) {
        oversized_reads.fetch_add(1, std::memory_order_relaxed);
        return core::StorageError::Corrupt;
      }
      if (revision < 0 || count <= 0) {
        return core::StorageError::Corrupt;
      }
      const auto* bytes = static_cast<const char*>(sqlite3_column_blob(query.get(), 1));
      if (!bytes) {
        return core::StorageError::Corrupt;
      }
      return std::pair{detail::decode(std::string(bytes, static_cast<std::size_t>(count))),
                       revision};
    } catch (const DatabaseFailure& failure) {
      return failure.error;
    } catch (...) {
      return core::StorageError::Corrupt;
    }
  }
};
class SqliteRuntimeStore::Transaction final : public core::RuntimeTransaction {
public:
  Transaction(std::shared_ptr<State> state, core::RuntimeSnapshot snapshot, sqlite3_int64 revision)
      : state_(std::move(state)), snapshot_(std::move(snapshot)), revision_(revision) {}
  core::RuntimeSnapshot& snapshot() override {
    return snapshot_;
  }
  core::StorageStatus commit() override {
    std::lock_guard lock(state_->mutex);
    if (finished_ || revision_ == std::numeric_limits<sqlite3_int64>::max()) {
      return core::StorageError::Conflict;
    }
    try {
      const std::string bytes = detail::encode(snapshot_);
      if (bytes.size() > kMaximumSnapshotBytes) {
        state_->oversized_bytes.fetch_add(1, std::memory_order_relaxed);
        return core::StorageError::Unavailable;
      }
      Statement update(
          state_->database,
          "UPDATE runtime_snapshot SET payload=?,revision=revision+1 WHERE id=1 AND revision=?");
      if (sqlite3_bind_blob(update.get(), 1, bytes.data(), static_cast<int>(bytes.size()),
                            SQLITE_TRANSIENT) != SQLITE_OK ||
          sqlite3_bind_int64(update.get(), 2, revision_) != SQLITE_OK ||
          sqlite3_step(update.get()) != SQLITE_DONE) {
        return database_error(state_->database);
      }
      if (sqlite3_changes(state_->database) != 1) {
        return core::StorageError::Conflict;
      }
      finished_ = true;
      return std::monostate{};
    } catch (const detail::SnapshotLimitError&) {
      state_->oversized_items.fetch_add(1, std::memory_order_relaxed);
      return core::StorageError::Unavailable;
    } catch (const DatabaseFailure& failure) {
      return failure.error;
    } catch (...) {
      return core::StorageError::Unavailable;
    }
  }

private:
  std::shared_ptr<State> state_;
  core::RuntimeSnapshot snapshot_;
  sqlite3_int64 revision_;
  bool finished_{false};
};
SqliteRuntimeStore::SqliteRuntimeStore(std::shared_ptr<State> state) : state_(std::move(state)) {
  if (!state_) {
    throw std::invalid_argument("SQLite state is required");
  }
}
SqliteRuntimeStore::~SqliteRuntimeStore() = default;
SnapshotLimits SqliteRuntimeStore::limits() {
  return {};
}
SnapshotRejections SqliteRuntimeStore::rejected_snapshots() const {
  return {state_->oversized_bytes.load(std::memory_order_relaxed),
          state_->oversized_items.load(std::memory_order_relaxed),
          state_->oversized_reads.load(std::memory_order_relaxed)};
}
core::StorageResult<std::unique_ptr<SqliteRuntimeStore>>
SqliteRuntimeStore::open(const std::string& path) {
  auto state = std::make_shared<State>();
  if (path.empty() ||
      sqlite3_open_v2(path.c_str(), &state->database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return core::StorageError::Unavailable;
  }
  sqlite3_limit(state->database, SQLITE_LIMIT_LENGTH,
                static_cast<int>(kMaximumSnapshotBytes) + 1024);
  if (sqlite3_exec(state->database, "PRAGMA synchronous=FULL; BEGIN IMMEDIATE", nullptr, nullptr,
                   nullptr) != SQLITE_OK) {
    return database_error(state->database);
  }
  auto rollback = [&] { sqlite3_exec(state->database, "ROLLBACK", nullptr, nullptr, nullptr); };
  try {
    int version = 0;
    {
      Statement query(state->database, "PRAGMA user_version");
      if (sqlite3_step(query.get()) != SQLITE_ROW) {
        rollback();
        return core::StorageError::Corrupt;
      }
      version = sqlite3_column_int(query.get(), 0);
    }
    if (version == 0) {
      if (sqlite3_exec(state->database,
                       "CREATE TABLE runtime_snapshot(id INTEGER PRIMARY KEY CHECK(id=1),revision "
                       "INTEGER NOT NULL,payload BLOB NOT NULL)",
                       nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        return core::StorageError::Corrupt;
      }
      const auto bytes = detail::encode(core::RuntimeSnapshot{});
      Statement insert(state->database, "INSERT INTO runtime_snapshot VALUES(1,0,?)");
      if (sqlite3_bind_blob(insert.get(), 1, bytes.data(), static_cast<int>(bytes.size()),
                            SQLITE_TRANSIENT) != SQLITE_OK ||
          sqlite3_step(insert.get()) != SQLITE_DONE ||
          sqlite3_exec(state->database, "PRAGMA user_version=5", nullptr, nullptr, nullptr) !=
              SQLITE_OK) {
        rollback();
        return core::StorageError::Unavailable;
      }
    } else if (version == 1 || version == 2 || version == 3 || version == 4) {
      Statement query(state->database, "SELECT payload FROM runtime_snapshot WHERE id=1");
      if (sqlite3_step(query.get()) != SQLITE_ROW) {
        rollback();
        return core::StorageError::Corrupt;
      }
      const int count = sqlite3_column_bytes(query.get(), 0);
      const auto* raw = static_cast<const char*>(sqlite3_column_blob(query.get(), 0));
      if (!raw || count <= 0 || count > static_cast<int>(kMaximumSnapshotBytes)) {
        rollback();
        return core::StorageError::Corrupt;
      }
      const auto migrated = detail::encode(
          detail::decode(std::string(raw, static_cast<std::size_t>(count)), version));
      Statement update(state->database, "UPDATE runtime_snapshot SET payload=? WHERE id=1");
      if (sqlite3_bind_blob(update.get(), 1, migrated.data(), static_cast<int>(migrated.size()),
                            SQLITE_TRANSIENT) != SQLITE_OK ||
          sqlite3_step(update.get()) != SQLITE_DONE ||
          sqlite3_exec(state->database, "PRAGMA user_version=5", nullptr, nullptr, nullptr) !=
              SQLITE_OK) {
        rollback();
        return core::StorageError::Unavailable;
      }
    } else if (version != 5) {
      rollback();
      return core::StorageError::Corrupt;
    }
    if (std::holds_alternative<core::StorageError>(state->load())) {
      rollback();
      return core::StorageError::Corrupt;
    }
    if (sqlite3_exec(state->database, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
      rollback();
      return database_error(state->database);
    }
    return std::make_unique<SqliteRuntimeStore>(std::move(state));
  } catch (const DatabaseFailure& failure) {
    rollback();
    return failure.error;
  } catch (...) {
    rollback();
    return core::StorageError::Corrupt;
  }
}
core::StorageResult<core::RuntimeSnapshot> SqliteRuntimeStore::read() const {
  std::lock_guard lock(state_->mutex);
  auto result = state_->load();
  if (auto* error = std::get_if<core::StorageError>(&result)) {
    return *error;
  }
  return std::get<0>(std::move(result)).first;
}
core::StorageResult<std::unique_ptr<core::RuntimeTransaction>> SqliteRuntimeStore::begin() {
  std::lock_guard lock(state_->mutex);
  auto result = state_->load();
  if (auto* error = std::get_if<core::StorageError>(&result)) {
    return *error;
  }
  auto [snapshot, revision] = std::get<0>(std::move(result));
  return std::unique_ptr<core::RuntimeTransaction>(
      std::make_unique<Transaction>(state_, std::move(snapshot), revision));
}
} // namespace kiarabot::infrastructure
