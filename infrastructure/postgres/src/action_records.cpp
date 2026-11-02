#include "action_records.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/action_rules.hpp"
#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

inline constexpr std::string_view kDbNow =
    "(extract(epoch FROM clock_timestamp())*1000000000)::bigint";
inline constexpr std::size_t kMaximumPage = 100;
inline constexpr std::size_t kMaximumPruneBatch = 1000;
inline constexpr std::uint32_t kMaximumScopeText = 128;

target::Failure failure(target::ErrorCode code,
                        target::Recovery recovery = target::Recovery::Diagnose) {
  return {code, target::Acceptance::NotAccepted, recovery, {}, {}};
}
target::Failure invalid() {
  return failure(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
}
target::Failure conflict() {
  return failure(target::ErrorCode::Conflict, target::Recovery::ReadCurrent);
}
target::Failure rate_limited() {
  return failure(target::ErrorCode::RateLimited, target::Recovery::Backoff);
}
target::Failure storage_failure() {
  return failure(target::ErrorCode::Internal);
}
bool valid_text_field(std::string_view value, std::size_t maximum) {
  return value.size() <= maximum && valid_sql_text(value);
}
bool valid_action_id(std::string_view value) {
  return !value.empty() && value.size() <= 128 && valid_sql_text(value);
}
bool positive(target::TimePoint value) {
  return value.time_since_epoch().count() > 0;
}
template <typename Value, typename Function>
target::Outcome<Value> query(SqlWork& sql, Function function) {
  std::optional<Value> value;
  auto status = sql.run([&](pqxx::transaction_base& transaction) -> target::Status {
    auto result = function(transaction);
    if (const auto* rejected = std::get_if<target::Failure>(&result)) {
      return *rejected;
    }
    value.emplace(std::move(std::get<Value>(result)));
    return std::monostate{};
  });
  if (const auto* rejected = std::get_if<target::Failure>(&status)) {
    return *rejected;
  }
  if (!value) {
    return storage_failure();
  }
  return std::move(*value);
}
target::TimePoint db_now(pqxx::transaction_base& transaction) {
  return time_field(transaction.exec("SELECT " + std::string{kDbNow} + " AS now").one_row(), "now");
}
int64_t utc_day(target::TimePoint now) {
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  return static_cast<int64_t>(seconds >= 0 ? seconds / 86400 : -((-seconds + 86399) / 86400));
}
target::ActionKind kind_from(int value) {
  if (value < static_cast<int>(target::ActionKind::SendMessage) ||
      value > static_cast<int>(target::ActionKind::ApproveRequest)) {
    throw std::runtime_error("corrupt Action kind");
  }
  return static_cast<target::ActionKind>(value);
}
target::ActionState state_from(int value) {
  if (value < 0 || value > static_cast<int>(target::ActionState::Cancelled)) {
    throw std::runtime_error("corrupt Action state");
  }
  return static_cast<target::ActionState>(value);
}
target::ActionDispatchState dispatch_from(int value) {
  if (value < 0 || value > static_cast<int>(target::ActionDispatchState::Dispatched)) {
    throw std::runtime_error("corrupt Action dispatch state");
  }
  return static_cast<target::ActionDispatchState>(value);
}
target::ActionEvidenceKind evidence_kind_from(int value) {
  if (value < 0 || value > static_cast<int>(target::ActionEvidenceKind::Conflict)) {
    throw std::runtime_error("corrupt Action evidence kind");
  }
  return static_cast<target::ActionEvidenceKind>(value);
}
target::ExecutionOutcome outcome_from(int value) {
  if (value < 0 || value > static_cast<int>(target::ExecutionOutcome::Unknown)) {
    throw std::runtime_error("corrupt Action execution outcome");
  }
  return static_cast<target::ExecutionOutcome>(value);
}
target::ActionRecord read_action(const pqxx::row& row) {
  target::ActionRecord record;
  record.action_id = {row["action_id"].as<std::string>()};
  record.owner_instance = {row["owner_instance"].as<std::string>()};
  record.account_id = {row["account_id"].as<std::string>()};
  record.adapter_id = {row["adapter_id"].as<std::string>()};
  if (!row["dispatched_to_instance"].is_null()) {
    record.dispatched_to_instance =
        target::InstanceId{row["dispatched_to_instance"].as<std::string>()};
  }
  record.binding_version = {row["binding_version"].as<std::uint64_t>()};
  record.kind = kind_from(row["kind"].as<int>());
  record.capability = row["capability"].as<std::string>();
  record.idempotency_key = row["idempotency_key"].as<std::string>();
  record.request_hash = read_digest(row["request_hash"]);
  const auto envelope = row["serialized_envelope"].as<pqxx::bytes>();
  record.serialized_envelope.assign(reinterpret_cast<const char*>(envelope.data()),
                                    envelope.size());
  record.target_conversation = row["target_conversation"].as<std::string>();
  record.target_conversation_kind =
      static_cast<std::uint8_t>(row["target_conversation_kind"].as<int>());
  record.deadline = time_field(row, "deadline");
  record.created_at = time_field(row, "created_at");
  record.updated_at = time_field(row, "updated_at");
  record.state = state_from(row["state"].as<int>());
  record.execution_outcome = outcome_from(row["execution_outcome"].as<int>());
  record.dispatch_state = dispatch_from(row["dispatch_state"].as<int>());
  if (!row["dispatch_attempt_id"].is_null()) {
    record.dispatch_attempt_id = target::AttemptId{row["dispatch_attempt_id"].as<std::string>()};
  }
  record.dispatch_sequence = row["dispatch_sequence"].as<std::uint32_t>();
  if (const auto dispatched = optional_time(row, "dispatched_at")) {
    record.dispatched_at = *dispatched;
  }
  if (!row["platform_message_id"].is_null()) {
    record.platform_message_id = row["platform_message_id"].as<std::string>();
  }
  if (!row["error_code"].is_null()) {
    record.error_code = static_cast<target::ErrorCode>(row["error_code"].as<int>());
  }
  if (const auto completed = optional_time(row, "completed_at")) {
    record.completed_at = *completed;
  }
  record.retain_until = time_field(row, "retain_until");
  return record;
}
target::ActionEvidenceRecord read_evidence(const pqxx::row& row) {
  target::ActionEvidenceRecord evidence;
  evidence.action_id = {row["action_id"].as<std::string>()};
  evidence.dispatch_attempt_id = {row["dispatch_attempt_id"].as<std::string>()};
  evidence.adapter_instance = {row["adapter_instance"].as<std::string>()};
  evidence.kind = evidence_kind_from(row["evidence_kind"].as<int>());
  evidence.execution_outcome = outcome_from(row["execution_outcome"].as<int>());
  if (!row["error_code"].is_null()) {
    evidence.error_code = static_cast<target::ErrorCode>(row["error_code"].as<int>());
  }
  if (!row["platform_reference"].is_null()) {
    evidence.platform_reference = row["platform_reference"].as<std::string>();
  }
  evidence.observed_at = time_field(row, "observed_at");
  evidence.received_at = time_field(row, "received_at");
  evidence.retain_until = time_field(row, "retain_until");
  return evidence;
}
pqxx::params action_params(const target::ActionRecord& record) {
  return pqxx::params{
      record.action_id.value,
      record.owner_instance.value,
      record.account_id.value,
      record.adapter_id.value,
      record.dispatched_to_instance
          ? std::optional<std::string>{record.dispatched_to_instance->value}
          : std::nullopt,
      record.binding_version.value,
      static_cast<int>(record.kind),
      record.capability,
      record.idempotency_key,
      pqxx::binarystring{record.request_hash.bytes.data(), record.request_hash.bytes.size()},
      pqxx::binarystring{record.serialized_envelope},
      record.target_conversation,
      static_cast<int>(record.target_conversation_kind),
      timestamp(record.deadline),
      timestamp(record.created_at),
      timestamp(record.updated_at),
      static_cast<int>(record.state),
      static_cast<int>(record.execution_outcome),
      static_cast<int>(record.dispatch_state),
      record.dispatch_attempt_id ? std::optional<std::string>{record.dispatch_attempt_id->value}
                                 : std::nullopt,
      static_cast<int>(record.dispatch_sequence),
      record.dispatched_at ? std::optional<std::string>{timestamp(*record.dispatched_at)}
                           : std::nullopt,
      record.platform_message_id,
      record.error_code ? std::optional<int>{static_cast<int>(*record.error_code)} : std::nullopt,
      record.completed_at ? std::optional<std::string>{timestamp(*record.completed_at)}
                          : std::nullopt,
      timestamp(record.retain_until)};
}
std::string action_projection() {
  return "action_id,owner_instance,account_id,adapter_id,dispatched_to_instance,binding_version,"
         "kind,capability,idempotency_key,request_hash,serialized_envelope,target_conversation,"
         "target_conversation_kind,deadline,created_at,updated_at,state,execution_outcome,"
         "dispatch_state,dispatch_attempt_id,dispatch_sequence,"
         "dispatched_at,platform_message_id,error_code,completed_at,retain_until";
}
std::vector<std::string> read_groups(pqxx::transaction_base& transaction,
                                     const std::string& account_id) {
  std::vector<std::string> result;
  const auto rows = transaction.exec(
      "SELECT unnest(allowed_group_ids) FROM kiarabot.action_policies WHERE account_id=$1",
      pqxx::params{account_id});
  for (const auto& row : rows) {
    result.push_back(row[0].as<std::string>());
  }
  return result;
}
target::ActionPolicy read_policy(pqxx::transaction_base& transaction, const pqxx::row& row) {
  target::ActionPolicy policy;
  policy.account_id = {row["account_id"].as<std::string>()};
  policy.mute_enabled = row["mute_enabled"].as<bool>();
  policy.kick_enabled = row["kick_enabled"].as<bool>();
  policy.mute_daily_budget = row["mute_daily_budget"].as<std::uint32_t>();
  policy.kick_daily_budget = row["kick_daily_budget"].as<std::uint32_t>();
  policy.version = {row["version"].as<std::uint64_t>()};
  policy.updated_at = time_field(row, "updated_at");
  policy.allowed_group_ids = read_groups(transaction, policy.account_id.value);
  return policy;
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
} // namespace

target::Outcome<std::optional<target::ActionRecord>>
ActionRecords::find_by_id(const target::ActionId& action_id) {
  return query<std::optional<target::ActionRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::ActionRecord>> {
        if (!valid_action_id(action_id.value)) {
          return invalid();
        }
        const auto rows = transaction.exec("SELECT " + action_projection() +
                                               " FROM kiarabot.actions WHERE action_id=$1",
                                           pqxx::params{action_id.value});
        if (rows.empty()) {
          return std::optional<target::ActionRecord>{};
        }
        return std::optional<target::ActionRecord>{read_action(rows.one_row())};
      });
}

