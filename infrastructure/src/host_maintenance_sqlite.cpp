#include "kiarabot/infrastructure/host/host_maintenance_sqlite.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace kiarabot::infrastructure::host {
namespace {
namespace t = core::target;
using Json = nlohmann::json;

t::Failure failure(t::ErrorCode code, bool unknown = false) {
  return {code,
          unknown ? t::Acceptance::Unknown : t::Acceptance::NotAccepted,
          unknown ? t::Recovery::QuerySameIdentity : t::Recovery::ReadCurrent,
          {},
          {}};
}
struct StorageError {
  t::Failure failure;
};
[[noreturn]] void storage_error(sqlite3* db) {
  const auto code = sqlite3_errcode(db);
  throw StorageError{
      {code == SQLITE_BUSY || code == SQLITE_LOCKED ? t::ErrorCode::UpstreamUnavailable
                                                    : t::ErrorCode::Internal,
       t::Acceptance::NotAccepted,
       code == SQLITE_BUSY || code == SQLITE_LOCKED ? t::Recovery::Backoff : t::Recovery::Diagnose,
       {},
       {}}};
}
void execute(sqlite3* db, const char* sql) {
  if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
    storage_error(db);
  }
}
class Statement {
public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &value_, nullptr) != SQLITE_OK) {
      storage_error(db);
    }
  }
  ~Statement() {
    if (value_) {
      sqlite3_finalize(value_);
    }
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  void text(int index, std::string_view text) {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_text(value_, index, text.data(), static_cast<int>(text.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      storage_error(db_);
    }
  }
  void number(int index, std::int64_t number) {
    if (sqlite3_bind_int64(value_, index, number) != SQLITE_OK) {
      storage_error(db_);
    }
  }
  bool row() {
    const auto result = sqlite3_step(value_);
    if (result == SQLITE_ROW) {
      return true;
    }
    if (result != SQLITE_DONE) {
      storage_error(db_);
    }
    return false;
  }
  void done() {
    if (row()) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
  }
  std::string text(int index) const {
    const auto* data = sqlite3_column_text(value_, index);
    return data ? std::string{reinterpret_cast<const char*>(data),
                              static_cast<std::size_t>(sqlite3_column_bytes(value_, index))}
                : std::string{};
  }
  std::int64_t number(int index) const {
    return sqlite3_column_int64(value_, index);
  }

private:
  sqlite3* db_;
  sqlite3_stmt* value_ = nullptr;
};
std::int64_t integer(std::uint64_t number) {
  if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw StorageError{failure(t::ErrorCode::Internal)};
  }
  return static_cast<std::int64_t>(number);
}
t::TimePoint time(std::int64_t value) {
  return t::TimePoint{std::chrono::nanoseconds{value}};
}
bool token(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char character) { return character > 32 && character < 127; });
}
bool terminal(t::OperationState state) {
  return state == t::OperationState::Succeeded || state == t::OperationState::Cancelled ||
         state == t::OperationState::Failed || state == t::OperationState::Expired;
}
Json operation_json(const t::Operation& value) {
  Json result{{"id", value.ref.id.value},
              {"host", value.ref.resource.id.value},
              {"version", value.version.value},
              {"state", static_cast<int>(value.state)},
              {"outcome", static_cast<int>(value.execution_outcome)},
              {"created", value.created_at.time_since_epoch().count()},
              {"updated", value.updated_at.time_since_epoch().count()}};
  result["reason"] =
      value.blocked_reason ? Json(static_cast<int>(*value.blocked_reason)) : Json(nullptr);
  result["next"] =
      value.next_attempt ? Json(value.next_attempt->time_since_epoch().count()) : Json(nullptr);
  result["retain"] =
      value.retain_until ? Json(value.retain_until->time_since_epoch().count()) : Json(nullptr);
  result["participants"] = Json::array();
  for (const auto& participant : value.participants) {
    result["participants"].push_back(
        {{"name", participant.participant},
         {"state", static_cast<int>(participant.state)},
         {"reason",
          participant.reason ? Json(static_cast<int>(*participant.reason)) : Json(nullptr)},
         {"next", participant.next_attempt
                      ? Json(participant.next_attempt->time_since_epoch().count())
                      : Json(nullptr)}});
  }
  return result;
}
t::Operation decode_operation(const Json& value) {
  t::Operation result;
  const auto state = value.at("state").get<int>();
  const auto outcome = value.at("outcome").get<int>();
  if (state < 0 || state > 6 || outcome < 0 || outcome > 3) {
    throw StorageError{failure(t::ErrorCode::Internal)};
  }
  result.ref = {t::Service::Host,
                {value.at("id").get<std::string>()},
                {t::ResourceKind::Host, {value.at("host").get<std::string>()}}};
  result.version.value = value.at("version").get<std::uint64_t>();
  result.state = static_cast<t::OperationState>(state);
  result.execution_outcome = static_cast<t::ExecutionOutcome>(outcome);
  result.created_at = time(value.at("created").get<std::int64_t>());
  result.updated_at = time(value.at("updated").get<std::int64_t>());
  const auto reason = [](const Json& value) -> std::optional<t::ErrorCode> {
    if (value.is_null()) {
      return {};
    }
    const auto number = value.get<int>();
    if (number < 0 || number > static_cast<int>(t::ErrorCode::Internal)) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    return static_cast<t::ErrorCode>(number);
  };
  result.blocked_reason = reason(value.at("reason"));
  if (!value.at("next").is_null()) {
    result.next_attempt = time(value.at("next").get<std::int64_t>());
  }
  if (!value.at("retain").is_null()) {
    result.retain_until = time(value.at("retain").get<std::int64_t>());
  }
  for (const auto& part : value.at("participants")) {
    const auto number = part.at("state").get<int>();
    if (number < 0 || number > 6) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    result.participants.push_back(
        {part.at("name").get<std::string>(), static_cast<t::OperationState>(number),
         reason(part.at("reason")),
         part.at("next").is_null()
             ? std::nullopt
             : std::optional<t::TimePoint>{time(part.at("next").get<std::int64_t>())}});
  }
  if (!token(result.ref.id.value) || !token(result.ref.resource.id.value) ||
      result.version.value == 0) {
    throw StorageError{failure(t::ErrorCode::Internal)};
  }
  return result;
}
Json intent_json(const t::HostMaintenanceIntent& value) {
  return {{"action", static_cast<int>(value.action)},
          {"actor", value.actor.value},
          {"auth", value.authorization_version.value},
          {"request", value.request_id},
          {"trace", value.trace_id},
          {"key", value.idempotency_key},
          {"host", value.host_id},
          {"expected", value.expected_version},
          {"grace", value.grace_ms}};
}
bool same_intent(const t::HostMaintenanceIntent& left, const t::HostMaintenanceIntent& right) {
  return left.action == right.action && left.actor == right.actor &&
         left.idempotency_key == right.idempotency_key && left.host_id == right.host_id &&
         left.expected_version == right.expected_version && left.grace_ms == right.grace_ms;
}
Json encode(const t::HostMaintenanceOperation& value) {
  Json targets = Json::array();
  for (const auto& target : value.targets) {
    const auto& execution = target.execution;
    targets.push_back({{"host", execution.host_id},
                       {"instance", execution.instance_id},
                       {"installation", execution.installation_id},
                       {"generation", execution.execution_generation},
                       {"desired", execution.desired_version},
                       {"pid", execution.pid},
                       {"marker", execution.process_marker},
                       {"state", static_cast<int>(execution.state)},
                       {"outcome", static_cast<int>(execution.outcome)},
                       {"command", execution.command_id},
                       {"stopped", target.stopped}});
  }
  return {
      {"operation", operation_json(value.operation)}, {"intent", intent_json(value.intent)},
      {"phase", static_cast<int>(value.phase)},       {"gate_intent", value.gate_intent_version},
      {"expected_gate", value.expected_gate_version}, {"targets", targets}};
}
t::HostMaintenanceOperation decode(const std::string& text) {
  const auto value = Json::parse(text);
  t::HostMaintenanceOperation result;
  result.operation = decode_operation(value.at("operation"));
  const auto& intent = value.at("intent");
  const auto action = intent.at("action").get<int>();
  const auto phase = value.at("phase").get<int>();
  if (action < 0 || action > 1 || phase < 0 || phase > 5) {
    throw StorageError{failure(t::ErrorCode::Internal)};
  }
  result.intent = {
      static_cast<t::HostMaintenanceAction>(action), {intent.at("actor").get<std::string>()},
      {intent.at("auth").get<std::uint64_t>()},      intent.at("request").get<std::string>(),
      intent.at("trace").get<std::string>(),         intent.at("key").get<std::string>(),
      intent.at("host").get<std::string>(),          intent.at("expected").get<std::uint64_t>(),
      intent.at("grace").get<std::uint32_t>()};
  result.phase = static_cast<t::HostMaintenancePhase>(phase);
  result.gate_intent_version = value.at("gate_intent").get<std::uint64_t>();
  result.expected_gate_version = value.at("expected_gate").get<std::uint64_t>();
  for (const auto& target : value.at("targets")) {
    t::ManagedExecution execution;
    const auto state = target.at("state").get<int>();
    const auto outcome = target.at("outcome").get<int>();
    if (state < 0 || state > 4 || outcome < 0 || outcome > 3) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    execution.host_id = target.at("host").get<std::string>();
    execution.instance_id = target.at("instance").get<std::string>();
    execution.installation_id = target.at("installation").get<std::string>();
    execution.execution_generation = target.at("generation").get<std::uint64_t>();
    execution.desired_version = target.at("desired").get<std::uint64_t>();
    execution.pid = target.at("pid").get<std::int64_t>();
    execution.process_marker = target.at("marker").get<std::string>();
    execution.state = static_cast<t::ManagedExecutionState>(state);
    execution.outcome = static_cast<t::ExecutionOutcome>(outcome);
    execution.command_id = target.at("command").get<std::string>();
    result.targets.push_back({std::move(execution), target.at("stopped").get<bool>()});
  }
  if (result.targets.size() > t::kHostMaintenanceExecutionLimit ||
      result.intent.host_id != result.operation.ref.resource.id.value) {
    throw StorageError{failure(t::ErrorCode::Internal)};
  }
  return result;
}

