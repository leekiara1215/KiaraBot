#include "kiarabot/infrastructure/host/host_journal.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace kiarabot::infrastructure::host {
namespace {

namespace t = core::target;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure conflict() {
  return failure(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
}
t::Failure internal() {
  return failure(t::ErrorCode::Internal);
}
t::Failure upstream() {
  return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
}

std::int64_t nanos(t::TimePoint value) {
  return value.time_since_epoch().count();
}
t::TimePoint from_nanos(std::int64_t value) {
  return t::TimePoint{std::chrono::nanoseconds{value}};
}

class Statement {
public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      statement_ = nullptr;
    }
  }
  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt* get() const {
    return statement_;
  }
  bool valid() const {
    return statement_ != nullptr;
  }
  void text(int index, std::string_view value) {
    sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
  }
  void integer(int index, std::int64_t value) {
    sqlite3_bind_int64(statement_, index, value);
  }
  void nullable_integer(int index, const std::optional<int>& value) {
    if (value) {
      sqlite3_bind_int(statement_, index, *value);
    } else {
      sqlite3_bind_null(statement_, index);
    }
  }
  int step() {
    return sqlite3_step(statement_);
  }
  std::string column_text(int index) const {
    const auto* value = sqlite3_column_text(statement_, index);
    return value == nullptr ? std::string{}
                            : std::string(reinterpret_cast<const char*>(value),
                                          static_cast<std::size_t>(sqlite3_column_bytes(statement_, index)));
  }
  std::int64_t column_int64(int index) const {
    return sqlite3_column_int64(statement_, index);
  }
  int column_int(int index) const {
    return sqlite3_column_int(statement_, index);
  }
  bool column_null(int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL;
  }

private:
  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};

