#include "host_command_outbox.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pqxx/pqxx>

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

target::Failure failed(target::ErrorCode code,
                       target::Recovery recovery = target::Recovery::Diagnose) {
  return {code, target::Acceptance::NotAccepted, recovery, {}, {}};
}
target::Failure invalid() {
  return failed(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
}
target::Failure conflict() {
  return failed(target::ErrorCode::Conflict, target::Recovery::ReadCurrent);
}
bool valid_text(std::string_view value) {
  return value.find('\0') == std::string_view::npos;
}
bool valid_id(std::string_view value) {
  return !value.empty() && value.size() <= 128 && valid_text(value);
}
bool hex_digest(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}
std::int64_t ns(target::TimePoint value) {
  return static_cast<std::int64_t>(value.time_since_epoch().count());
}
target::TimePoint from_ns(std::int64_t value) {
  return target::TimePoint{std::chrono::nanoseconds{value}};
}
template <typename Value, typename Function>
target::Outcome<Value> query(SqlWork& sql, Function function) {
  std::optional<Value> value;
  const auto status = sql.run([&](pqxx::transaction_base& tx) -> target::Status {
    auto result = function(tx);
    if (const auto* failure = std::get_if<target::Failure>(&result)) {
      return *failure;
    }
    value.emplace(std::move(std::get<Value>(result)));
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<target::Failure>(&status)) {
    return *failure;
  }
  if (!value) {
    return failed(target::ErrorCode::Internal);
  }
  return std::move(*value);
}
std::string text_array_literal(pqxx::transaction_base& transaction,
                               const std::vector<std::string>& values) {
  if (values.empty()) {
    return "{}";
  }
  std::string literal = "{";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      literal += ',';
    }
    literal += transaction.quote(values[index]);
  }
  literal += '}';
  return literal;
}
std::vector<std::string> read_array(pqxx::transaction_base& transaction,
                                    const std::string& command_id, const char* column) {
  std::vector<std::string> values;
  const auto rows = transaction.exec("SELECT unnest(" + std::string(column) +
                                         ") FROM kiarabot.host_commands WHERE command_id=$1",
                                     pqxx::params{command_id});
  for (const auto& row : rows) {
    values.push_back(row[0].as<std::string>());
  }
  return values;
}
bool valid_state(target::HostCommandDeliveryState state) {
  return static_cast<int>(state) >= 0 &&
         static_cast<int>(state) <= static_cast<int>(target::HostCommandDeliveryState::Unknown);
}
target::HostCommandOutboxRecord read_row(pqxx::transaction_base& transaction,
                                         const pqxx::row& row) {
  target::HostCommandOutboxRecord record;
  record.command_id = row["command_id"].as<std::string>();
  record.host_id = row["host_id"].as<std::string>();
  record.instance_id = row["instance_id"].as<std::string>();
  record.kind = static_cast<target::HostCommandKind>(row["kind"].as<int>());
  record.execution_generation = row["execution_generation"].as<std::uint64_t>();
  record.desired_version = row["desired_version"].as<std::uint64_t>();
  record.desired_state = static_cast<target::DesiredState>(row["desired_state"].as<int>());
  record.package_sha256 = row["package_sha256"].as<std::string>();
  record.launch_profile = row["launch_profile"].as<std::string>();
  record.arguments = read_array(transaction, record.command_id, "arguments");
  record.environment_names = read_array(transaction, record.command_id, "environment_names");
  record.payload_sha256 = row["payload_sha256"].as<std::string>();
  record.state = static_cast<target::HostCommandDeliveryState>(row["state"].as<int>());
  record.connection_generation = row["connection_generation"].as<std::uint64_t>();
  record.runtime_epoch = row["runtime_epoch"].as<std::string>();
  record.created_at = from_ns(row["created_at"].as<std::int64_t>());
  record.updated_at = from_ns(row["updated_at"].as<std::int64_t>());
  record.dispatched_at = from_ns(row["dispatched_at"].as<std::int64_t>());
  record.receipted_at = from_ns(row["receipted_at"].as<std::int64_t>());
  if (!row["receipt_persisted"].is_null()) {
    record.receipt_persisted = row["receipt_persisted"].as<bool>();
  }
  if (!row["receipt_outcome"].is_null()) {
    record.receipt_outcome =
        static_cast<target::ExecutionOutcome>(row["receipt_outcome"].as<int>());
  }
  if (!row["receipt_process_marker"].is_null()) {
    record.receipt_process_marker = row["receipt_process_marker"].as<std::string>();
  }
  return record;
}
target::HostObservationRecord read_observation(const pqxx::row& row) {
  target::HostObservationRecord record;
  record.host_id = row["host_id"].as<std::string>();
  record.connection_generation = row["connection_generation"].as<std::uint64_t>();
  record.sequence = row["sequence"].as<std::uint64_t>();
  record.instance_id = row["instance_id"].as<std::string>();
  record.execution_generation = row["execution_generation"].as<std::uint64_t>();
  record.outcome = static_cast<target::ExecutionOutcome>(row["outcome"].as<int>());
  record.process_marker = row["process_marker"].as<std::string>();
  record.command_id = row["command_id"].as<std::string>();
  record.observed_at = from_ns(row["observed_at"].as<std::int64_t>());
  return record;
}
constexpr std::string_view kColumns =
    "command_id,host_id,instance_id,kind,execution_generation,desired_version,desired_state,"
    "package_sha256,launch_profile,payload_sha256,state,connection_generation,runtime_epoch,"
    "created_at,updated_at,dispatched_at,receipted_at,receipt_persisted,receipt_outcome,"
    "receipt_process_marker";
} // namespace