target::Outcome<std::optional<target::ActionRecord>>
ActionRecords::find_by_owner_key(const target::InstanceId& owner,
                                 std::string_view idempotency_key) {
  return query<std::optional<target::ActionRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::ActionRecord>> {
        if (!valid_action_id(owner.value) || !valid_text_field(idempotency_key, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT " + action_projection() +
                " FROM kiarabot.actions WHERE owner_instance=$1 AND idempotency_key=$2",
            pqxx::params{owner.value, std::string{idempotency_key}});
        if (rows.empty()) {
          return std::optional<target::ActionRecord>{};
        }
        return std::optional<target::ActionRecord>{read_action(rows.one_row())};
      });
}

target::Outcome<target::ActionRecord> ActionRecords::insert(const target::ActionRecord& record,
                                                            target::ActionCapacity capacity) {
  return query<target::ActionRecord>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::ActionRecord> {
        if (!valid_action_id(record.action_id.value) ||
            !valid_action_id(record.owner_instance.value) ||
            !valid_action_id(record.account_id.value) ||
            !valid_action_id(record.adapter_id.value) || !valid_action_id(record.idempotency_key) ||
            !valid_text_field(record.capability, 128) ||
            !valid_text_field(record.target_conversation, 1024) ||
            record.serialized_envelope.size() > target::kMaximumSerializedEnvelope ||
            record.target_conversation_kind > 2 || !target::known_action_kind(record.kind) ||
            record.binding_version.value == 0 || !positive(record.deadline) ||
            !positive(record.created_at) || !positive(record.updated_at) ||
            !target::action_status_outcome_compatible(record.state, record.execution_outcome) ||
            capacity.max_pending_per_owner == 0 || capacity.max_records == 0) {
          return invalid();
        }
        const auto pending =
            transaction
                .exec("SELECT count(*) FROM kiarabot.actions WHERE owner_instance=$1 AND state=0",
                      pqxx::params{record.owner_instance.value})
                .one_row()[0]
                .as<std::int64_t>();
        const auto total = transaction.exec("SELECT count(*) FROM kiarabot.actions")
                               .one_row()[0]
                               .as<std::int64_t>();
        if (pending >= static_cast<std::int64_t>(capacity.max_pending_per_owner) ||
            total >= static_cast<std::int64_t>(capacity.max_records)) {
          return rate_limited();
        }
        const auto rows = transaction.exec(
            "INSERT INTO kiarabot.actions(action_id,owner_instance,account_id,adapter_id,"
            "dispatched_to_instance,binding_version,kind,capability,idempotency_key,request_hash,"
            "serialized_envelope,target_conversation,target_conversation_kind,deadline,created_at,"
            "updated_at,state,execution_outcome,dispatch_state,dispatch_attempt_id,"
            "dispatch_sequence,dispatched_at,platform_message_id,error_code,completed_at,"
            "retain_until) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,"
            "$18,$19,$20,$21,$22,$23,$24,$25,$26) RETURNING " +
                action_projection(),
            action_params(record));
        if (rows.empty()) {
          return storage_failure();
        }
        transaction.exec(
            "INSERT INTO kiarabot.action_idempotency(owner_instance,idempotency_key,request_hash,"
            "action_id,created_at) VALUES($1,$2,$3,$4,$5)",
            pqxx::params{record.owner_instance.value, record.idempotency_key,
                         pqxx::binarystring{record.request_hash.bytes.data(),
                                            record.request_hash.bytes.size()},
                         record.action_id.value, timestamp(record.created_at)});
        return read_action(rows.one_row());
      });
}