std::string schema() {
  return R"sql(
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS command_journal(
  command_id TEXT PRIMARY KEY,
  payload_sha256 TEXT NOT NULL,
  kind INTEGER NOT NULL,
  state INTEGER NOT NULL,
  instance_id TEXT NOT NULL,
  execution_generation INTEGER NOT NULL,
  receipt TEXT NOT NULL DEFAULT '',
  execution_payload TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  retain_until INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS command_journal_state ON command_journal(state, updated_at);
CREATE TABLE IF NOT EXISTS managed_executions(
  instance_id TEXT NOT NULL,
  execution_generation INTEGER NOT NULL,
  host_id TEXT NOT NULL,
  installation_id TEXT NOT NULL,
  desired_version INTEGER NOT NULL,
  pid INTEGER NOT NULL,
  process_marker TEXT NOT NULL,
  state INTEGER NOT NULL,
  outcome INTEGER NOT NULL,
  exit_code INTEGER,
  exit_signal INTEGER,
  started_at INTEGER NOT NULL,
  exited_at INTEGER NOT NULL,
  observed_at INTEGER NOT NULL,
  command_id TEXT NOT NULL,
  retain_until INTEGER NOT NULL,
  PRIMARY KEY(instance_id, execution_generation)
);
CREATE INDEX IF NOT EXISTS managed_executions_state ON managed_executions(state);
CREATE TABLE IF NOT EXISTS execution_observations(
  observation_id TEXT PRIMARY KEY,
  sequence INTEGER NOT NULL UNIQUE,
  host_id TEXT NOT NULL,
  connection_generation INTEGER NOT NULL DEFAULT 0,
  command_id TEXT NOT NULL,
  execution_payload TEXT NOT NULL,
  reported INTEGER NOT NULL DEFAULT 0,
  observed_at INTEGER NOT NULL,
  retain_until INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS execution_observations_pending
  ON execution_observations(reported, sequence);
)sql";
}

// Compact, wire-free serialization for the embedded execution snapshot. Fields are fixed-order
// and length-prefixed so no delimiter in ids can corrupt a record. This is Host-local storage,
// not a cross-process contract.
std::string encode_execution(const t::ManagedExecution& execution) {
  std::string out;
  const auto append = [&out](std::string_view value) {
    out.append(std::to_string(value.size()));
    out.push_back(':');
    out.append(value);
  };
  append(execution.host_id);
  append(execution.instance_id);
  append(execution.installation_id);
  append(std::to_string(execution.execution_generation));
  append(std::to_string(execution.desired_version));
  append(std::to_string(execution.pid));
  append(execution.process_marker);
  append(std::to_string(static_cast<int>(execution.state)));
  append(std::to_string(static_cast<int>(execution.outcome)));
  append(execution.exit_code ? std::to_string(*execution.exit_code) : std::string{});
  append(execution.exit_signal ? std::to_string(*execution.exit_signal) : std::string{});
  append(std::to_string(nanos(execution.started_at)));
  append(std::to_string(nanos(execution.exited_at)));
  append(std::to_string(nanos(execution.observed_at)));
  append(execution.command_id);
  return out;
}

bool decode_execution(std::string_view payload, t::ManagedExecution& execution) {
  std::size_t offset = 0;
  const auto next = [&payload, &offset](std::string& out) {
    const auto separator = payload.find(':', offset);
    if (separator == std::string_view::npos) {
      return false;
    }
    std::uint64_t length = 0;
    for (const char character : payload.substr(offset, separator - offset)) {
      if (character < '0' || character > '9') {
        return false;
      }
      length = length * 10 + static_cast<std::uint64_t>(character - '0');
      if (length > payload.size()) {
        return false;
      }
    }
    if (separator + 1 + length > payload.size()) {
      return false;
    }
    out.assign(payload.substr(separator + 1, length));
    offset = separator + 1 + static_cast<std::size_t>(length);
    return true;
  };
  std::string value;
  const auto number = [&next](std::int64_t& out) {
    std::string text;
    if (!next(text) || text.empty()) {
      return false;
    }
    std::size_t consumed = 0;
    try {
      out = std::stoll(text, &consumed);
    } catch (...) {
      return false;
    }
    return consumed == text.size();
  };
  std::int64_t generation = 0;
  std::int64_t version = 0;
  std::int64_t pid = 0;
  std::int64_t state = 0;
  std::int64_t outcome = 0;
  std::int64_t started = 0;
  std::int64_t exited = 0;
  std::int64_t observed = 0;
  std::string exit_code;
  std::string exit_signal;
  if (!next(execution.host_id) || !next(execution.instance_id) ||
      !next(execution.installation_id) || !number(generation) || !number(version) || !number(pid) ||
      !next(execution.process_marker) || !number(state) || !number(outcome) || !next(exit_code) ||
      !next(exit_signal) || !number(started) || !number(exited) || !number(observed) ||
      !next(execution.command_id)) {
    return false;
  }
  if (state < 0 || state > static_cast<std::int64_t>(t::ManagedExecutionState::Unknown) ||
      outcome < 0 || outcome > static_cast<std::int64_t>(t::ExecutionOutcome::Unknown)) {
    return false;
  }
  execution.execution_generation = static_cast<std::uint64_t>(generation);
  execution.desired_version = static_cast<std::uint64_t>(version);
  execution.pid = pid;
  execution.state = static_cast<t::ManagedExecutionState>(state);
  execution.outcome = static_cast<t::ExecutionOutcome>(outcome);
  if (!exit_code.empty()) {
    try {
      execution.exit_code = std::stoi(exit_code);
    } catch (...) {
      return false;
    }
  }
  if (!exit_signal.empty()) {
    try {
      execution.exit_signal = std::stoi(exit_signal);
    } catch (...) {
      return false;
    }
  }
  execution.started_at = from_nanos(started);
  execution.exited_at = from_nanos(exited);
  execution.observed_at = from_nanos(observed);
  return true;
}

std::int64_t retention_for(const t::ManagedExecution& execution) {
  const auto base = execution.observed_at.time_since_epoch().count() > 0
                        ? execution.observed_at
                        : execution.started_at;
  const auto horizon = execution.state == t::ManagedExecutionState::Exited
                           ? t::kHostResolvedRetention
                           : t::kHostUnresolvedRetention;
  return nanos(base + horizon);
}

class Journal final : public t::HostJournal {
public:
  explicit Journal(std::string path) : path_(std::move(path)) {}
  ~Journal() override {
    close();
  }
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  t::Status open() override {
    close();
    std::error_code error;
    const std::filesystem::path path(path_);
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error ||
        sqlite3_open_v2(path_.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                                      SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      close();
      return upstream();
    }
    sqlite3_busy_timeout(database_, 2000);
    if (sqlite3_exec(database_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr) !=
            SQLITE_OK ||
        sqlite3_exec(database_, schema().c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
      close();
      return internal();
    }
    return std::monostate{};
  }

  void close() override {
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
  }

  t::Outcome<t::PersistedCommand> persist_command(const t::HostCommandRecord& command) override {
    if (database_ == nullptr || command.command_id.empty() || command.payload_sha256.empty()) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      return upstream();
    }
    Statement existing(database_,
                       "SELECT payload_sha256,kind,state,instance_id,execution_generation,receipt,"
                       "execution_payload,created_at,updated_at,retain_until FROM command_journal "
                       "WHERE command_id=?1;");
    if (!existing.valid()) {
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return internal();
    }
    existing.text(1, command.command_id);
    if (existing.step() == SQLITE_ROW) {
      const auto stored_hash = existing.column_text(0);
      std::string receipt = existing.column_text(5);
      std::string execution_payload = existing.column_text(6);
      t::HostCommandRecord stored;
      stored.command_id = command.command_id;
      stored.payload_sha256 = stored_hash;
      stored.kind = static_cast<t::HostCommandKind>(existing.column_int(1));
      stored.state = static_cast<t::HostCommandState>(existing.column_int(2));
      stored.instance_id = existing.column_text(3);
      stored.execution_generation = static_cast<std::uint64_t>(existing.column_int64(4));
      stored.receipt = std::move(receipt);
      stored.created_at = from_nanos(existing.column_int64(7));
      stored.updated_at = from_nanos(existing.column_int64(8));
      stored.retain_until = from_nanos(existing.column_int64(9));
      t::ManagedExecution execution;
      if (!execution_payload.empty() && decode_execution(execution_payload, execution)) {
        stored.execution = execution;
      }
      sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr);
      if (stored_hash != command.payload_sha256) {
        return conflict();
      }
      return t::PersistedCommand{std::move(stored), true};
    }
    Statement insert(
        database_,
        "INSERT INTO command_journal(command_id,payload_sha256,kind,state,instance_id,"
        "execution_generation,receipt,execution_payload,created_at,updated_at,retain_until) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,'',?8,?9,?10);");
    if (!insert.valid()) {
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return internal();
    }
    insert.text(1, command.command_id);
    insert.text(2, command.payload_sha256);
    insert.integer(3, static_cast<int>(command.kind));
    insert.integer(4, static_cast<int>(command.state));
    insert.text(5, command.instance_id);
    insert.integer(6, static_cast<std::int64_t>(command.execution_generation));
    insert.text(7, command.receipt);
    insert.integer(8, nanos(command.created_at));
    insert.integer(9, nanos(command.updated_at));
    insert.integer(10, nanos(command.retain_until));
    const bool inserted = insert.step() == SQLITE_DONE;
    const int result =
        inserted ? sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr)
                 : sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
    if (!inserted || result != SQLITE_OK) {
      return internal();
    }
    return t::PersistedCommand{command, false};
  }

  t::Status update_command(const t::HostCommandRecord& command) override {
    if (database_ == nullptr) {
      return upstream();
    }
    Statement update(database_,
                     "UPDATE command_journal SET state=?2,receipt=?3,execution_payload=?4,"
                     "updated_at=?5,retain_until=?6 WHERE command_id=?1;");
    if (!update.valid()) {
      return internal();
    }
    update.text(1, command.command_id);
    update.integer(2, static_cast<int>(command.state));
    update.text(3, command.receipt);
    update.text(4, command.execution ? encode_execution(*command.execution) : std::string{});
    update.integer(5, nanos(command.updated_at));
    update.integer(6, nanos(command.retain_until));
    if (update.step() != SQLITE_DONE) {
      return internal();
    }
    return sqlite3_changes(database_) == 1 ? t::Status{std::monostate{}} : internal();
  }

  t::Outcome<std::vector<t::HostCommandRecord>> unfinished_commands() override {
    if (database_ == nullptr) {
      return upstream();
    }
    Statement query(database_,
                    "SELECT command_id,payload_sha256,kind,state,instance_id,"
                    "execution_generation,receipt,execution_payload,created_at,updated_at,"
                    "retain_until FROM command_journal WHERE state IN (0,1,4) "
                    "ORDER BY created_at;");
    if (!query.valid()) {
      return internal();
    }
    std::vector<t::HostCommandRecord> commands;
    while (query.step() == SQLITE_ROW) {
      t::HostCommandRecord command;
      command.command_id = query.column_text(0);
      command.payload_sha256 = query.column_text(1);
      command.kind = static_cast<t::HostCommandKind>(query.column_int(2));
      command.state = static_cast<t::HostCommandState>(query.column_int(3));
      command.instance_id = query.column_text(4);
      command.execution_generation = static_cast<std::uint64_t>(query.column_int64(5));
      command.receipt = query.column_text(6);
      t::ManagedExecution execution;
      if (decode_execution(query.column_text(7), execution)) {
        command.execution = execution;
      }
      command.created_at = from_nanos(query.column_int64(8));
      command.updated_at = from_nanos(query.column_int64(9));
      command.retain_until = from_nanos(query.column_int64(10));
      commands.push_back(std::move(command));
    }
    return commands;
  }

  t::Outcome<std::optional<t::ManagedExecution>> find_execution(
      const t::InstanceId& instance, t::ExecutionGeneration generation) override {
    if (database_ == nullptr) {
      return upstream();
    }
    Statement query(database_,
                    "SELECT instance_id,execution_generation,host_id,installation_id,"
                    "desired_version,pid,process_marker,state,outcome,exit_code,exit_signal,"
                    "started_at,exited_at,observed_at,command_id FROM managed_executions "
                    "WHERE instance_id=?1 AND execution_generation=?2;");
    if (!query.valid()) {
      return internal();
    }
    query.text(1, instance.value);
    query.integer(2, static_cast<std::int64_t>(generation.value));
    if (query.step() != SQLITE_ROW) {
      return std::optional<t::ManagedExecution>{};
    }
    return std::optional<t::ManagedExecution>{read_execution(query)};
  }

  t::Outcome<std::vector<t::ManagedExecution>> active_executions() override {
    if (database_ == nullptr) {
      return upstream();
    }
    Statement query(database_,
                    "SELECT instance_id,execution_generation,host_id,installation_id,"
                    "desired_version,pid,process_marker,state,outcome,exit_code,exit_signal,"
                    "started_at,exited_at,observed_at,command_id FROM managed_executions "
                    "WHERE state<>3 ORDER BY started_at;");
    if (!query.valid()) {
      return internal();
    }
    std::vector<t::ManagedExecution> executions;
    while (query.step() == SQLITE_ROW) {
      executions.push_back(read_execution(query));
    }
    return executions;
  }

  t::Status save_execution(const t::ManagedExecution& execution) override {
    if (database_ == nullptr || execution.instance_id.empty() ||
        execution.execution_generation == 0) {
      return upstream();
    }
    Statement upsert(
        database_,
        "INSERT INTO managed_executions(instance_id,execution_generation,host_id,installation_id,"
        "desired_version,pid,process_marker,state,outcome,exit_code,exit_signal,started_at,"
        "exited_at,observed_at,command_id,retain_until) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16) "
        "ON CONFLICT(instance_id,execution_generation) DO UPDATE SET "
        "host_id=excluded.host_id,installation_id=excluded.installation_id,"
        "desired_version=excluded.desired_version,pid=excluded.pid,"
        "process_marker=excluded.process_marker,state=excluded.state,outcome=excluded.outcome,"
        "exit_code=excluded.exit_code,exit_signal=excluded.exit_signal,"
        "started_at=excluded.started_at,exited_at=excluded.exited_at,"
        "observed_at=excluded.observed_at,command_id=excluded.command_id,"
        "retain_until=excluded.retain_until;");
    if (!upsert.valid()) {
      return internal();
    }
    upsert.text(1, execution.instance_id);
    upsert.integer(2, static_cast<std::int64_t>(execution.execution_generation));
    upsert.text(3, execution.host_id);
    upsert.text(4, execution.installation_id);
    upsert.integer(5, static_cast<std::int64_t>(execution.desired_version));
    upsert.integer(6, execution.pid);
    upsert.text(7, execution.process_marker);
    upsert.integer(8, static_cast<int>(execution.state));
    upsert.integer(9, static_cast<int>(execution.outcome));
    upsert.nullable_integer(10, execution.exit_code);
    upsert.nullable_integer(11, execution.exit_signal);
    upsert.integer(12, nanos(execution.started_at));
    upsert.integer(13, nanos(execution.exited_at));
    upsert.integer(14, nanos(execution.observed_at));
    upsert.text(15, execution.command_id);
    upsert.integer(16, retention_for(execution));
    if (upsert.step() != SQLITE_DONE) {
      return internal();
    }
    return std::monostate{};
  }

  t::Outcome<t::ExecutionObservation> append_observation(
      t::ExecutionObservation observation) override {
    if (database_ == nullptr) {
      return upstream();
    }
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      return upstream();
    }
    Statement next(database_, "SELECT COALESCE(MAX(sequence),0)+1 FROM execution_observations;");
    if (!next.valid() || next.step() != SQLITE_ROW) {
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return internal();
    }
    observation.sequence = static_cast<std::uint64_t>(next.column_int64(0));
    observation.observation_id = std::to_string(observation.sequence);
    observation.reported = false;
    Statement insert(
        database_,
        "INSERT INTO execution_observations(observation_id,sequence,host_id,"
        "connection_generation,command_id,execution_payload,reported,observed_at,retain_until) "
        "VALUES(?1,?2,?3,0,?4,?5,0,?6,?7);");
    if (!insert.valid()) {
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return internal();
    }
    insert.text(1, observation.observation_id);
    insert.integer(2, static_cast<std::int64_t>(observation.sequence));
    insert.text(3, observation.execution.host_id);
    insert.text(4, observation.command_id);
    insert.text(5, encode_execution(observation.execution));
    insert.integer(6, nanos(observation.execution.observed_at));
    insert.integer(
        7, nanos(observation.execution.observed_at + t::kHostResolvedRetention));
    if (insert.step() != SQLITE_DONE ||
        sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return internal();
    }
    return observation;
  }

  t::Outcome<std::vector<t::ExecutionObservation>> pending_observations(
      std::size_t limit) override {
    if (database_ == nullptr || limit == 0 || limit > 256) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    Statement query(database_,
                    "SELECT sequence,observation_id,command_id,execution_payload,reported "
                    "FROM execution_observations WHERE reported=0 ORDER BY sequence LIMIT ?1;");
    if (!query.valid()) {
      return internal();
    }
    query.integer(1, static_cast<std::int64_t>(limit));
    std::vector<t::ExecutionObservation> observations;
    while (query.step() == SQLITE_ROW) {
      t::ExecutionObservation observation;
      observation.sequence = static_cast<std::uint64_t>(query.column_int64(0));
      observation.observation_id = query.column_text(1);
      observation.command_id = query.column_text(2);
      if (!decode_execution(query.column_text(3), observation.execution)) {
        return internal();
      }
      observation.reported = query.column_int(4) != 0;
      observations.push_back(std::move(observation));
    }
    return observations;
  }

  t::Status mark_observation_reported(std::uint64_t sequence) override {
    if (database_ == nullptr) {
      return upstream();
    }
    Statement update(database_,
                     "UPDATE execution_observations SET reported=1 WHERE sequence=?1;");
    if (!update.valid()) {
      return internal();
    }
    update.integer(1, static_cast<std::int64_t>(sequence));
    if (update.step() != SQLITE_DONE) {
      return internal();
    }
    return sqlite3_changes(database_) == 1 ? t::Status{std::monostate{}} : internal();
  }

  t::Outcome<std::uint64_t> last_reported_sequence() override {
    if (database_ == nullptr) {
      return upstream();
    }
    Statement query(database_,
                    "SELECT COALESCE(MAX(sequence),0) FROM execution_observations "
                    "WHERE reported=1;");
    if (!query.valid() || query.step() != SQLITE_ROW) {
      return internal();
    }
    return static_cast<std::uint64_t>(query.column_int64(0));
  }

  t::Outcome<std::size_t> prune(t::TimePoint now, std::size_t limit) override {
    if (database_ == nullptr || limit == 0) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    const auto cutoff = nanos(now);
    const auto bounded = static_cast<std::int64_t>(std::min<std::size_t>(limit, 1000));
    std::size_t removed = 0;
    const auto run = [&](const char* sql) -> bool {
      Statement statement(database_, sql);
      if (!statement.valid()) {
        return false;
      }
      statement.integer(1, cutoff);
      statement.integer(2, bounded);
      if (statement.step() != SQLITE_DONE) {
        return false;
      }
      removed += static_cast<std::size_t>(sqlite3_changes(database_));
      return true;
    };
    if (!run("DELETE FROM command_journal WHERE command_id IN (SELECT command_id FROM "
             "command_journal WHERE state IN (2,3,4) AND retain_until<=?1 LIMIT ?2);") ||
        !run("DELETE FROM execution_observations WHERE observation_id IN (SELECT observation_id "
             "FROM execution_observations WHERE reported=1 AND retain_until<=?1 LIMIT ?2);") ||
        !run("DELETE FROM managed_executions WHERE instance_id IN (SELECT instance_id FROM "
             "managed_executions WHERE state=3 AND retain_until<=?1 LIMIT ?2);")) {
      return internal();
    }
    return removed;
  }