target::Outcome<target::PersistedHostCommand>
HostCommandOutbox::insert(const target::HostCommandOutboxRecord& record) {
  return query<target::PersistedHostCommand>(
      sql_,
      [&](pqxx::transaction_base& transaction) -> target::Outcome<target::PersistedHostCommand> {
        if (!valid_id(record.command_id) || !valid_id(record.host_id) ||
            !valid_id(record.instance_id) || !hex_digest(record.payload_sha256) ||
            !valid_state(record.state) || record.execution_generation == 0 ||
            !valid_text(record.package_sha256) || !valid_text(record.launch_profile)) {
          return invalid();
        }
        for (const auto& argument : record.arguments) {
          if (!valid_text(argument)) {
            return invalid();
          }
        }
        for (const auto& name : record.environment_names) {
          if (!valid_text(name)) {
            return invalid();
          }
        }
        const auto arguments = text_array_literal(transaction, record.arguments);
        const auto environments = text_array_literal(transaction, record.environment_names);
        const auto inserted = transaction.exec(
            "INSERT INTO kiarabot.host_commands(" + std::string(kColumns) +
                ") VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,"
                "$18,$19,$20) ON CONFLICT(command_id) DO NOTHING RETURNING command_id",
            pqxx::params{record.command_id,
                         record.host_id,
                         record.instance_id,
                         static_cast<int>(record.kind),
                         record.execution_generation,
                         record.desired_version,
                         static_cast<int>(record.desired_state),
                         record.package_sha256,
                         record.launch_profile,
                         record.payload_sha256,
                         static_cast<int>(record.state),
                         record.connection_generation,
                         record.runtime_epoch,
                         ns(record.created_at),
                         ns(record.updated_at),
                         ns(record.dispatched_at),
                         ns(record.receipted_at),
                         record.receipt_persisted,
                         record.receipt_outcome
                             ? std::optional<int>{static_cast<int>(*record.receipt_outcome)}
                             : std::nullopt,
                         record.receipt_process_marker});
        if (inserted.affected_rows() == 1) {
          return target::PersistedHostCommand{record, false};
        }
        const auto existing = transaction.exec(
            "SELECT " + std::string(kColumns) + " FROM kiarabot.host_commands WHERE command_id=$1",
            pqxx::params{record.command_id});
        if (existing.empty()) {
          return failed(target::ErrorCode::Internal);
        }
        auto stored = read_row(transaction, existing.one_row());
        if (stored.payload_sha256 != record.payload_sha256) {
          return conflict();
        }
        return target::PersistedHostCommand{std::move(stored), true};
      });
}

target::Outcome<std::vector<target::HostCommandOutboxRecord>>
HostCommandOutbox::pending(const std::string& host_id, std::uint64_t connection_generation,
                           std::size_t limit) {
  return query<std::vector<target::HostCommandOutboxRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::HostCommandOutboxRecord>> {
        if (!valid_id(host_id) || connection_generation == 0 || limit == 0 || limit > 100) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT " + std::string(kColumns) +
                " FROM kiarabot.host_commands WHERE host_id=$1 AND state<>2"
                " AND (state<>1 OR connection_generation<>$2) ORDER BY created_at LIMIT $3",
            pqxx::params{host_id, connection_generation, static_cast<int>(limit)});
        std::vector<target::HostCommandOutboxRecord> result;
        result.reserve(rows.size());
        for (const auto& row : rows) {
          result.push_back(read_row(transaction, row));
        }
        return result;
      });
}

target::Outcome<std::optional<target::HostCommandOutboxRecord>>
HostCommandOutbox::find(const std::string& command_id) {
  return query<std::optional<target::HostCommandOutboxRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::HostCommandOutboxRecord>> {
        const auto rows = transaction.exec("SELECT " + std::string(kColumns) +
                                               " FROM kiarabot.host_commands WHERE command_id=$1",
                                           pqxx::params{command_id});
        if (rows.empty()) {
          return std::optional<target::HostCommandOutboxRecord>{};
        }
        return std::optional<target::HostCommandOutboxRecord>{
            read_row(transaction, rows.one_row())};
      });
}