class Store final : public t::HostMaintenanceStore {
public:
  Store(sqlite3* database, t::ResourceId host, t::HostClock& clock,
        HostMaintenanceStorageLimits limits, HostMaintenanceCommitCheckpoint checkpoint)
      : db_(database), host_(std::move(host)), clock_(clock), limits_(limits),
        checkpoint_(checkpoint) {}
  ~Store() override {
    sqlite3_close(db_);
  }

  template <typename Value, typename Work> t::Outcome<Value> transaction(bool write, Work work) {
    std::unique_lock guard(mutex_, std::try_to_lock);
    if (!guard.owns_lock()) {
      return t::Failure{t::ErrorCode::UpstreamUnavailable,
                        t::Acceptance::NotAccepted,
                        t::Recovery::Backoff,
                        {},
                        {}};
    }
    try {
      execute(db_, write ? "BEGIN IMMEDIATE" : "BEGIN");
      auto result = work();
      if (std::holds_alternative<t::Failure>(result)) {
        (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return result;
      }
      if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return failure(t::ErrorCode::UpstreamUnavailable, write);
      }
      guard.unlock();
      if (write && checkpoint_) {
        checkpoint_();
      }
      return result;
    } catch (const StorageError& error) {
      (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
      return error.failure;
    } catch (...) {
      (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
      return failure(t::ErrorCode::Internal);
    }
  }
  t::TimePoint now() {
    const auto now = clock_.now();
    if (now.time_since_epoch().count() <= 0 ||
        now > t::TimePoint::max() - t::kHostUnresolvedRetention) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    return now;
  }
  t::HostMaintenanceState state() {
    Statement query(db_, "SELECT host,version,admission,active,gate,observed FROM "
                         "host_maintenance_state WHERE singleton=1");
    if (!query.row() || query.text(0) != host_.value || query.number(1) <= 0 ||
        (query.number(2) != 0 && query.number(2) != 1) || query.number(4) < 0) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    t::HostMaintenanceState result{
        host_, {static_cast<std::uint64_t>(query.number(1))}, query.number(2) != 0,
        {},    static_cast<std::uint64_t>(query.number(4)),   time(query.number(5))};
    if (!query.text(3).empty()) {
      result.active_operation =
          t::OperationRef{t::Service::Host, {query.text(3)}, {t::ResourceKind::Host, host_}};
    }
    return result;
  }
  void state(const t::HostMaintenanceState& value) {
    Statement update(db_, "UPDATE host_maintenance_state SET "
                          "version=?,admission=?,active=?,gate=?,observed=? WHERE singleton=1");
    update.number(1, integer(value.version.value));
    update.number(2, value.admission_open ? 1 : 0);
    update.text(3, value.active_operation ? value.active_operation->id.value : "");
    update.number(4, integer(value.runtime_gate_version));
    update.number(5, value.observed_at.time_since_epoch().count());
    update.done();
  }
  std::optional<t::HostMaintenanceOperation> find(const t::OperationId& operation) {
    Statement query(db_, "SELECT payload FROM host_maintenance_operations WHERE operation_id=?");
    query.text(1, operation.value);
    if (!query.row()) {
      return {};
    }
    auto value = decode(query.text(0));
    if (value.operation.ref.resource.id != host_) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    return value;
  }
  void audit(const t::HostMaintenanceIntent& intent, const t::OperationId& operation,
             std::string_view method, t::TimePoint at) {
    Statement insert(db_, "INSERT INTO "
                          "host_maintenance_audit(actor,operation_id,method,request_id,trace_id,"
                          "authorization_version,occurred_at) VALUES(?,?,?,?,?,?,?)");
    insert.text(1, intent.actor.value);
    insert.text(2, operation.value);
    insert.text(3, method);
    insert.text(4, intent.request_id);
    insert.text(5, intent.trace_id);
    insert.number(6, integer(intent.authorization_version.value));
    insert.number(7, at.time_since_epoch().count());
    insert.done();
  }
  void append(const t::Operation& operation) {
    const auto payload = operation_json(operation).dump();
    Statement head(db_, "SELECT head FROM host_maintenance_meta WHERE singleton=1");
    if (!head.row()) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    const auto previous = head.number(0);
    if (previous == std::numeric_limits<std::int64_t>::max()) {
      throw StorageError{failure(t::ErrorCode::Internal)};
    }
    Statement insert(
        db_,
        "INSERT INTO host_operation_changes(sequence,payload,bytes,occurred_at) VALUES(?,?,?,?)");
    insert.number(1, previous + 1);
    insert.text(2, payload);
    insert.number(3, static_cast<std::int64_t>(payload.size()));
    insert.number(4, operation.updated_at.time_since_epoch().count());
    insert.done();
    Statement advance(db_, "UPDATE host_maintenance_meta SET head=? WHERE singleton=1");
    advance.number(1, previous + 1);
    advance.done();
    for (;;) {
      Statement sizes(db_, "SELECT count(*),COALESCE(sum(bytes),0) FROM host_operation_changes");
      if (!sizes.row()) {
        throw StorageError{failure(t::ErrorCode::Internal)};
      }
      if (static_cast<std::uint64_t>(sizes.number(0)) <= limits_.change_capacity &&
          static_cast<std::uint64_t>(sizes.number(1)) <= limits_.change_bytes) {
        break;
      }
      Statement oldest(db_, "SELECT min(sequence) FROM host_operation_changes");
      if (!oldest.row() || oldest.number(0) <= 0) {
        throw StorageError{failure(t::ErrorCode::Internal)};
      }
      erase_through(oldest.number(0));
    }
  }
  void erase_through(std::int64_t sequence) {
    Statement remove(db_, "DELETE FROM host_operation_changes WHERE sequence<=?");
    remove.number(1, sequence);
    remove.done();
    Statement advance(
        db_, "UPDATE host_maintenance_meta SET low_water=max(low_water,?) WHERE singleton=1");
    advance.number(1, sequence);
    advance.done();
  }
  void persist(const t::HostMaintenanceOperation& value) {
    const auto payload = encode(value).dump();
    if (payload.size() > 1024 * 1024) {
      throw StorageError{failure(t::ErrorCode::RateLimited)};
    }
    Statement update(db_, "UPDATE host_maintenance_operations SET payload=? WHERE operation_id=?");
    update.text(1, payload);
    update.text(2, value.operation.ref.id.value);
    update.done();
    append(value.operation);
  }
  t::Outcome<t::HostMaintenanceState> maintenance_state() override {
    return transaction<t::HostMaintenanceState>(
        false, [&]() -> t::Outcome<t::HostMaintenanceState> { return state(); });
  }
  t::Outcome<bool> allows_start() override {
    return transaction<bool>(false, [&]() -> t::Outcome<bool> { return state().admission_open; });
  }
  t::Outcome<std::optional<t::HostMaintenanceOperation>>
  find_maintenance(const t::OperationId& operation) override {
    return transaction<std::optional<t::HostMaintenanceOperation>>(
        false, [&]() -> t::Outcome<std::optional<t::HostMaintenanceOperation>> {
          return find(operation);
        });
  }
  t::Outcome<t::HostMaintenanceOperation>
  accept_maintenance(const t::HostMaintenanceIntent& intent) override {
    return transaction<
        t::HostMaintenanceOperation>(true, [&]() -> t::Outcome<t::HostMaintenanceOperation> {
      if (intent.host_id != host_.value || !token(intent.actor.value) ||
          !token(intent.request_id) || !token(intent.trace_id) ||
          intent.authorization_version.value == 0 || intent.idempotency_key.size() < 16 ||
          intent.idempotency_key.size() > 128 || intent.grace_ms > 10000 || intent.grace_ms == 0) {
        return failure(t::ErrorCode::InvalidArgument);
      }
      Statement replay(db_, "SELECT payload FROM host_maintenance_operations WHERE actor=? AND "
                            "action=? AND idempotency_key=?");
      replay.text(1, intent.actor.value);
      replay.number(2, static_cast<int>(intent.action));
      replay.text(3, intent.idempotency_key);
      if (replay.row()) {
        auto original = decode(replay.text(0));
        if (!same_intent(original.intent, intent)) {
          return failure(t::ErrorCode::Conflict);
        }
        return original;
      }
      auto current = state();
      if (current.version.value != intent.expected_version || current.active_operation ||
          (intent.action == t::HostMaintenanceAction::Begin) != current.admission_open ||
          find({intent.request_id})) {
        return failure(t::ErrorCode::Conflict);
      }
      const auto at = now();
      t::HostMaintenanceOperation value;
      value.intent = intent;
      value.operation.ref = {t::Service::Host, {intent.request_id}, {t::ResourceKind::Host, host_}};
      value.operation.version.value = 1;
      value.operation.created_at = value.operation.updated_at = at;
      value.operation.retain_until = at + t::kHostUnresolvedRetention;
      Statement insert(db_, "INSERT INTO "
                            "host_maintenance_operations(operation_id,actor,action,idempotency_key,"
                            "payload) VALUES(?,?,?,?,?)");
      insert.text(1, intent.request_id);
      insert.text(2, intent.actor.value);
      insert.number(3, static_cast<int>(intent.action));
      insert.text(4, intent.idempotency_key);
      insert.text(5, encode(value).dump());
      insert.done();
      current.active_operation = value.operation.ref;
      ++current.version.value;
      current.observed_at = at;
      state(current);
      append(value.operation);
      audit(intent, value.operation.ref.id, "accepted", at);
      return value;
    });
  }
  t::Outcome<t::HostMaintenanceOperation>
  update_maintenance(const t::HostMaintenanceUpdate& request) override {
    return transaction<t::HostMaintenanceOperation>(
        true, [&]() -> t::Outcome<t::HostMaintenanceOperation> {
          auto old = find(request.value.operation.ref.id);
          if (!old) {
            return failure(t::ErrorCode::NotFound);
          }
          auto current = state();
          if (old->operation.version.value != request.expected_operation_version ||
              terminal(old->operation.state) || !current.active_operation ||
              current.active_operation->id != old->operation.ref.id) {
            return failure(t::ErrorCode::Conflict);
          }
          auto next = request.value;
          next.intent = old->intent;
          next.operation.ref = old->operation.ref;
          next.operation.created_at = old->operation.created_at;
          next.operation.version.value = old->operation.version.value + 1;
          next.operation.updated_at = now();
          if (next.operation.updated_at < next.operation.created_at ||
              next.targets.size() > t::kHostMaintenanceExecutionLimit) {
            return failure(t::ErrorCode::Internal);
          }
          next.operation.retain_until =
              next.operation.updated_at + (terminal(next.operation.state)
                                               ? t::kHostResolvedRetention
                                               : t::kHostUnresolvedRetention);
          if (old->phase == t::HostMaintenancePhase::Accepted &&
              (next.phase == t::HostMaintenancePhase::Closing ||
               next.phase == t::HostMaintenancePhase::Opening)) {
            ++current.version.value;
            next.gate_intent_version = current.version.value;
            next.expected_gate_version = current.runtime_gate_version;
          } else {
            next.gate_intent_version = old->gate_intent_version;
            next.expected_gate_version = old->expected_gate_version;
          }
          if (request.admission_open && current.admission_open != *request.admission_open) {
            current.admission_open = *request.admission_open;
            if (old->phase != t::HostMaintenancePhase::Accepted) {
              ++current.version.value;
            }
          }
          if (request.runtime_gate_version) {
            current.runtime_gate_version = *request.runtime_gate_version;
          }
          if (terminal(next.operation.state)) {
            current.active_operation.reset();
            ++current.version.value;
          }
          current.observed_at = next.operation.updated_at;
          state(current);
          persist(next);
          audit(next.intent, next.operation.ref.id, "progress", next.operation.updated_at);
          return next;
        });
  }
  t::Outcome<t::HostMaintenanceOperation>
  cancel_maintenance(const t::HostMaintenanceIntent& identity, const t::OperationId& operation,
                     std::uint64_t expected) override {
    return transaction<t::HostMaintenanceOperation>(
        true, [&]() -> t::Outcome<t::HostMaintenanceOperation> {
          if (identity.host_id != host_.value || !token(identity.actor.value) ||
              !token(identity.request_id) || !token(identity.trace_id) ||
              identity.authorization_version.value == 0 || identity.idempotency_key.size() < 16 ||
              identity.idempotency_key.size() > 128 || expected == 0) {
            return failure(t::ErrorCode::InvalidArgument);
          }
          Statement replay(db_,
                           "SELECT operation_id,expected_version,payload FROM "
                           "host_maintenance_cancellations WHERE actor=? AND idempotency_key=?");
          replay.text(1, identity.actor.value);
          replay.text(2, identity.idempotency_key);
          if (replay.row()) {
            if (replay.text(0) != operation.value ||
                static_cast<std::uint64_t>(replay.number(1)) != expected) {
              return failure(t::ErrorCode::Conflict);
            }
            return decode(replay.text(2));
          }
          auto value = find(operation);
          if (!value) {
            return failure(t::ErrorCode::NotFound);
          }
          if (value->phase != t::HostMaintenancePhase::Accepted ||
              terminal(value->operation.state) || value->operation.version.value != expected) {
            return failure(t::ErrorCode::Conflict);
          }
          auto current = state();
          if (!current.active_operation || current.active_operation->id != operation) {
            return failure(t::ErrorCode::Conflict);
          }
          value->phase = t::HostMaintenancePhase::Finished;
          value->operation.state = t::OperationState::Cancelled;
          value->operation.execution_outcome = t::ExecutionOutcome::NotStarted;
          value->operation.blocked_reason.reset();
          value->operation.next_attempt.reset();
          value->operation.participants.clear();
          value->operation.updated_at = now();
          value->operation.retain_until = value->operation.updated_at + t::kHostResolvedRetention;
          ++value->operation.version.value;
          current.active_operation.reset();
          ++current.version.value;
          current.observed_at = value->operation.updated_at;
          state(current);
          persist(*value);
          audit(identity, operation, "cancelled", value->operation.updated_at);
          Statement insert(db_, "INSERT INTO "
                                "host_maintenance_cancellations(actor,idempotency_key,operation_id,"
                                "expected_version,payload) VALUES(?,?,?,?,?)");
          insert.text(1, identity.actor.value);
          insert.text(2, identity.idempotency_key);
          insert.text(3, operation.value);
          insert.number(4, integer(expected));
          insert.text(5, encode(*value).dump());
          insert.done();
          return *value;
        });
  }
  t::Outcome<t::OperationObservationPage>
  query_operation_changes(const t::OperationObservationQuery& request) override {
    if (request.scope != std::vector<t::ResourceRef>{{t::ResourceKind::Host, host_}} ||
        request.page_size > 100) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    if (std::chrono::steady_clock::now() >= request.deadline) {
      return failure(t::ErrorCode::Timeout);
    }
    return transaction<t::OperationObservationPage>(
        false, [&]() -> t::Outcome<t::OperationObservationPage> {
          if (std::chrono::steady_clock::now() >= request.deadline) {
            return failure(t::ErrorCode::Timeout);
          }
          Statement meta(
              db_, "SELECT epoch,head,low_water FROM host_maintenance_meta WHERE singleton=1");
          if (!meta.row()) {
            return failure(t::ErrorCode::Internal);
          }
          if (!token(meta.text(0)) || meta.number(1) < 0 || meta.number(2) < 0 ||
              meta.number(2) > meta.number(1)) {
            return failure(t::ErrorCode::Internal);
          }
          const auto at = now();
          const auto epoch = meta.text(0);
          const auto head = static_cast<std::uint64_t>(meta.number(1));
          auto low = static_cast<std::uint64_t>(meta.number(2));
          Statement expired(
              db_,
              "SELECT COALESCE(max(sequence),0) FROM host_operation_changes WHERE occurred_at<=?");
          expired.number(1, (at - t::kOperationObservationRetention).time_since_epoch().count());
          if (!expired.row()) {
            return failure(t::ErrorCode::Internal);
          }
          low = std::max(low, static_cast<std::uint64_t>(expired.number(0)));
          t::OperationObservationPage page;
          page.checked_at = at;
          page.next_cursor = {t::Service::Host, epoch, request.scope, head};
          if (!request.cursor) {
            return page;
          }
          const auto& cursor = *request.cursor;
          if (cursor.owner != t::Service::Host || cursor.scope != request.scope ||
              cursor.epoch.empty()) {
            return failure(t::ErrorCode::InvalidArgument);
          }
          if (cursor.epoch != epoch || cursor.sequence < low || cursor.sequence > head) {
            return failure(t::ErrorCode::Conflict);
          }
          const auto size = request.page_size == 0 ? 32U : request.page_size;
          Statement query(db_, "SELECT sequence,payload FROM host_operation_changes WHERE "
                               "sequence>? ORDER BY sequence LIMIT ?");
          query.number(1, integer(cursor.sequence));
          query.number(2, size + 1);
          while (query.row()) {
            if (page.changes.size() == size) {
              page.has_more = true;
              page.next_cursor.sequence = page.changes.back().cursor.sequence;
              break;
            }
            auto operation = decode_operation(Json::parse(query.text(1)));
            if (operation.ref.resource != request.scope.front()) {
              return failure(t::ErrorCode::Internal);
            }
            page.changes.push_back({std::move(operation),
                                    {t::Service::Host, epoch, request.scope,
                                     static_cast<std::uint64_t>(query.number(0))}});
          }
          if (std::chrono::steady_clock::now() >= request.deadline) {
            return failure(t::ErrorCode::Timeout);
          }
          return page;
        });
  }
  t::Outcome<std::size_t> prune_operation_changes(std::size_t limit) override {
    if (limit == 0 || limit > 1000) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    return transaction<std::size_t>(true, [&]() -> t::Outcome<std::size_t> {
      Statement query(db_, "SELECT sequence FROM host_operation_changes WHERE occurred_at<=? ORDER "
                           "BY sequence LIMIT ?");
      query.number(1, (now() - t::kOperationObservationRetention).time_since_epoch().count());
      query.number(2, static_cast<std::int64_t>(limit));
      std::size_t count = 0;
      std::int64_t last = 0;
      while (query.row()) {
        ++count;
        last = query.number(0);
      }
      if (last) {
        erase_through(last);
      }
      return count;
    });
  }
  t::Status rotate_observation_epoch(std::string_view epoch) override {
    if (!token(epoch)) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    return transaction<std::monostate>(true, [&]() -> t::Status {
      Statement query(db_, "SELECT epoch FROM host_maintenance_meta WHERE singleton=1");
      if (!query.row()) {
        return failure(t::ErrorCode::Internal);
      }
      if (query.text(0) == epoch) {
        return failure(t::ErrorCode::Conflict);
      }
      execute(db_, "DELETE FROM host_operation_changes");
      Statement update(
          db_, "UPDATE host_maintenance_meta SET epoch=?,head=0,low_water=0 WHERE singleton=1");
      update.text(1, epoch);
      update.done();
      return std::monostate{};
    });
  }

private:
  sqlite3* db_;
  t::ResourceId host_;
  t::HostClock& clock_;
  HostMaintenanceStorageLimits limits_;
  HostMaintenanceCommitCheckpoint checkpoint_;
  std::mutex mutex_;
};

const char* schema() {
  return R"sql(
CREATE TABLE IF NOT EXISTS host_maintenance_meta(singleton INTEGER PRIMARY KEY CHECK(singleton=1),schema_version INTEGER NOT NULL,epoch TEXT NOT NULL,head INTEGER NOT NULL,low_water INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS host_maintenance_state(singleton INTEGER PRIMARY KEY CHECK(singleton=1),host TEXT NOT NULL,version INTEGER NOT NULL,admission INTEGER NOT NULL,active TEXT NOT NULL,gate INTEGER NOT NULL,observed INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS host_maintenance_operations(operation_id TEXT PRIMARY KEY,actor TEXT NOT NULL,action INTEGER NOT NULL,idempotency_key TEXT NOT NULL,payload TEXT NOT NULL,UNIQUE(actor,action,idempotency_key));
CREATE TABLE IF NOT EXISTS host_maintenance_cancellations(actor TEXT NOT NULL,idempotency_key TEXT NOT NULL,operation_id TEXT NOT NULL,expected_version INTEGER NOT NULL,payload TEXT NOT NULL,PRIMARY KEY(actor,idempotency_key));
CREATE TABLE IF NOT EXISTS host_maintenance_audit(sequence INTEGER PRIMARY KEY AUTOINCREMENT,actor TEXT NOT NULL,operation_id TEXT NOT NULL,method TEXT NOT NULL,request_id TEXT NOT NULL,trace_id TEXT NOT NULL,authorization_version INTEGER NOT NULL,occurred_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS host_operation_changes(sequence INTEGER PRIMARY KEY,payload TEXT NOT NULL,bytes INTEGER NOT NULL,occurred_at INTEGER NOT NULL);
)sql";
}
} // namespace

t::Outcome<std::unique_ptr<t::HostMaintenanceStore>>
make_sqlite_host_maintenance(std::string path, t::ResourceId host, std::string epoch,
                             t::HostClock& clock, HostMaintenanceStorageLimits limits,
                             HostMaintenanceCommitCheckpoint checkpoint) {
  if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos ||
      !token(host.value) || !token(epoch) || limits.change_capacity == 0 ||
      limits.change_capacity > t::kOperationObservationCapacity || limits.change_bytes < 1024 ||
      limits.change_bytes > t::kOperationObservationBytes) {
    return failure(t::ErrorCode::InvalidArgument);
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  try {
    sqlite3_busy_timeout(db, 200);
    execute(db, "PRAGMA journal_mode=WAL");
    execute(db, "PRAGMA synchronous=FULL");
    execute(db, "BEGIN IMMEDIATE");
    execute(db, schema());
    Statement state(db, "INSERT OR IGNORE INTO "
                        "host_maintenance_state(singleton,host,version,admission,active,gate,"
                        "observed) VALUES(1,?,1,1,'',0,?)");
    state.text(1, host.value);
    state.number(2, clock.now().time_since_epoch().count());
    state.done();
    Statement metadata(
        db,
        "INSERT OR IGNORE INTO "
        "host_maintenance_meta(singleton,schema_version,epoch,head,low_water) VALUES(1,1,?,0,0)");
    metadata.text(1, epoch);
    metadata.done();
    Statement verify(db, "SELECT host,schema_version FROM host_maintenance_state JOIN "
                         "host_maintenance_meta USING(singleton)");
    if (!verify.row() || verify.text(0) != host.value || verify.number(1) != 1) {
      throw StorageError{failure(t::ErrorCode::Conflict)};
    }
    execute(db, "COMMIT");
    return std::unique_ptr<t::HostMaintenanceStore>{
        std::make_unique<Store>(db, std::move(host), clock, limits, checkpoint)};
  } catch (const StorageError& error) {
    (void)sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return error.failure;
  } catch (...) {
    (void)sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return failure(t::ErrorCode::Internal);
  }
}
} // namespace kiarabot::infrastructure::host