private:
  static t::ManagedExecution read_execution(Statement& query) {
    t::ManagedExecution execution;
    execution.instance_id = query.column_text(0);
    execution.execution_generation = static_cast<std::uint64_t>(query.column_int64(1));
    execution.host_id = query.column_text(2);
    execution.installation_id = query.column_text(3);
    execution.desired_version = static_cast<std::uint64_t>(query.column_int64(4));
    execution.pid = query.column_int64(5);
    execution.process_marker = query.column_text(6);
    execution.state = static_cast<t::ManagedExecutionState>(query.column_int(7));
    execution.outcome = static_cast<t::ExecutionOutcome>(query.column_int(8));
    if (!query.column_null(9)) {
      execution.exit_code = query.column_int(9);
    }
    if (!query.column_null(10)) {
      execution.exit_signal = query.column_int(10);
    }
    execution.started_at = from_nanos(query.column_int64(11));
    execution.exited_at = from_nanos(query.column_int64(12));
    execution.observed_at = from_nanos(query.column_int64(13));
    execution.command_id = query.column_text(14);
    return execution;
  }

  std::string path_;
  sqlite3* database_ = nullptr;
};

} // namespace

std::unique_ptr<core::target::HostJournal> make_sqlite_host_journal(std::string path) {
  return std::make_unique<Journal>(std::move(path));
}

} // namespace kiarabot::infrastructure::host