target::Status ActionRecords::mark_dispatched(const target::ActionId& action_id,
                                              const target::AttemptId& attempt_id,
                                              const target::InstanceId& adapter_instance,
                                              target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_action_id(action_id.value) || !valid_action_id(attempt_id.value) ||
        !valid_action_id(adapter_instance.value) || !positive(now)) {
      return invalid();
    }
    const auto rows = transaction.exec(
        "UPDATE kiarabot.actions SET dispatch_attempt_id=$2, dispatched_to_instance=$3,"
        "dispatch_sequence=dispatch_sequence+1, dispatch_state=1, execution_outcome=3,"
        "dispatched_at=$4, updated_at=$4 WHERE action_id=$1 AND state=0 AND dispatch_state=0"
        " AND dispatch_sequence<100 AND deadline>" +
            std::string{kDbNow} + " RETURNING 1",
        pqxx::params{action_id.value, attempt_id.value, adapter_instance.value, timestamp(now)});
    if (rows.affected_rows() != 1) {
      return conflict();
    }
    return std::monostate{};
  });
}

target::Outcome<target::ActionTerminalReceipt> ActionRecords::record_terminal(
    const target::ActionRecord& current, const target::ActionTerminalResult& result,
    std::optional<target::AttemptId> attempt_id, std::optional<target::InstanceId> adapter_instance,
    target::TimePoint now) {
  return query<target::ActionTerminalReceipt>(
      sql_,
      [&](pqxx::transaction_base& transaction) -> target::Outcome<target::ActionTerminalReceipt> {
        if (!valid_action_id(current.action_id.value) || !positive(now)) {
          return invalid();
        }
        const auto rows =
            transaction.exec("SELECT " + action_projection() +
                                 " FROM kiarabot.actions WHERE action_id=$1 FOR UPDATE",
                             pqxx::params{current.action_id.value});
        if (rows.empty()) {
          return failure(target::ErrorCode::NotFound, target::Recovery::ReadCurrent);
        }
        auto record = read_action(rows.one_row());
        auto decision =
            target::apply_action_terminal_result(record, attempt_id, adapter_instance, result, now);
        if (const auto* rejected = std::get_if<target::Failure>(&decision)) {
          return *rejected;
        }
        const auto& decided = std::get<target::ActionTerminalDecision>(decision);
        target::ActionTerminalReceipt receipt{decided.disposition, record, false};
        if (decided.disposition == target::ActionReceiptDisposition::Recorded) {
          const auto update = transaction.exec(
              "UPDATE kiarabot.actions SET state=$2, execution_outcome=$3, platform_message_id=$4,"
              "error_code=$5, completed_at=$6, updated_at=$6, retain_until=$7 WHERE action_id=$1"
              " AND state=0 RETURNING " +
                  action_projection(),
              pqxx::params{record.action_id.value, static_cast<int>(decided.record.state),
                           static_cast<int>(decided.record.execution_outcome),
                           decided.record.platform_message_id,
                           decided.record.error_code
                               ? std::optional<int>{static_cast<int>(*decided.record.error_code)}
                               : std::nullopt,
                           timestamp(now), timestamp(decided.record.retain_until)});
          if (update.affected_rows() != 1) {
            return conflict();
          }
          receipt.record = read_action(update.one_row());
        }
        if (decided.append_evidence) {
          target::ActionEvidenceRecord evidence;
          evidence.action_id = record.action_id;
          evidence.dispatch_attempt_id = *attempt_id;
          evidence.adapter_instance = *adapter_instance;
          evidence.kind = decided.evidence_kind;
          evidence.execution_outcome = result.execution_outcome;
          evidence.error_code = result.error_code;
          evidence.platform_reference.reset();
          evidence.observed_at = now;
          evidence.received_at = db_now(transaction);
          evidence.retain_until = target::action_evidence_retain_until(evidence.received_at);
          const auto appended = transaction.exec(
              "INSERT INTO kiarabot.adapter_result_evidence(action_id,dispatch_attempt_id,"
              "evidence_kind,execution_outcome,adapter_id,adapter_instance,observed_at,received_at,"
              "error_code,platform_reference,retain_until) "
              "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT DO NOTHING",
              pqxx::params{
                  evidence.action_id.value, evidence.dispatch_attempt_id.value,
                  static_cast<int>(evidence.kind), static_cast<int>(evidence.execution_outcome),
                  record.adapter_id.value, evidence.adapter_instance.value,
                  timestamp(evidence.observed_at), timestamp(evidence.received_at),
                  evidence.error_code ? std::optional<int>{static_cast<int>(*evidence.error_code)}
                                      : std::nullopt,
                  evidence.platform_reference, timestamp(evidence.retain_until)});
          receipt.evidence_appended = appended.affected_rows() == 1;
        }
        return receipt;
      });
}

