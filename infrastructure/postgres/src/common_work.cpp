#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

#include <sys/socket.h>

#include "kiarabot/core/domain/target/persistence_rules.hpp"
#include "record_codec.hpp"
#include "session.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

target::Failure error(target::ErrorCode code) {
  auto recovery = target::Recovery::Diagnose;
  if (code == target::ErrorCode::Conflict) {
    recovery = target::Recovery::ReadCurrent;
  } else if (code == target::ErrorCode::InvalidArgument) {
    recovery = target::Recovery::CorrectInput;
  } else if (code == target::ErrorCode::RateLimited || code == target::ErrorCode::Timeout) {
    recovery = target::Recovery::Backoff;
  }
  return target::rejected(code, recovery);
}
target::Failure storage_error() {
  try {
    throw;
  } catch (const pqxx::broken_connection&) {
    return error(target::ErrorCode::UpstreamUnavailable);
  } catch (const pqxx::unique_violation&) {
    return error(target::ErrorCode::Conflict);
  } catch (const pqxx::foreign_key_violation&) {
    return error(target::ErrorCode::Conflict);
  } catch (const pqxx::check_violation&) {
    return error(target::ErrorCode::InvalidArgument);
  } catch (const pqxx::sql_error& exception) {
    if (exception.sqlstate() == "57014" || exception.sqlstate() == "55P03") {
      return error(target::ErrorCode::Timeout);
    }
    if (exception.sqlstate().starts_with("53") || exception.sqlstate().starts_with("58") ||
        exception.sqlstate().starts_with("57")) {
      return error(target::ErrorCode::UpstreamUnavailable);
    }
    if (exception.sqlstate() == "40001" || exception.sqlstate() == "40P01") {
      return error(target::ErrorCode::Conflict);
    }
    if (exception.sqlstate() == "22003" || exception.sqlstate() == "22021") {
      return error(target::ErrorCode::InvalidArgument);
    }
    return error(target::ErrorCode::Internal);
  } catch (...) {
    return error(target::ErrorCode::Internal);
  }
}
bool valid_receipt_text(const target::ReceiptKey& key) {
  return valid_sql_text_fields(
      {key.subject.value, key.method, key.resource.id.value, key.idempotency_key});
}
pqxx::params receipt_key(const target::ReceiptKey& key) {
  return {key.subject.value, key.method, static_cast<int>(key.resource.kind), key.resource.id.value,
          key.idempotency_key};
}
constexpr std::string_view kReceiptWhere =
    "subject=$1 AND method=$2 AND resource_kind=$3 AND resource_id=$4 AND idempotency_key=$5";
std::string handoff_marks() {
  return "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15";
}
class BudgetExpired {};

// The watchdog owns a duplicate descriptor, so a pqxx disconnect cannot recycle the watched
// fd for an unrelated connection. It only shutdowns this socket; pqxx stays on its owner thread.
class SocketLease final {
public:
  explicit SocketLease(int socket) : descriptor_(::fcntl(socket, F_DUPFD_CLOEXEC, 0)) {
    if (descriptor_ < 0) {
      throw std::runtime_error("cannot retain session socket");
    }
  }
  ~SocketLease() {
    close();
  }
  void close() noexcept {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
  }
  SocketLease(const SocketLease&) = delete;
  SocketLease& operator=(const SocketLease&) = delete;
  int get() const noexcept {
    return descriptor_;
  }

private:
  int descriptor_;
};
class Deadline final {
public:
  Deadline(int socket, std::chrono::steady_clock::time_point deadline)
      : socket_(socket), thread_([this, deadline] {
          std::unique_lock lock(mutex_);
          if (!condition_.wait_until(lock, deadline, [this] { return stopped_; })) {
            expired_.store(true);
            ::shutdown(socket_.get(), SHUT_RDWR);
          }
        }) {}
  ~Deadline() {
    stop();
  }
  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopped_ = true;
    }
    condition_.notify_one();
    if (thread_.joinable()) {
      thread_.join();
    }
    socket_.close();
  }
  bool expired() const noexcept {
    return expired_.load();
  }

private:
  SocketLease socket_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopped_ = false;
  std::atomic<bool> expired_{false};
  std::thread thread_;
};