target::Status HostCommandOutbox::mark_dispatched(const std::string& command_id,
                                                  std::uint64_t connection_generation,
                                                  std::string_view runtime_epoch,
                                                  target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_id(command_id) || connection_generation == 0 || runtime_epoch.empty() ||
        !valid_text(runtime_epoch)) {
      return invalid();
    }
    const auto updated = transaction.exec(
        "UPDATE kiarabot.host_commands SET state=1,connection_generation=$2,"
        "runtime_epoch=$3,dispatched_at=$4,updated_at=$4 WHERE command_id=$1 AND state<>2"
        " RETURNING 1",
        pqxx::params{command_id, connection_generation, runtime_epoch, ns(now)});
    return updated.affected_rows() == 1 ? target::Status{std::monostate{}}
                                        : target::Status{conflict()};
  });
}

target::Status HostCommandOutbox::record_receipt(const std::string& command_id, bool persisted,
                                                 std::optional<target::ExecutionOutcome> outcome,
                                                 std::optional<std::string> process_marker,
                                                 target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_id(command_id)) {
      return invalid();
    }
    if (outcome &&
        (static_cast<int>(*outcome) < 0 ||
         static_cast<int>(*outcome) > static_cast<int>(target::ExecutionOutcome::Unknown))) {
      return invalid();
    }
    if (process_marker && !valid_text(*process_marker)) {
      return invalid();
    }
    const auto updated = transaction.exec(
        "UPDATE kiarabot.host_commands SET state=2,receipt_persisted=$2,receipt_outcome=$3,"
        "receipt_process_marker=$4,receipted_at=$5,updated_at=$5 WHERE command_id=$1"
        " RETURNING 1",
        pqxx::params{command_id, persisted,
                     outcome ? std::optional<int>{static_cast<int>(*outcome)} : std::nullopt,
                     process_marker, ns(now)});
    return updated.affected_rows() == 1 ? target::Status{std::monostate{}}
                                        : target::Status{failed(target::ErrorCode::NotFound)};
  });
}

target::Status HostCommandOutbox::mark_unknown(const std::string& command_id,
                                               target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_id(command_id)) {
      return invalid();
    }
    const auto updated = transaction.exec(
        "UPDATE kiarabot.host_commands SET state=3,updated_at=$2 WHERE command_id=$1"
        " AND state<>2 RETURNING 1",
        pqxx::params{command_id, ns(now)});
    return updated.affected_rows() == 1 ? target::Status{std::monostate{}}
                                        : target::Status{conflict()};
  });
}

target::Outcome<bool>
HostCommandOutbox::record_observation(const target::HostObservationRecord& observation) {
  return query<bool>(sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<bool> {
    if (!valid_id(observation.host_id) || observation.connection_generation == 0 ||
        observation.sequence == 0 || !valid_id(observation.instance_id) ||
        observation.execution_generation == 0 || static_cast<int>(observation.outcome) < 0 ||
        static_cast<int>(observation.outcome) >
            static_cast<int>(target::ExecutionOutcome::Unknown)) {
      return invalid();
    }
    const auto inserted = transaction.exec(
        "INSERT INTO kiarabot.host_observations(host_id,connection_generation,sequence,"
        "instance_id,execution_generation,outcome,process_marker,command_id,observed_at)"
        " VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9) ON CONFLICT DO NOTHING RETURNING 1",
        pqxx::params{observation.host_id, observation.connection_generation, observation.sequence,
                     observation.instance_id, observation.execution_generation,
                     static_cast<int>(observation.outcome), observation.process_marker,
                     observation.command_id, ns(observation.observed_at)});
    return inserted.affected_rows() == 1 ? false : true;
  });
}

target::Outcome<std::vector<target::HostObservationRecord>>
HostCommandOutbox::observations(const std::string& host_id, std::uint64_t connection_generation,
                                std::size_t limit) {
  return query<std::vector<target::HostObservationRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::HostObservationRecord>> {
        if (!valid_id(host_id) || connection_generation == 0 || limit == 0 || limit > 100) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT host_id,connection_generation,sequence,instance_id,execution_generation,"
            "outcome,process_marker,command_id,observed_at FROM kiarabot.host_observations"
            " WHERE host_id=$1 AND connection_generation=$2 ORDER BY sequence LIMIT $3",
            pqxx::params{host_id, connection_generation, static_cast<int>(limit)});
        std::vector<target::HostObservationRecord> result;
        result.reserve(rows.size());
        for (const auto& row : rows) {
          result.push_back(read_observation(row));
        }
        return result;
      });
}

} // namespace kiarabot::infrastructure::postgres::detail