target::Status ActionRecords::cancel(const target::ActionId& action_id, target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_action_id(action_id.value) || !positive(now)) {
      return invalid();
    }
    const auto updated = transaction.exec(
        "UPDATE kiarabot.actions SET state=4, execution_outcome=0, updated_at=$2, completed_at=$2,"
        "retain_until=$3 WHERE action_id=$1 AND state=0 AND dispatch_state=0 RETURNING 1",
        pqxx::params{action_id.value, timestamp(now), timestamp(now)});
    if (updated.affected_rows() == 1) {
      return std::monostate{};
    }
    const auto rows = transaction.exec("SELECT state FROM kiarabot.actions WHERE action_id=$1",
                                       pqxx::params{action_id.value});
    if (rows.empty()) {
      return failure(target::ErrorCode::NotFound, target::Recovery::ReadCurrent);
    }
    const auto state = state_from(rows.one_row()[0].as<int>());
    if (state == target::ActionState::Cancelled || state == target::ActionState::Expired) {
      return std::monostate{};
    }
    return conflict();
  });
}

target::Outcome<std::vector<target::ActionRecord>>
ActionRecords::expire_due(std::size_t limit, target::TimePoint now) {
  return query<std::vector<target::ActionRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::ActionRecord>> {
        if (limit == 0 || limit > kMaximumPruneBatch || !positive(now)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "UPDATE kiarabot.actions SET state=3, execution_outcome=CASE WHEN dispatch_state=1 "
            "THEN 3 ELSE 0 END, error_code=6, updated_at=$1, completed_at=$1, retain_until=$2 "
            "WHERE action_id IN (SELECT action_id FROM kiarabot.actions WHERE state=0 AND "
            "deadline<=" +
                std::string{kDbNow} +
                " ORDER BY deadline LIMIT $3 FOR UPDATE SKIP LOCKED)"
                " RETURNING " +
                action_projection(),
            pqxx::params{timestamp(now), timestamp(target::action_terminal_retain_until(now)),
                         static_cast<int>(limit)});
        std::vector<target::ActionRecord> result;
        for (const auto& row : rows) {
          result.push_back(read_action(row));
        }
        return result;
      });
}