class CommonWork final : public target::UnitOfWork,
                         public SqlWork,
                         public target::ReceiptRepository,
                         public target::JournalRepository,
                         public target::Repository<target::OperationId, target::Operation> {
public:
  explicit CommonWork(std::shared_ptr<Session> session)
      : session_(std::move(session)), owner_(session_->owner),
        deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(2)),
        budget_(session_->connection->sock(), deadline_),
        transaction_(std::make_unique<pqxx::work>(*session_->connection)) {
    transaction_->exec("SET LOCAL statement_timeout='2000ms'; SET LOCAL lock_timeout='250ms'; "
                       "SET LOCAL idle_in_transaction_session_timeout='2000ms'");
    const auto locks = transaction_->exec(
        "SELECT count(*) FROM pg_locks WHERE locktype='advisory' AND pid=pg_backend_pid() "
        "AND database=(SELECT oid FROM pg_database WHERE datname=current_database()) "
        "AND classid=$1::oid AND objid=$2::oid AND objsubid=2 AND granted",
        pqxx::params{kWriterLockClass, kWriterLockId});
    if (locks.one_row()[0].as<int>() != 1) {
      session_->disabled = true;
      throw std::runtime_error("writer lock lost");
    }
  }
  ~CommonWork() override {
    rollback();
  }
  target::Service owner() const noexcept override {
    return owner_;
  }
  target::ReceiptRepository& receipts() override {
    return *this;
  }
  target::JournalRepository& journal() override {
    return *this;
  }
  target::Repository<target::OperationId, target::Operation>& operations() override {
    return *this;
  }
  target::Outcome<target::TimePoint> current_time() override {
    return invoke<target::TimePoint>([&]() -> target::Outcome<target::TimePoint> {
      return time_field(
          exec("SELECT (extract(epoch FROM clock_timestamp())*1000000000)::bigint AS now")
              .one_row(),
          "now");
    });
  }
  target::CommitResult commit() override {
    if (!transaction_) {
      return terminal_;
    }
    if (failed_ || std::chrono::steady_clock::now() >= deadline_) {
      rollback();
      return terminal_;
    }
    try {
      transaction_->commit();
      terminal_ = target::CommitResult::Committed;
    } catch (const pqxx::in_doubt_error&) {
      terminal_ = target::CommitResult::Unknown;
      session_->disabled = true;
    } catch (const pqxx::broken_connection&) {
      terminal_ = target::CommitResult::Unknown;
      session_->disabled = true;
    } catch (...) {
      terminal_ = target::CommitResult::RolledBack;
    }
    transaction_.reset();
    budget_.stop();
    if (budget_.expired()) {
      session_->disabled = true;
    }
    if (session_->disabled) {
      session_->connection.reset();
    }
    session_->permit.release();
    session_.reset();
    return terminal_;
  }
  void rollback() noexcept override {
    if (!transaction_) {
      return;
    }
    transaction_.reset();
    budget_.stop();
    if (budget_.expired()) {
      session_->disabled = true;
    }
    if (session_->disabled) {
      session_->connection.reset();
    }
    session_->permit.release();
    session_.reset();
    terminal_ = target::CommitResult::RolledBack;
  }
  target::Outcome<std::optional<target::CommandReceipt>>
  find(const target::ReceiptKey& key) override {
    return invoke<std::optional<target::CommandReceipt>>(
        [&]() -> target::Outcome<std::optional<target::CommandReceipt>> {
          if (!valid_receipt_text(key)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          return find_receipt(key);
        });
  }
  target::Outcome<target::ReceiptWrite> insert(const target::CommandReceipt& receipt,
                                               target::ReceiptCapacity capacity) override {
    return invoke<target::ReceiptWrite>([&]() -> target::Outcome<target::ReceiptWrite> {
      if (!valid_receipt_text(receipt.key)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      lock_capacity(1);
      const auto existing = find_receipt(receipt.key);
      if (!existing && !valid_text(receipt.result)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto used =
          exec("SELECT count(*) FROM kiarabot.command_receipts").one_row()[0].as<std::size_t>();
      const auto decision = target::check_receipt_write(existing ? &*existing : nullptr, receipt,
                                                        used, capacity, db_now());
      if (const auto* failure = std::get_if<target::Failure>(&decision)) {
        return *failure;
      }
      if (existing) {
        return target::ReceiptWrite{*existing, true};
      }
      const auto expired =
          exec("DELETE FROM kiarabot.command_receipts WHERE " + std::string(kReceiptWhere) +
                   " AND retain_until<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint "
                   "RETURNING result_id",
               receipt_key(receipt.key));
      for (const auto& row : expired) {
        exec("DELETE FROM kiarabot.replay_results WHERE id=$1", {row[0].as<std::int64_t>()});
      }
      const auto result_id = store_result(*transaction_, receipt.result);
      auto parameters = receipt_key(receipt.key);
      parameters.append(
          pqxx::params{digest(receipt.fingerprint), result_id, timestamp(receipt.retain_until)});
      exec("INSERT INTO kiarabot.command_receipts VALUES($1,$2,$3,$4,$5,$6,$7,(extract(epoch FROM "
           "clock_timestamp())*1000000000)::bigint,"
           "GREATEST($8::bigint,(extract(epoch FROM "
           "clock_timestamp())*1000000000)::bigint+604800000000000))",
           parameters);
      return target::ReceiptWrite{*find_receipt(receipt.key), false};
    });
  }
  target::Outcome<std::size_t> prune_expired(target::TimePoint now, std::size_t limit) override {
    (void)now;
    return prune("command_receipts", "retain_until", limit);
  }
  target::Status append_audit(const target::AuditEntry& entry) override {
    return invoke<std::monostate>([&]() -> target::Status {
      if (!valid_sql_text_fields({entry.actor.value, entry.resource.id.value, entry.request_id,
                                  entry.trace_id, entry.method})) {
        return error(target::ErrorCode::InvalidArgument);
      }
      exec("INSERT INTO "
           "kiarabot.audit_entries(actor,resource_kind,resource_id,request_id,trace_id,method,"
           "rejection,authorization_version,occurred_at) "
           "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9)",
           {entry.actor.value, static_cast<int>(entry.resource.kind), entry.resource.id.value,
            entry.request_id, entry.trace_id, entry.method,
            entry.rejection ? std::optional<int>(static_cast<int>(*entry.rejection)) : std::nullopt,
            entry.authorization_version.value, timestamp(entry.occurred_at)});
      return std::monostate{};
    });
  }
  target::Status enqueue(const target::OutboxEntry& entry, target::JournalCapacity capacity,
                         target::TimePoint now) override {
    (void)now;
    return invoke<std::monostate>([&]() -> target::Status {
      if (entry.context.source != owner()) {
        return error(target::ErrorCode::PermissionDenied);
      }
      if (!valid_text(entry.context) ||
          !valid_sql_text(entry.payload_reference.resource.id.value)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      lock_capacity(entry.lane == target::OutboxLane::Ordinary ? 2 : 3);
      const auto records =
          exec("SELECT " + handoff_projection() +
                   ",payload_kind,payload_id,payload_version,lane FROM kiarabot.outbox WHERE "
                   "source=$1 AND message=$2",
               {static_cast<int>(entry.context.source), entry.context.message.value});
      std::optional<target::OutboxEntry> existing;
      if (!records.empty()) {
        const auto row = records.one_row();
        target::OutboxEntry stored;
        stored.context = read_handoff(row);
        stored.payload_reference = {
            {static_cast<target::ResourceKind>(row["payload_kind"].as<int>()),
             {row["payload_id"].as<std::string>()}},
            {row["payload_version"].as<std::uint64_t>()}};
        stored.lane = static_cast<target::OutboxLane>(row["lane"].as<int>());
        existing = stored;
      }
      const auto used =
          exec("SELECT count(*) FROM kiarabot.outbox WHERE lane=$1 AND state NOT IN (2,3)",
               {static_cast<int>(entry.lane)})
              .one_row()[0]
              .as<std::size_t>();
      const auto decision = target::check_outbox_enqueue(existing ? &*existing : nullptr, entry,
                                                         used, capacity, db_now());
      if (const auto* failure = std::get_if<target::Failure>(&decision)) {
        return *failure;
      }
      if (existing) {
        return std::monostate{};
      }
      auto parameters = handoff_params(entry.context);
      parameters.append(pqxx::params{
          static_cast<int>(entry.payload_reference.resource.kind),
          entry.payload_reference.resource.id.value, entry.payload_reference.version.value,
          static_cast<int>(entry.lane), timestamp(entry.next_attempt_at)});
      const auto inserted =
          exec("INSERT INTO kiarabot.outbox(" + std::string(kHandoffFields) +
                   ",payload_kind,payload_id,payload_version,lane,state,attempts,next_attempt_at) "
                   "SELECT " +
                   handoff_marks() +
                   ",$16,$17,$18,$19,0,0,$20 WHERE $15::bigint>(extract(epoch FROM "
                   "clock_timestamp())*1000000000)::bigint AND "
                   "$14::bigint<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
               parameters);
      if (inserted.affected_rows() != 1) {
        return error(target::ErrorCode::Conflict);
      }
      return std::monostate{};
    });
  }
  target::Outcome<std::vector<target::OutboxClaim>> claim(target::Service source, std::size_t limit,
                                                          target::TimePoint now,
                                                          std::chrono::seconds lease) override {
    (void)now;
    return invoke<std::vector<target::OutboxClaim>>(
        [&]() -> target::Outcome<std::vector<target::OutboxClaim>> {
          const auto validation = target::validate_claim_request(source, owner(), limit, lease);
          if (const auto* failure = std::get_if<target::Failure>(&validation)) {
            return *failure;
          }
          exec("UPDATE kiarabot.outbox SET state=3 WHERE source=$1 AND state IN (0,1) AND "
               "valid_until<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
               {static_cast<int>(source)});
          exec("UPDATE kiarabot.outbox SET state=4 WHERE source=$1 AND state IN (0,1) AND "
               "attempts>=100 AND (lease_until IS NULL OR lease_until<=(extract(epoch FROM "
               "clock_timestamp())*1000000000)::bigint)",
               {static_cast<int>(source)});
          const auto rows = exec(
              "SELECT " + handoff_projection() +
                  ",payload_kind,payload_id,payload_version,lane,attempts," +
                  time_projection("next_attempt_at") +
                  " FROM kiarabot.outbox "
                  "WHERE source=$1 AND state IN (0,1) AND attempts<100 AND "
                  "valid_until>(extract(epoch FROM clock_timestamp())*1000000000)::bigint AND "
                  "next_attempt_at<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint "
                  "AND (lease_until IS NULL OR lease_until<=(extract(epoch FROM "
                  "clock_timestamp())*1000000000)::bigint) ORDER BY "
                  "next_attempt_at,message LIMIT $2 FOR UPDATE SKIP LOCKED",
              {static_cast<int>(source), limit});
          std::vector<target::OutboxClaim> claims;
          for (const auto& row : rows) {
            const auto updated =
                exec("UPDATE kiarabot.outbox SET "
                     "state=1,attempts=attempts+1,claim_token=gen_random_uuid()::text,lease_until="
                     "LEAST(valid_until,(extract(epoch FROM "
                     "clock_timestamp())*1000000000)::bigint+$3::bigint*1000000000) "
                     "WHERE source=$1 AND message=$2 AND valid_until>(extract(epoch FROM "
                     "clock_timestamp())*1000000000)::bigint RETURNING "
                     "claim_token," +
                         time_projection("lease_until"),
                     {static_cast<int>(source), row["message"].as<std::string>(), lease.count()});
            if (updated.empty()) {
              continue;
            }
            target::OutboxEntry entry;
            entry.context = read_handoff(row);
            entry.payload_reference = {
                {static_cast<target::ResourceKind>(row["payload_kind"].as<int>()),
                 {row["payload_id"].as<std::string>()}},
                {row["payload_version"].as<std::uint64_t>()}};
            entry.lane = static_cast<target::OutboxLane>(row["lane"].as<int>());
            entry.state = target::OutboxState::Claimed;
            entry.attempts = row["attempts"].as<std::uint32_t>() + 1;
            entry.claim_token =
                target::ClaimToken{updated.one_row()["claim_token"].as<std::string>()};
            entry.lease_until = time_field(updated.one_row(), "lease_until");
            entry.next_attempt_at = time_field(row, "next_attempt_at");
            claims.push_back({entry, *entry.claim_token, *entry.lease_until});
          }
          return claims;
        });
  }
  target::Status confirm(const target::HandoffKey& key, const target::ClaimToken& token,
                         target::TimePoint now) override {
    (void)now;
    return invoke<std::monostate>([&]() -> target::Status {
      if (!valid_sql_text_fields({key.message.value, token.value})) {
        return error(target::ErrorCode::InvalidArgument);
      }
      if (key.source != owner()) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto updated = exec(
          "UPDATE kiarabot.outbox SET state=2 WHERE source=$1 AND message=$2 AND claim_token=$3 "
          "AND ((state=1 AND lease_until>(extract(epoch FROM "
          "clock_timestamp())*1000000000)::bigint AND created_at<=(extract(epoch FROM "
          "clock_timestamp())*1000000000)::bigint) OR "
          "state=2) RETURNING message",
          {static_cast<int>(key.source), key.message.value, token.value});
      return updated.empty() ? target::Status(error(target::ErrorCode::Conflict))
                             : target::Status(std::monostate{});
    });
  }
  target::Outcome<std::optional<target::InboxRecord>>
  received(const target::HandoffKey& key) override {
    return invoke<std::optional<target::InboxRecord>>(
        [&]() -> target::Outcome<std::optional<target::InboxRecord>> {
          if (!target::valid_service(key.source) || !valid_sql_text(key.message.value)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          return find_inbox(key);
        });
  }
  target::Outcome<target::InboxWrite> record_received(const target::InboxRecord& receipt,
                                                      target::JournalCapacity capacity,
                                                      target::TimePoint now) override {
    (void)now;
    return invoke<target::InboxWrite>([&]() -> target::Outcome<target::InboxWrite> {
      if (!valid_text(receipt.context)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      lock_capacity(4);
      const target::HandoffKey key{receipt.context.source, receipt.context.message};
      const auto existing = find_inbox(key);
      if (!existing && !valid_text(receipt.result)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto used = exec("SELECT count(*) FROM kiarabot.inbox").one_row()[0].as<std::size_t>();
      const auto decision = target::check_inbox_write(existing ? &*existing : nullptr, receipt,
                                                      used, capacity, db_now());
      if (const auto* failure = std::get_if<target::Failure>(&decision)) {
        return *failure;
      }
      if (existing) {
        return target::InboxWrite{*existing, true};
      }
      const auto result_id = store_result(*transaction_, receipt.result);
      auto parameters = handoff_params(receipt.context);
      parameters.append(result_id);
      const auto inserted = exec(
          "INSERT INTO kiarabot.inbox(" + std::string(kHandoffFields) +
              ",received_at,retain_until,result_id) SELECT " + handoff_marks() +
              ",(extract(epoch FROM clock_timestamp())*1000000000)::bigint,GREATEST((extract(epoch "
              "FROM clock_timestamp())*1000000000)::bigint,$15::bigint)+604800000000000,$16 "
              "WHERE $15::bigint>(extract(epoch FROM clock_timestamp())*1000000000)::bigint AND "
              "$14::bigint<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
          parameters);
      if (inserted.affected_rows() != 1) {
        return error(target::ErrorCode::Conflict);
      }
      return target::InboxWrite{*find_inbox(key), false};
    });
  }
  target::Outcome<std::size_t> prune_received(target::TimePoint now, std::size_t limit) override {
    (void)now;
    return prune("inbox", "retain_until", limit);
  }
  target::Outcome<std::optional<target::Operation>> find(const target::OperationId& id) override {
    return invoke<std::optional<target::Operation>>(
        [&]() -> target::Outcome<std::optional<target::Operation>> {
          if (!valid_sql_text(id.value)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const auto rows = exec(
              "SELECT "
              "id,owner,resource_kind,resource_id,version,state,execution_outcome,blocked_reason," +
                  time_projection("next_attempt") + " FROM kiarabot.operations WHERE id=$1",
              {id.value});
          if (rows.empty()) {
            return std::optional<target::Operation>{};
          }
          return std::optional<target::Operation>(read_operation(rows.one_row()));
        });
  }
  target::Status save(const target::Operation& value,
                      std::optional<target::ResourceVersion> expected) override {
    return invoke<std::monostate>([&]() -> target::Status {
      if (!valid_text(value) || value.ref.owner != owner() || value.ref.id.value.empty() ||
          value.ref.resource.id.value.empty() ||
          (expected && (expected->value == std::numeric_limits<std::uint64_t>::max() ||
                        value.version.value != expected->value + 1))) {
        return error(target::ErrorCode::InvalidArgument);
      }
      pqxx::params parameters{
          value.ref.id.value,
          static_cast<int>(value.ref.owner),
          static_cast<int>(value.ref.resource.kind),
          value.ref.resource.id.value,
          value.version.value,
          static_cast<int>(value.state),
          static_cast<int>(value.execution_outcome),
          value.blocked_reason ? std::optional<int>(static_cast<int>(*value.blocked_reason))
                               : std::nullopt,
          value.next_attempt ? std::optional<std::string>(timestamp(*value.next_attempt))
                             : std::nullopt};
      if (!expected) {
        const auto inserted =
            exec("INSERT INTO kiarabot.operations VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9) ON CONFLICT "
                 "DO NOTHING RETURNING id",
                 parameters);
        if (inserted.empty()) {
          return error(target::ErrorCode::Conflict);
        }
      } else {
        parameters.append(expected->value);
        const auto changed =
            exec("UPDATE kiarabot.operations SET "
                 "version=$5,state=$6,execution_outcome=$7,blocked_reason=$8,next_attempt=$9 "
                 "WHERE id=$1 AND owner=$2 AND resource_kind=$3 AND resource_id=$4 AND version=$10 "
                 "RETURNING id",
                 parameters);
        if (changed.empty()) {
          return error(target::ErrorCode::Conflict);
        }
      }
      return std::monostate{};
    });
  }

  target::Outcome<std::optional<target::ReplayResult>>
  lookup_live(RecordKind kind, std::span<const std::string> key) {
    return invoke<std::optional<target::ReplayResult>>(
        [&]() -> target::Outcome<std::optional<target::ReplayResult>> {
          const auto spec = record_spec(kind);
          if (spec.owner != owner()) {
            return error(target::ErrorCode::PermissionDenied);
          }
          if (key.size() != spec.keys.size() ||
              std::any_of(key.begin(), key.end(),
                          [](const auto& part) { return part.empty() || !valid_sql_text(part); })) {
            return error(target::ErrorCode::InvalidArgument);
          }
          return load_live(*transaction_, kind, key);
        });
  }
  target::Status write_live(RecordKind kind, const target::ReplayResult& value,
                            std::optional<target::ResourceVersion> expected) {
    return invoke<std::monostate>([&]() -> target::Status {
      const auto spec = record_spec(kind);
      if (spec.owner != owner()) {
        return error(target::ErrorCode::PermissionDenied);
      }
      if (static_cast<int>(kind) != static_cast<int>(value.index())) {
        return error(target::ErrorCode::InvalidArgument);
      }
      if (!valid_text(value)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto key = record_key(value);
      const auto version = record_version(value);
      if (key.size() != spec.keys.size() ||
          std::any_of(key.begin(), key.end(),
                      [](const auto& part) { return part.empty() || !valid_sql_text(part); }) ||
          (expected && (expected->value == std::numeric_limits<std::uint64_t>::max() ||
                        version.value != expected->value + 1))) {
        return error(target::ErrorCode::InvalidArgument);
      }
      if (!store_live(*transaction_, kind, value, expected)) {
        return error(target::ErrorCode::Conflict);
      }
      return std::monostate{};
    });
  }
  target::Outcome<std::optional<target::AuthorizationState>>
  lookup_authorization(const target::SubjectId& subject) {
    return invoke<std::optional<target::AuthorizationState>>([&]()
                                                                 -> target::Outcome<std::optional<
                                                                     target::AuthorizationState>> {
      if (owner() != target::Service::Identity) {
        return error(target::ErrorCode::PermissionDenied);
      }
      if (subject.value.empty() || !valid_sql_text(subject.value)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto rows = exec(
          "SELECT subject,version,invalidation_dirty FROM kiarabot.authorizations WHERE subject=$1",
          {subject.value});
      if (rows.empty()) {
        return std::optional<target::AuthorizationState>{};
      }
      const auto row = rows.one_row();
      return std::optional<target::AuthorizationState>({{row["subject"].as<std::string>()},
                                                        {row["version"].as<std::uint64_t>()},
                                                        row["invalidation_dirty"].as<bool>()});
    });
  }
  target::Status write_authorization(const target::AuthorizationState& state,
                                     std::optional<target::ResourceVersion> expected) {
    return invoke<std::monostate>([&]() -> target::Status {
      if (owner() != target::Service::Identity) {
        return error(target::ErrorCode::PermissionDenied);
      }
      if (state.subject.value.empty() || !valid_sql_text(state.subject.value) ||
          (expected && (expected->value == std::numeric_limits<std::uint64_t>::max() ||
                        state.version.value != expected->value + 1))) {
        return error(target::ErrorCode::InvalidArgument);
      }
      pqxx::params parameters{state.subject.value, state.version.value, state.invalidation_dirty};
      pqxx::result rows;
      if (!expected) {
        rows = exec("INSERT INTO kiarabot.authorizations VALUES($1,$2,$3) ON CONFLICT DO NOTHING "
                    "RETURNING subject",
                    parameters);
      } else {
        parameters.append(expected->value);
        rows = exec("UPDATE kiarabot.authorizations SET version=$2,invalidation_dirty=$3 WHERE "
                    "subject=$1 AND version=$4 RETURNING subject",
                    parameters);
      }
      if (rows.empty()) {
        return error(target::ErrorCode::Conflict);
      }
      return std::monostate{};
    });
  }

  target::Status
  run(const std::function<target::Status(pqxx::transaction_base&)>& function) override {
    return invoke<std::monostate>([&]() -> target::Status {
      // Refresh the remaining SQL timeout before lending the same transaction.
      exec("SELECT 1");
      return function(*transaction_);
    });
  }

private:
  template <typename Value, typename Function> target::Outcome<Value> invoke(Function function) {
    if (!transaction_ || failed_ || session_->disabled) {
      return error(target::ErrorCode::UpstreamUnavailable);
    }
    try {
      auto result = function();
      if (budget_.expired()) {
        session_->disabled = true;
        failed_ = true;
        return error(target::ErrorCode::Timeout);
      }
      if (std::holds_alternative<target::Failure>(result)) {
        failed_ = true;
      }
      return result;
    } catch (const BudgetExpired&) {
      failed_ = true;
      return error(target::ErrorCode::Timeout);
    } catch (...) {
      failed_ = true;
      if (budget_.expired()) {
        session_->disabled = true;
        return error(target::ErrorCode::Timeout);
      }
      const auto failure = storage_error();
      if (failure.code == target::ErrorCode::UpstreamUnavailable) {
        session_->disabled = true;
      }
      return failure;
    }
  }
  pqxx::result exec(const std::string& sql, const pqxx::params& parameters = {}) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline_ - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) {
      throw BudgetExpired{};
    }
    transaction_->exec("SELECT set_config('statement_timeout',$1,true)",
                       pqxx::params{std::to_string(remaining)});
    return transaction_->exec(sql, parameters);
  }
  target::TimePoint db_now() {
    return time_field(
        exec("SELECT (extract(epoch FROM clock_timestamp())*1000000000)::bigint AS now").one_row(),
        "now");
  }
  void lock_capacity(int category) {
    exec("SELECT pg_advisory_xact_lock($1,$2)", {kWriterLockClass, 100 + category});
  }
  std::optional<target::CommandReceipt> find_receipt(const target::ReceiptKey& key) {
    const auto rows =
        exec("SELECT fingerprint,result_id," + time_projection("created_at") + "," +
                 time_projection("retain_until") + " FROM kiarabot.command_receipts WHERE " +
                 std::string(kReceiptWhere) +
                 " AND retain_until>(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
             receipt_key(key));
    if (rows.empty()) {
      return {};
    }
    const auto row = rows.one_row();
    return target::CommandReceipt{key, read_digest(row["fingerprint"]),
                                  load_result(*transaction_, row["result_id"].as<std::int64_t>()),
                                  time_field(row, "created_at"), time_field(row, "retain_until")};
  }
  std::optional<target::InboxRecord> find_inbox(const target::HandoffKey& key) {
    const auto rows =
        exec("SELECT " + handoff_projection() + "," + time_projection("received_at") + "," +
                 time_projection("retain_until") +
                 ",result_id FROM kiarabot.inbox WHERE source=$1 AND message=$2 AND "
                 "retain_until>(extract(epoch FROM clock_timestamp())*1000000000)::bigint",
             {static_cast<int>(key.source), key.message.value});
    if (rows.empty()) {
      return {};
    }
    const auto row = rows.one_row();
    return target::InboxRecord{read_handoff(row), time_field(row, "received_at"),
                               time_field(row, "retain_until"),
                               load_result(*transaction_, row["result_id"].as<std::int64_t>())};
  }
  target::Outcome<std::size_t> prune(std::string_view table, std::string_view expiry,
                                     std::size_t limit) {
    return invoke<std::size_t>([&]() -> target::Outcome<std::size_t> {
      if (limit < 1 || limit > 1000) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto rows =
          exec("DELETE FROM kiarabot." + std::string(table) +
                   " WHERE ctid IN (SELECT ctid FROM kiarabot." + std::string(table) + " WHERE " +
                   std::string(expiry) +
                   "<=(extract(epoch FROM clock_timestamp())*1000000000)::bigint ORDER BY " +
                   std::string(expiry) + " LIMIT $1 FOR UPDATE SKIP LOCKED) RETURNING result_id",
               {limit});
      for (const auto& row : rows) {
        exec("DELETE FROM kiarabot.replay_results WHERE id=$1", {row[0].as<std::int64_t>()});
      }
      return static_cast<std::size_t>(rows.size());
    });
  }
  std::shared_ptr<Session> session_;
  target::Service owner_;
  std::chrono::steady_clock::time_point deadline_;
  Deadline budget_;
  std::unique_ptr<pqxx::work> transaction_;
  bool failed_ = false;
  target::CommitResult terminal_ = target::CommitResult::RolledBack;
};
} // namespace

target::Outcome<std::optional<target::ReplayResult>>
find_record(target::UnitOfWork& work, RecordKind kind, std::span<const std::string> key) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->lookup_live(kind, key);
  }
  return error(target::ErrorCode::Internal);
}
target::Status save_record(target::UnitOfWork& work, RecordKind kind,
                           const target::ReplayResult& value,
                           std::optional<target::ResourceVersion> expected) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->write_live(kind, value, expected);
  }
  return error(target::ErrorCode::Internal);
}
target::Outcome<std::optional<target::AuthorizationState>>
find_authorization(target::UnitOfWork& work, const target::SubjectId& subject) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->lookup_authorization(subject);
  }
  return error(target::ErrorCode::Internal);
}
target::Status save_authorization(target::UnitOfWork& work, const target::AuthorizationState& state,
                                  std::optional<target::ResourceVersion> expected) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->write_authorization(state, expected);
  }
  return error(target::ErrorCode::Internal);
}

target::Outcome<CommonWorkAccess> begin_common_access(const std::shared_ptr<Session>& session) {
  if (!session->permit.try_acquire_for(std::chrono::milliseconds(250))) {
    return error(target::ErrorCode::Conflict);
  }
  if (session->disabled) {
    session->permit.release();
    return error(target::ErrorCode::UpstreamUnavailable);
  }
  try {
    auto work = std::make_unique<CommonWork>(session);
    auto* sql = static_cast<SqlWork*>(work.get());
    return CommonWorkAccess{std::move(work), sql};
  } catch (...) {
    const auto failure = storage_error();
    if (failure.code == target::ErrorCode::UpstreamUnavailable) {
      session->disabled = true;
    }
    session->permit.release();
    return failure;
  }
}
target::Outcome<std::unique_ptr<target::UnitOfWork>>
begin_common(const std::shared_ptr<Session>& session) {
  auto begun = begin_common_access(session);
  if (const auto* failure = std::get_if<target::Failure>(&begun)) {
    return *failure;
  }
  return std::move(std::get<CommonWorkAccess>(begun).work);
}
} // namespace kiarabot::infrastructure::postgres::detail