target::Outcome<std::vector<target::ActionRecord>>
ActionRecords::list_owner(const target::InstanceId& owner, const target::ActionPageRequest& page) {
  return query<std::vector<target::ActionRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::ActionRecord>> {
        if (!valid_action_id(owner.value) || page.limit == 0 || page.limit > kMaximumPage ||
            !valid_text_field(page.after_action_id, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT " + action_projection() +
                " FROM kiarabot.actions WHERE owner_instance=$1 AND ($2='' OR action_id>$2)"
                " ORDER BY action_id LIMIT $3",
            pqxx::params{owner.value, page.after_action_id, static_cast<int>(page.limit)});
        std::vector<target::ActionRecord> result;
        for (const auto& row : rows) {
          result.push_back(read_action(row));
        }
        return result;
      });
}

target::Outcome<std::vector<target::ActionRecord>>
ActionRecords::list_control(const target::ActionControlQuery& query_request) {
  return query<std::vector<target::ActionRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::ActionRecord>> {
        if (query_request.scope.resource.kind != target::ResourceKind::Account ||
            !valid_action_id(query_request.scope.resource.id.value) ||
            query_request.page.limit == 0 || query_request.page.limit > kMaximumPage ||
            !valid_text_field(query_request.page.after_action_id, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT " + action_projection() +
                " FROM kiarabot.actions WHERE account_id=$1 AND ($2='' OR action_id>$2)"
                " AND ($3::smallint IS NULL OR state=$3) AND ($4::smallint IS NULL OR kind=$4)"
                " ORDER BY action_id LIMIT $5",
            pqxx::params{
                query_request.scope.resource.id.value, query_request.page.after_action_id,
                query_request.state ? std::optional<int>{static_cast<int>(*query_request.state)}
                                    : std::nullopt,
                query_request.kind ? std::optional<int>{static_cast<int>(*query_request.kind)}
                                   : std::nullopt,
                static_cast<int>(query_request.page.limit)});
        std::vector<target::ActionRecord> result;
        for (const auto& row : rows) {
          result.push_back(read_action(row));
        }
        return result;
      });
}

target::Outcome<std::size_t> ActionRecords::prune_terminal(target::TimePoint now,
                                                           std::size_t limit) {
  return query<std::size_t>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
        if (limit == 0 || limit > kMaximumPruneBatch || !positive(now)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "DELETE FROM kiarabot.actions WHERE action_id IN (SELECT action_id FROM"
            " kiarabot.actions WHERE state<>0 AND retain_until<=" +
                std::string{kDbNow} + " ORDER BY retain_until LIMIT $1) RETURNING 1",
            pqxx::params{static_cast<int>(limit)});
        return static_cast<std::size_t>(rows.affected_rows());
      });
}

target::Status ActionQuotaRecords::admit(const target::ActionId& action_id, target::ActionKind kind,
                                         std::span<const target::ActionScope> scopes,
                                         const target::ActionQuotaLimits& limits,
                                         target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_action_id(action_id.value) || !target::known_action_kind(kind) || scopes.empty() ||
        scopes.size() > 3 || !positive(now)) {
      return invalid();
    }
    std::vector<target::ActionScope> ordered(scopes.begin(), scopes.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const target::ActionScope& left, const target::ActionScope& right) {
                return std::tie(left.kind, left.id) < std::tie(right.kind, right.id);
              });
    const auto database_now = db_now(transaction);
    for (const auto& scope : ordered) {
      if (!valid_text_field(scope.id, kMaximumScopeText)) {
        return invalid();
      }
    }
    const auto budget_index = static_cast<std::size_t>(kind) - 1;
    if (budget_index >= limits.daily_budget.size() || limits.daily_budget[budget_index] == 0) {
      return rate_limited();
    }
    std::optional<std::string> account_scope;
    for (const auto& scope : ordered) {
      const auto& window = scope.kind == target::ActionScopeKind::Account ? limits.account
                           : scope.kind == target::ActionScopeKind::Group
                               ? limits.group
                               : limits.private_conversation;
      if (window.maximum == 0 || window.window.count() <= 0) {
        return rate_limited();
      }
      const auto count =
          transaction
              .exec("SELECT count(*) FROM kiarabot.quota_windows WHERE kind=$1 AND scope_kind=$2"
                    " AND scope_id=$3 AND accepted_at>$4",
                    pqxx::params{static_cast<int>(kind), static_cast<int>(scope.kind), scope.id,
                                 timestamp(database_now - window.window)})
              .one_row()[0]
              .as<std::int64_t>();
      if (count >= static_cast<std::int64_t>(window.maximum)) {
        return rate_limited();
      }
      if (scope.kind == target::ActionScopeKind::Account) {
        account_scope = scope.id;
      }
    }
    if (!account_scope) {
      return invalid();
    }
    const auto day = utc_day(database_now);
    const auto used = transaction.exec(
        "INSERT INTO kiarabot.daily_budgets(kind,account_id,utc_day,used) VALUES($1,$2,$3,1)"
        " ON CONFLICT(kind,account_id,utc_day) DO UPDATE SET used=kiarabot.daily_budgets.used+1"
        " WHERE kiarabot.daily_budgets.used<$4 RETURNING used",
        pqxx::params{static_cast<int>(kind), *account_scope, day,
                     static_cast<int>(limits.daily_budget[budget_index])});
    if (used.empty()) {
      return rate_limited();
    }
    for (const auto& scope : ordered) {
      transaction.exec(
          "INSERT INTO kiarabot.quota_windows(action_id,kind,scope_kind,scope_id,accepted_at)"
          " VALUES($1,$2,$3,$4,$5)",
          pqxx::params{action_id.value, static_cast<int>(kind), static_cast<int>(scope.kind),
                       scope.id, timestamp(database_now)});
    }
    return std::monostate{};
  });
}

target::Outcome<std::size_t> ActionQuotaRecords::prune_windows(target::TimePoint now,
                                                               std::size_t limit) {
  return query<std::size_t>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
        if (limit == 0 || limit > kMaximumPruneBatch || !positive(now)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "DELETE FROM kiarabot.quota_windows WHERE action_id IN (SELECT action_id FROM"
            " kiarabot.quota_windows WHERE accepted_at<" +
                std::string{kDbNow} +
                " - 3600000000000 ORDER BY accepted_at LIMIT $1)"
                " RETURNING 1",
            pqxx::params{static_cast<int>(limit)});
        return static_cast<std::size_t>(rows.affected_rows());
      });
}

target::Outcome<std::optional<target::ActionPolicy>>
ActionPolicyRecords::get(const target::ResourceId& account_id) {
  return query<std::optional<target::ActionPolicy>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::ActionPolicy>> {
        if (!valid_action_id(account_id.value)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT account_id,mute_enabled,kick_enabled,allowed_group_ids,mute_daily_budget,"
            "kick_daily_budget,version,updated_at FROM kiarabot.action_policies WHERE "
            "account_id=$1",
            pqxx::params{account_id.value});
        if (rows.empty()) {
          return std::optional<target::ActionPolicy>{};
        }
        return std::optional<target::ActionPolicy>{read_policy(transaction, rows.one_row())};
      });
}

target::Status ActionPolicyRecords::upsert(const target::ActionPolicy& policy,
                                           std::optional<target::ResourceVersion> expected) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_action_id(policy.account_id.value) || !positive(policy.updated_at) ||
        policy.version.value == 0 || policy.mute_daily_budget > 1000000 ||
        policy.kick_daily_budget > 1000000) {
      return invalid();
    }
    for (const auto& group : policy.allowed_group_ids) {
      if (!valid_text_field(group, kMaximumScopeText)) {
        return invalid();
      }
    }
    const auto groups = text_array_literal(transaction, policy.allowed_group_ids);
    if (!expected) {
      if (policy.version.value != 1) {
        return invalid();
      }
      const auto inserted = transaction.exec(
          "INSERT INTO kiarabot.action_policies(account_id,mute_enabled,kick_enabled,"
          "allowed_group_ids,mute_daily_budget,kick_daily_budget,version,updated_at)"
          " VALUES($1,$2,$3,$4::text[],$5,$6,$7,$8) ON CONFLICT(account_id) DO NOTHING"
          " RETURNING 1",
          pqxx::params{policy.account_id.value, policy.mute_enabled, policy.kick_enabled, groups,
                       static_cast<int>(policy.mute_daily_budget),
                       static_cast<int>(policy.kick_daily_budget), policy.version.value,
                       timestamp(policy.updated_at)});
      if (inserted.affected_rows() == 1) {
        return std::monostate{};
      }
      return conflict();
    }
    if (policy.version.value <= expected->value) {
      return invalid();
    }
    const auto updated = transaction.exec(
        "UPDATE kiarabot.action_policies SET mute_enabled=$3, kick_enabled=$4,"
        "allowed_group_ids=$5::text[], mute_daily_budget=$6, kick_daily_budget=$7,"
        "version=$8, updated_at=$9 WHERE account_id=$1 AND version=$2 RETURNING 1",
        pqxx::params{policy.account_id.value, expected->value, policy.mute_enabled,
                     policy.kick_enabled, groups, static_cast<int>(policy.mute_daily_budget),
                     static_cast<int>(policy.kick_daily_budget), policy.version.value,
                     timestamp(policy.updated_at)});
    if (updated.affected_rows() == 1) {
      return std::monostate{};
    }
    return conflict();
  });
}

target::Status ActionEvidenceRecords::append(const target::ActionEvidenceRecord& evidence) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_action_id(evidence.action_id.value) ||
        !valid_action_id(evidence.dispatch_attempt_id.value) ||
        !valid_action_id(evidence.adapter_instance.value) || !positive(evidence.observed_at) ||
        !positive(evidence.received_at) || !positive(evidence.retain_until) ||
        (evidence.platform_reference && evidence.platform_reference->size() > 256) ||
        !valid_text_field(evidence.platform_reference.value_or(std::string{}), 256)) {
      return invalid();
    }
    const auto adapter =
        transaction.exec("SELECT adapter_id FROM kiarabot.actions WHERE action_id=$1",
                         pqxx::params{evidence.action_id.value});
    if (adapter.empty()) {
      return failure(target::ErrorCode::NotFound, target::Recovery::ReadCurrent);
    }
    transaction.exec(
        "INSERT INTO kiarabot.adapter_result_evidence(action_id,dispatch_attempt_id,"
        "evidence_kind,execution_outcome,adapter_id,adapter_instance,observed_at,received_at,"
        "error_code,platform_reference,retain_until) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)",
        pqxx::params{evidence.action_id.value, evidence.dispatch_attempt_id.value,
                     static_cast<int>(evidence.kind), static_cast<int>(evidence.execution_outcome),
                     adapter.one_row()[0].as<std::string>(), evidence.adapter_instance.value,
                     timestamp(evidence.observed_at), timestamp(evidence.received_at),
                     evidence.error_code
                         ? std::optional<int>{static_cast<int>(*evidence.error_code)}
                         : std::nullopt,
                     evidence.platform_reference, timestamp(evidence.retain_until)});
    return std::monostate{};
  });
}

target::Outcome<std::vector<target::ActionEvidenceRecord>>
ActionEvidenceRecords::list(const target::ActionId& action_id) {
  return query<std::vector<target::ActionEvidenceRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::ActionEvidenceRecord>> {
        if (!valid_action_id(action_id.value)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT action_id,dispatch_attempt_id,evidence_kind,execution_outcome,adapter_instance,"
            "observed_at,received_at,error_code,platform_reference,retain_until"
            " FROM kiarabot.adapter_result_evidence WHERE action_id=$1"
            " ORDER BY received_at,dispatch_attempt_id,evidence_kind",
            pqxx::params{action_id.value});
        std::vector<target::ActionEvidenceRecord> result;
        for (const auto& row : rows) {
          result.push_back(read_evidence(row));
        }
        return result;
      });
}

target::Outcome<std::size_t> ActionEvidenceRecords::prune(target::TimePoint now,
                                                          std::size_t limit) {
  return query<std::size_t>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
        if (limit == 0 || limit > kMaximumPruneBatch || !positive(now)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "DELETE FROM kiarabot.adapter_result_evidence WHERE ctid IN (SELECT ctid FROM"
            " kiarabot.adapter_result_evidence WHERE retain_until<=" +
                std::string{kDbNow} + " ORDER BY retain_until LIMIT $1) RETURNING 1",
            pqxx::params{static_cast<int>(limit)});
        return static_cast<std::size_t>(rows.affected_rows());
      });
}

} // namespace kiarabot::infrastructure::postgres::detail
