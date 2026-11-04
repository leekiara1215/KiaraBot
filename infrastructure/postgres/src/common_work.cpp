#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

#include <sys/socket.h>

#include "kiarabot/core/domain/target/persistence_rules.hpp"
#include "observation_feeds.hpp"
#include "operation_observations.hpp"
#include "record_codec.hpp"
#include "session.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

// v1.2 owner-local feed family binding: an owner journal only ever produces the families it owns.
// Kept here (storage layer) so the UoW adapter cannot accept a foreign family by construction.
bool feed_family_owns(target::Service owner, target::ObservationFamily family) {
  switch (family) {
  case target::ObservationFamily::Logs:
    return owner == target::Service::Identity || owner == target::Service::Runtime;
  case target::ObservationFamily::Messages:
    return owner == target::Service::Runtime;
  case target::ObservationFamily::Tasks:
    return owner == target::Service::Scheduler;
  }
  return false;
}
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
template <typename Enum> Enum enum_value(const pqxx::field& field, int maximum) {
  const auto value = field.as<int>();
  if (value < 0 || value > maximum) {
    throw std::runtime_error("invalid stored enum");
  }
  return static_cast<Enum>(value);
}
target::LifecycleTombstone read_tombstone(const pqxx::row& row) {
  target::LifecycleTombstone value;
  value.resource = {enum_value<target::ResourceKind>(row["resource_kind"], 15),
                    {row["resource_id"].as<std::string>()}};
  value.operation_id = row["operation_id"].as<std::string>();
  value.state = enum_value<target::LifecycleState>(row["state"], 5);
  value.deleted_at = time_field(row, "deleted_at");
  value.restore_until = time_field(row, "restore_until");
  value.purge_after = time_field(row, "purge_after");
  return value;
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
  explicit CommonWork(std::shared_ptr<Session> session, bool resource_snapshot = false)
      : session_(std::move(session)), owner_(session_->owner),
        deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(2)),
        budget_(session_->connection->sock(), deadline_),
        transaction_(std::make_unique<pqxx::work>(*session_->connection)) {
    if (resource_snapshot) {
      transaction_->exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY");
    }
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
    // All business writes are complete. Flush the final safe snapshots under the
    // last lock class before commit; a failure rolls back business and journal.
    if (const auto flushed = flush_operation_observations(*this, owner_, dirty_operations_);
        std::holds_alternative<target::Failure>(flushed)) {
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

  // --- P13a owner-local lifecycle/retention extensions -------------------------------------
  target::Outcome<std::vector<target::OperationParticipant>>
  operation_participants(const target::OperationId& id) override {
    return invoke<std::vector<target::OperationParticipant>>(
        [&]() -> target::Outcome<std::vector<target::OperationParticipant>> {
          if (!valid_sql_text(id.value)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const auto rows =
              exec("SELECT participant,state,reason," + time_projection("next_attempt") +
                       " FROM kiarabot.operation_participants WHERE operation_id=$1 "
                       "ORDER BY participant",
                   {id.value});
          std::vector<target::OperationParticipant> result;
          result.reserve(rows.size());
          for (const auto& row : rows) {
            target::OperationParticipant participant;
            participant.participant = row["participant"].as<std::string>();
            participant.state = enum_value<target::OperationState>(row["state"], 6);
            if (!row["reason"].is_null()) {
              participant.reason = enum_value<target::ErrorCode>(row["reason"], 10);
            }
            participant.next_attempt = optional_time(row, "next_attempt");
            if (!valid_sql_text(participant.participant)) {
              return error(target::ErrorCode::Internal);
            }
            result.push_back(std::move(participant));
          }
          return result;
        });
  }
  target::Status save_operation_participants(
      const target::OperationId& id,
      const std::vector<target::OperationParticipant>& participants) override {
    return invoke<std::monostate>([&]() -> target::Status {
      if (!valid_sql_text(id.value) || participants.size() > 16) {
        return error(target::ErrorCode::InvalidArgument);
      }
      auto previous = operation_participants(id);
      if (const auto* failure = std::get_if<target::Failure>(&previous)) {
        return *failure;
      }
      auto ordered = participants;
      std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.participant < right.participant;
      });
      const auto& old = std::get<std::vector<target::OperationParticipant>>(previous);
      const bool unchanged = old.size() == ordered.size() &&
                             std::equal(old.begin(), old.end(), ordered.begin(),
                                        [](const auto& left, const auto& right) {
                                          return left.participant == right.participant &&
                                                 left.state == right.state &&
                                                 left.reason == right.reason &&
                                                 left.next_attempt == right.next_attempt;
                                        });
      if (unchanged) {
        return std::monostate{};
      }
      exec("DELETE FROM kiarabot.operation_participants WHERE operation_id=$1", {id.value});
      for (const auto& participant : participants) {
        if (!valid_sql_text(participant.participant)) {
          return error(target::ErrorCode::InvalidArgument);
        }
        exec("INSERT INTO kiarabot.operation_participants"
             "(operation_id,participant,state,reason,next_attempt) VALUES($1,$2,$3,$4,$5)",
             {id.value, participant.participant, static_cast<int>(participant.state),
              participant.reason ? std::optional<int>(static_cast<int>(*participant.reason))
                                 : std::nullopt,
              participant.next_attempt
                  ? std::optional<std::string>(timestamp(*participant.next_attempt))
                  : std::nullopt});
      }
      return mark_operation_dirty(id);
    });
  }
  target::Outcome<target::OperationObservationPage>
  query_operation_changes(const target::OperationObservationQuery& query) override {
    if (!observed_owner()) {
      return target::unsupported_lifecycle_read();
    }
    return query_operation_observations(*this, owner_, query);
  }
  target::Outcome<std::size_t> prune_operation_changes(std::size_t limit) override {
    if (!observed_owner()) {
      return target::unsupported_lifecycle_read();
    }
    return prune_operation_observations(*this, limit);
  }
  // v1.2 owner observation feed journal: same-UoW append, bounded read and low-water prune.
  target::Outcome<target::ObservationFeedPage>
  query_observation_changes(const target::ObservationFeedQuery& query) override {
    if (!observed_owner()) {
      return target::unsupported_lifecycle_read();
    }
    return ::kiarabot::infrastructure::postgres::detail::query_observation_changes(*this, owner_,
                                                                                  query);
  }
  target::Outcome<std::size_t>
  prune_observation_changes(target::ObservationFamily family, std::size_t limit) override {
    if (!observed_owner() || !feed_family_owns(owner_, family)) {
      return target::unsupported_lifecycle_read();
    }
    return ::kiarabot::infrastructure::postgres::detail::prune_observation_feeds(*this, family,
                                                                                limit);
  }
  target::Status append_observation_changes(
      target::ObservationFamily family,
      const std::vector<target::ObservationFeedAppend>& changes) override {
    if (!observed_owner()) {
      return target::unsupported_lifecycle_read();
    }
    return ::kiarabot::infrastructure::postgres::detail::append_observation_changes(
        *this, owner_, family, changes);
  }
  target::Outcome<std::size_t> prune_outbox(target::TimePoint now, std::size_t limit) override {
    (void)now;
    return invoke<std::size_t>([&]() -> target::Outcome<std::size_t> {
      if (limit < 1 || limit > 1000) {
        return error(target::ErrorCode::InvalidArgument);
      }
      // Only confirmed/expired/needs-attention handoffs are removable, and only after all
      // handoff obligations' 7-day receipt window elapsed. Pending/claimed obligations are never
      // deleted by retention.
      const auto rows =
          exec("DELETE FROM kiarabot.outbox WHERE ctid IN (SELECT ctid FROM kiarabot.outbox WHERE "
               "state IN (2,3,4) AND valid_until+604800000000000<="
               "(extract(epoch FROM clock_timestamp())*1000000000)::bigint "
               "ORDER BY valid_until LIMIT $1 FOR UPDATE SKIP LOCKED) RETURNING 1",
               {limit});
      return static_cast<std::size_t>(rows.size());
    });
  }
  target::Outcome<std::size_t> prune_audit(target::TimePoint cutoff, std::size_t limit) override {
    return invoke<std::size_t>([&]() -> target::Outcome<std::size_t> {
      if (limit < 1 || limit > 1000) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto rows =
          exec("DELETE FROM kiarabot.audit_entries WHERE ctid IN (SELECT ctid FROM "
               "kiarabot.audit_entries WHERE occurred_at<=$2 ORDER BY occurred_at LIMIT $1 "
               "FOR UPDATE SKIP LOCKED) RETURNING 1",
               {limit, timestamp(cutoff)});
      return static_cast<std::size_t>(rows.size());
    });
  }
  target::Outcome<std::size_t> prune_operations(target::TimePoint now, std::size_t limit) override {
    (void)now;
    return invoke<std::size_t>([&]() -> target::Outcome<std::size_t> {
      if (limit < 1 || limit > 1000) {
        return error(target::ErrorCode::InvalidArgument);
      }
      // Operations without an explicit retention deadline are P05 records and are never pruned
      // by P13a retention. Participants cascade with the operation row.
      const auto rows =
          exec("DELETE FROM kiarabot.operations WHERE ctid IN (SELECT ctid FROM "
               "kiarabot.operations WHERE retain_until IS NOT NULL AND retain_until<="
               "(extract(epoch FROM clock_timestamp())*1000000000)::bigint ORDER BY retain_until "
               "LIMIT $1 FOR UPDATE SKIP LOCKED) RETURNING 1",
               {limit});
      return static_cast<std::size_t>(rows.size());
    });
  }
  target::Outcome<target::AuditPage> query_audit(const target::AuditQuery& query) override {
    return invoke<target::AuditPage>([&]() -> target::Outcome<target::AuditPage> {
      if (query.limit == 0 || query.limit > 100 ||
          (query.filter.occurred_from && query.filter.occurred_until &&
           *query.filter.occurred_from >= *query.filter.occurred_until) ||
          (query.filter.actor && !valid_sql_text(query.filter.actor->value)) ||
          (query.filter.resource && !valid_sql_text(query.filter.resource->id.value)) ||
          (query.cursor &&
           (query.cursor->id <= 0 || query.cursor->occurred_at <= target::TimePoint{}))) {
        return error(target::ErrorCode::InvalidArgument);
      }
      pqxx::params parameters;
      std::string where{" WHERE true"};
      const auto add_time = [&](const char* column, const char* op, target::TimePoint value) {
        parameters.append(timestamp(value));
        where += std::string{" AND "} + column + op + "$" + std::to_string(parameters.size()) +
                 "::bigint";
      };
      if (query.filter.occurred_from) {
        add_time("occurred_at", ">=", *query.filter.occurred_from);
      }
      if (query.filter.occurred_until) {
        add_time("occurred_at", "<", *query.filter.occurred_until);
      }
      if (query.filter.actor) {
        parameters.append(query.filter.actor->value);
        where += " AND actor=$" + std::to_string(parameters.size());
      }
      if (query.filter.resource) {
        parameters.append(static_cast<int>(query.filter.resource->kind));
        where += " AND resource_kind=$" + std::to_string(parameters.size()) + "::int";
        parameters.append(query.filter.resource->id.value);
        where += " AND resource_id=$" + std::to_string(parameters.size());
      }
      if (query.cursor) {
        parameters.append(timestamp(query.cursor->occurred_at));
        const auto first = std::to_string(parameters.size());
        parameters.append(query.cursor->id);
        where += " AND (occurred_at,id)<($" + first + "::bigint,$" +
                 std::to_string(parameters.size()) + "::bigint)";
      }
      parameters.append(static_cast<int>(query.limit) + 1);
      const auto rows =
          exec("SELECT id,actor,resource_kind,resource_id,request_id,trace_id,method,rejection,"
               "authorization_version," +
                   time_projection("occurred_at") +
                   ",target_account_id,target_conversation_id FROM kiarabot.audit_entries" + where +
                   " ORDER BY occurred_at DESC,id DESC LIMIT $" +
                   std::to_string(parameters.size()) + "::int",
               parameters);
      target::AuditPage page;
      page.checked_at = db_now();
      for (const auto& row : rows) {
        target::AuditRecord record;
        record.id = row["id"].as<std::int64_t>();
        record.actor = {row["actor"].as<std::string>()};
        record.resource = {static_cast<target::ResourceKind>(row["resource_kind"].as<int>()),
                           {row["resource_id"].as<std::string>()}};
        record.request_id = row["request_id"].as<std::string>();
        record.trace_id = row["trace_id"].as<std::string>();
        record.method = row["method"].as<std::string>();
        if (!row["rejection"].is_null()) {
          record.rejection = static_cast<target::ErrorCode>(row["rejection"].as<int>());
        }
        record.authorization_version = {row["authorization_version"].as<std::uint64_t>()};
        record.occurred_at = time_field(row, "occurred_at");
        if (!row["target_account_id"].is_null()) {
          record.target_account_id = row["target_account_id"].as<std::string>();
        }
        if (!row["target_conversation_id"].is_null()) {
          record.target_conversation_id = row["target_conversation_id"].as<std::string>();
        }
        if (page.records.size() == query.limit) {
          page.next_cursor = target::encode_audit_cursor(
              {page.records.back().occurred_at, page.records.back().id});
          break;
        }
        page.records.push_back(std::move(record));
      }
      return page;
    });
  }
  target::Outcome<target::PrincipalDirectoryPage>
  query_principals(const target::PrincipalDirectoryQuery& query) override {
    return invoke<target::PrincipalDirectoryPage>(
        [&]() -> target::Outcome<target::PrincipalDirectoryPage> {
          if (query.limit == 0 || query.limit > 100 ||
              (!query.catalog_scope && query.visible_resources.empty()) ||
              query.visible_resources.size() > 64) {
            return target::Failure{target::ErrorCode::InvalidArgument,
                                   target::Acceptance::NotAccepted,
                                   target::Recovery::CorrectInput,
                                   {},
                                   {}};
          }
          // Human principals only; deleted/purged are never returned. The caller visibility
          // filter is either the Control management catalog or exact resource tuples. This is
          // one Identity database/UoW read; no cross-owner or cross-database access.
          std::string visibility = "TRUE";
          if (!query.catalog_scope) {
            visibility = "(";
          }
          // Parameter numbering: $1 = human kind; each visible resource consumes two parameters
          // (kind,id) used by both the binding equality and the exact-role EXISTS branch.
          std::size_t next = 2;
          if (!query.catalog_scope) {
            for (std::size_t index = 0; index < query.visible_resources.size(); ++index) {
              if (index != 0) {
                visibility += " OR ";
              }
              const auto kind_parameter = next++;
              const auto id_parameter = next++;
              visibility += "(p.binding_kind=$" + std::to_string(kind_parameter) +
                            " AND p.binding_id=$" + std::to_string(id_parameter) +
                            " OR EXISTS (SELECT 1 FROM kiarabot.exact_assignments ea "
                            "JOIN kiarabot.exact_roles er ON er.assignment=ea.id "
                            "WHERE ea.subject=p.id AND ea.state=0 AND er.resource_kind=$" +
                            std::to_string(kind_parameter) + " AND er.resource_id=$" +
                            std::to_string(id_parameter) + "))";
            }
            visibility += ")";
          }
          std::string sql =
              "SELECT p.id,p.binding_kind,p.binding_id,p.version,p.state "
              "FROM kiarabot.principals p WHERE p.kind=$1 AND p.state NOT IN (3,5) AND " +
              visibility;
          pqxx::params parameters;
          parameters.append(static_cast<int>(target::PrincipalKind::Human));
          if (!query.catalog_scope) {
            for (const auto& resource : query.visible_resources) {
              parameters.append(static_cast<int>(resource.kind));
              parameters.append(resource.id.value);
            }
          }
          if (!query.include_all_states) {
            sql += " AND p.state=$" + std::to_string(next++);
            parameters.append(static_cast<int>(query.state_filter));
          }
          if (query.after) {
            sql += " AND p.id>$" + std::to_string(next++);
            parameters.append(query.after->value);
          }
          sql += " ORDER BY p.id LIMIT $" + std::to_string(next);
          parameters.append(static_cast<int>(query.limit + 1));
          const auto rows = exec(sql, parameters);
          target::PrincipalDirectoryPage page;
          std::vector<std::string> selected;
          for (const auto& row : rows) {
            if (selected.size() == query.limit) {
              page.has_more = true;
              break;
            }
            target::Principal principal;
            principal.id = target::SubjectId{row["id"].as<std::string>()};
            principal.kind = target::PrincipalKind::Human;
            if (!row["binding_kind"].is_null() && !row["binding_id"].is_null()) {
              const int kind = row["binding_kind"].as<int>();
              if (kind < 0 || kind > 15) {
                return target::Failure{target::ErrorCode::Internal,
                                       target::Acceptance::NotAccepted,
                                       target::Recovery::Diagnose,
                                       {},
                                       {}};
              }
              principal.binding =
                  target::ResourceRef{static_cast<target::ResourceKind>(kind),
                                      target::ResourceId{row["binding_id"].as<std::string>()}};
            }
            principal.version = target::ResourceVersion{row["version"].as<std::uint64_t>()};
            const int state = row["state"].as<int>();
            if (state < 0 || state > 5) {
              return target::Failure{target::ErrorCode::Internal,
                                     target::Acceptance::NotAccepted,
                                     target::Recovery::Diagnose,
                                     {},
                                     {}};
            }
            principal.state = static_cast<target::LifecycleState>(state);
            page.entries.push_back(target::PrincipalDirectoryEntry{std::move(principal), {}});
            selected.push_back(page.entries.back().principal.id.value);
          }
          if (selected.empty()) {
            return page;
          }
          std::string ids;
          for (std::size_t index = 0; index < selected.size(); ++index) {
            if (index != 0) {
              ids += ",";
            }
            ids += transaction_->quote(selected[index]);
          }
          std::string roles_sql = "SELECT ea.subject,er.role FROM kiarabot.exact_assignments ea "
                                  "JOIN kiarabot.exact_roles er ON er.assignment=ea.id "
                                  "WHERE ea.state=0 AND ea.subject IN (" +
                                  ids + ")";
          pqxx::params role_parameters;
          std::size_t role_next = 1;
          if (!query.catalog_scope) {
            roles_sql += " AND (";
            for (std::size_t index = 0; index < query.visible_resources.size(); ++index) {
              if (index != 0) {
                roles_sql += " OR ";
              }
              roles_sql += "(er.resource_kind=$" + std::to_string(role_next++) +
                           " AND er.resource_id=$" + std::to_string(role_next++) + ")";
              role_parameters.append(static_cast<int>(query.visible_resources[index].kind));
              role_parameters.append(query.visible_resources[index].id.value);
            }
            roles_sql += ")";
          }
          roles_sql += " ORDER BY ea.subject,er.role";
          const auto role_rows = exec(roles_sql, role_parameters);
          std::map<std::string, std::vector<target::FixedRole>> roles;
          for (const auto& row : role_rows) {
            const int role = row["role"].as<int>();
            if (role < 0 || role > 2) {
              return target::Failure{target::ErrorCode::Internal,
                                     target::Acceptance::NotAccepted,
                                     target::Recovery::Diagnose,
                                     {},
                                     {}};
            }
            roles[row["subject"].as<std::string>()].push_back(static_cast<target::FixedRole>(role));
          }
          for (auto& entry : page.entries) {
            const auto found = roles.find(entry.principal.id.value);
            if (found != roles.end()) {
              entry.roles = found->second;
            }
          }
          return page;
        });
  }
  target::Outcome<target::LogPage> query_logs(const target::LogQuery& query) override {
    return invoke<target::LogPage>([&]() -> target::Outcome<target::LogPage> {
      if (query.limit == 0 || query.limit > 100) {
        return error(target::ErrorCode::InvalidArgument);
      }
      auto audit = query_audit(query);
      if (const auto* failure = std::get_if<target::Failure>(&audit)) {
        return *failure;
      }
      const auto& page = std::get<target::AuditPage>(audit);
      target::LogPage logs;
      logs.next_cursor = page.next_cursor;
      logs.checked_at = page.checked_at;
      for (const auto& record : page.records) {
        target::LogRecord entry;
        entry.id = record.id;
        entry.occurred_at = record.occurred_at;
        entry.severity = record.rejection ? "rejected" : "info";
        switch (owner_) {
        case target::Service::Identity:
          entry.component = "identity";
          break;
        case target::Service::Runtime:
          entry.component = "runtime";
          break;
        default:
          entry.component = "owner";
          break;
        }
        entry.method = record.method;
        entry.actor = record.actor;
        entry.resource = record.resource;
        entry.request_id = record.request_id;
        entry.trace_id = record.trace_id;
        entry.message = record.rejection ? "request.rejected" : "request.recorded";
        logs.records.push_back(std::move(entry));
      }
      return logs;
    });
  }
  target::Outcome<std::optional<target::LifecycleTombstone>>
  find_lifecycle_tombstone(const target::ResourceRef& resource) override {
    return invoke<std::optional<target::LifecycleTombstone>>(
        [&]() -> target::Outcome<std::optional<target::LifecycleTombstone>> {
          if (!valid_sql_text(resource.id.value)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const auto rows = exec(
              "SELECT resource_kind,resource_id,operation_id,state," +
                  time_projection("deleted_at") + "," + time_projection("restore_until") + "," +
                  time_projection("purge_after") +
                  " FROM kiarabot.lifecycle_tombstones WHERE resource_kind=$1 AND resource_id=$2",
              {static_cast<int>(resource.kind), resource.id.value});
          if (rows.empty()) {
            return std::optional<target::LifecycleTombstone>{};
          }
          return std::optional<target::LifecycleTombstone>(read_tombstone(rows.one_row()));
        });
  }
  target::Status save_lifecycle_tombstone(const target::LifecycleTombstone& value) override {
    return invoke<std::monostate>([&]() -> target::Status {
      if (!valid_sql_text(value.resource.id.value) || !valid_sql_text(value.operation_id) ||
          value.deleted_at <= target::TimePoint{} || value.restore_until <= value.deleted_at ||
          value.purge_after <= value.restore_until) {
        return error(target::ErrorCode::InvalidArgument);
      }
      exec("INSERT INTO kiarabot.lifecycle_tombstones"
           "(resource_kind,resource_id,operation_id,state,deleted_at,restore_until,purge_after) "
           "VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT(resource_kind,resource_id) DO UPDATE SET "
           "operation_id=EXCLUDED.operation_id,state=EXCLUDED.state,deleted_at=EXCLUDED.deleted_at,"
           "restore_until=EXCLUDED.restore_until,purge_after=EXCLUDED.purge_after",
           {static_cast<int>(value.resource.kind), value.resource.id.value, value.operation_id,
            static_cast<int>(value.state), timestamp(value.deleted_at),
            timestamp(value.restore_until), timestamp(value.purge_after)});
      return std::monostate{};
    });
  }
  target::Status delete_lifecycle_tombstone(const target::ResourceRef& resource) override {
    return invoke<std::monostate>([&]() -> target::Status {
      if (!valid_sql_text(resource.id.value)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      exec("DELETE FROM kiarabot.lifecycle_tombstones WHERE resource_kind=$1 AND resource_id=$2",
           {static_cast<int>(resource.kind), resource.id.value});
      return std::monostate{};
    });
  }
  target::Outcome<std::vector<target::LifecycleTombstone>>
  list_lifecycle_tombstones(std::size_t limit, const std::string& after) override {
    return invoke<std::vector<target::LifecycleTombstone>>(
        [&]() -> target::Outcome<std::vector<target::LifecycleTombstone>> {
          if (limit < 1 || limit > 100) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const auto rows =
              exec("SELECT resource_kind,resource_id,operation_id,state," +
                       time_projection("deleted_at") + "," + time_projection("restore_until") +
                       "," + time_projection("purge_after") +
                       " FROM kiarabot.lifecycle_tombstones WHERE resource_id>$1 "
                       "ORDER BY resource_id LIMIT $2",
                   {after, limit});
          std::vector<target::LifecycleTombstone> result;
          result.reserve(rows.size());
          for (const auto& row : rows) {
            result.push_back(read_tombstone(row));
          }
          return result;
        });
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
                  time_projection("next_attempt") + "," + time_projection("created_at") + "," +
                  time_projection("updated_at") + "," + time_projection("retain_until") +
                  " FROM kiarabot.operations WHERE id=$1",
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
      const auto retain = value.retain_until
                              ? std::optional<std::string>(timestamp(*value.retain_until))
                              : std::nullopt;
      if (!expected) {
        parameters.append(timestamp(value.created_at));
        parameters.append(timestamp(value.updated_at));
        parameters.append(retain);
        const auto inserted =
            exec("INSERT INTO kiarabot.operations("
                 "id,owner,resource_kind,resource_id,version,state,execution_outcome,"
                 "blocked_reason,next_attempt,created_at,updated_at,retain_until) "
                 "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12) ON CONFLICT DO NOTHING "
                 "RETURNING id",
                 parameters);
        if (inserted.empty()) {
          return error(target::ErrorCode::Conflict);
        }
      } else {
        parameters.append(timestamp(value.updated_at));
        parameters.append(retain);
        parameters.append(expected->value);
        const auto changed =
            exec("UPDATE kiarabot.operations SET "
                 "version=$5,state=$6,execution_outcome=$7,blocked_reason=$8,next_attempt=$9,"
                 "updated_at=$10,retain_until=$11 "
                 "WHERE id=$1 AND owner=$2 AND resource_kind=$3 AND resource_id=$4 AND version=$12 "
                 "RETURNING id",
                 parameters);
        if (changed.empty()) {
          return error(target::ErrorCode::Conflict);
        }
      }
      return mark_operation_dirty(value.ref.id);
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
  target::Outcome<std::vector<target::ReplayResult>> list_live(RecordKind kind, std::size_t limit,
                                                               const std::string& after) {
    return invoke<std::vector<target::ReplayResult>>(
        [&]() -> target::Outcome<std::vector<target::ReplayResult>> {
          const auto spec = record_spec(kind);
          if (spec.owner != owner()) {
            return error(target::ErrorCode::PermissionDenied);
          }
          if (limit < 1 || limit > 1024 || spec.keys.size() != 1) {
            return error(target::ErrorCode::InvalidArgument);
          }
          if (!after.empty() && !valid_sql_text(after)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const std::string columns{spec.keys.front()};
          const std::string predicate = after.empty() ? "" : " WHERE " + columns + ">$2";
          pqxx::params parameters;
          parameters.append(static_cast<int>(limit));
          if (!after.empty()) {
            parameters.append(after);
          }
          const auto rows = exec("SELECT " + columns + " FROM kiarabot." + std::string(spec.table) +
                                     predicate + " ORDER BY " + columns + " LIMIT $1",
                                 parameters);
          std::vector<target::ReplayResult> result;
          result.reserve(rows.size());
          for (const auto& row : rows) {
            std::vector<std::string> key{row[columns].as<std::string>()};
            auto loaded = load_live(*transaction_, kind, key);
            if (!loaded) {
              throw std::runtime_error("listed relation row disappeared");
            }
            result.push_back(std::move(*loaded));
          }
          return result;
        });
  }
  target::Outcome<std::size_t> active_binding_count(const std::string& adapter_instance) {
    return invoke<std::size_t>([&]() -> target::Outcome<std::size_t> {
      if (owner() != target::Service::Runtime || adapter_instance.empty() ||
          !valid_sql_text(adapter_instance)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      return exec("SELECT count(*) FROM kiarabot.adapter_bindings "
                  "WHERE adapter_instance=$1 AND state=0",
                  {adapter_instance})
          .one_row()[0]
          .as<std::size_t>();
    });
  }
  target::Outcome<std::optional<target::BridgeCredentialRecord>>
  bridge_credential_by_digest(const target::Sha256Digest& value) {
    return invoke<std::optional<target::BridgeCredentialRecord>>(
        [&]() -> target::Outcome<std::optional<target::BridgeCredentialRecord>> {
          if (owner() != target::Service::Runtime) {
            return error(target::ErrorCode::PermissionDenied);
          }
          const auto rows = exec(
              "SELECT id,resource_id,digest,created_at,revoked,version "
              "FROM kiarabot.bridge_credentials WHERE digest=$1 AND NOT revoked",
              {digest(value)});
          if (rows.empty()) {
            return std::optional<target::BridgeCredentialRecord>{};
          }
          const auto row = rows.one_row();
          target::BridgeCredentialRecord record{row["id"].as<std::string>(),
                                                row["resource_id"].as<std::string>(),
                                                read_digest(row["digest"]),
                                                time_field(row, "created_at"), false};
          record.version = row["version"].as<std::uint64_t>();
          return std::optional<target::BridgeCredentialRecord>{std::move(record)};
        });
  }
  target::Outcome<bool> bridge_credential_active(const std::string& resource_id) {
    return invoke<bool>([&]() -> target::Outcome<bool> {
      if (owner() != target::Service::Runtime || resource_id.empty() ||
          !valid_sql_text(resource_id)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      return !exec("SELECT 1 FROM kiarabot.bridge_credentials WHERE resource_id=$1 AND NOT revoked",
                   {resource_id})
                  .empty();
    });
  }
  target::Status insert_bridge_credential(const target::BridgeCredentialRecord& record) {
    return invoke<std::monostate>([&]() -> target::Status {
      if (owner() != target::Service::Runtime || record.id.empty() ||
          !valid_sql_text_fields({record.id, record.resource_id}) ||
          !valid_sql_text(record.resource_id) || record.revoked) {
        return error(target::ErrorCode::InvalidArgument);
      }
      // Legacy seeds/fixtures that materialize version 0 keep working and are stored as the frozen
      // P06 bridge version 1; the column itself never stores 0.
      const auto version = record.version == 0 ? std::uint64_t{1} : record.version;
      exec("INSERT INTO kiarabot.bridge_credentials"
           "(id,resource_id,digest,created_at,revoked,version) VALUES($1,$2,$3,$4,false,$5)",
           {record.id, record.resource_id, digest(record.digest), timestamp(record.created_at),
            version});
      return std::monostate{};
    });
  }
  target::Status revoke_bridge_credentials(const std::string& resource_id, target::TimePoint now) {
    return invoke<std::monostate>([&]() -> target::Status {
      (void)now;
      if (owner() != target::Service::Runtime || resource_id.empty() ||
          !valid_sql_text(resource_id)) {
        return error(target::ErrorCode::InvalidArgument);
      }
      exec("UPDATE kiarabot.bridge_credentials SET revoked=true "
           "WHERE resource_id=$1 AND NOT revoked",
           {resource_id});
      return std::monostate{};
    });
  }
  target::Status revoke_bridge_credential_if_current(const std::string& resource_id,
                                                     std::uint64_t expected_version,
                                                     target::TimePoint now) {
    return invoke<std::monostate>([&]() -> target::Status {
      (void)now;
      if (owner() != target::Service::Runtime || resource_id.empty() ||
          !valid_sql_text(resource_id) || expected_version == 0) {
        return error(target::ErrorCode::InvalidArgument);
      }
      const auto rows = exec(
          "UPDATE kiarabot.bridge_credentials SET revoked=true "
          "WHERE resource_id=$1 AND NOT revoked AND version=$2 RETURNING id",
          {resource_id, expected_version});
      if (rows.empty()) {
        return error(target::ErrorCode::Conflict);
      }
      return std::monostate{};
    });
  }
  target::Outcome<std::optional<target::BridgeCredentialRecord>>
  latest_bridge_credential(const std::string& resource_id) {
    return invoke<std::optional<target::BridgeCredentialRecord>>(
        [&]() -> target::Outcome<std::optional<target::BridgeCredentialRecord>> {
          if (owner() != target::Service::Runtime || resource_id.empty() ||
              !valid_sql_text(resource_id)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const auto rows =
              exec("SELECT id,resource_id,digest,created_at,revoked,version "
                   "FROM kiarabot.bridge_credentials WHERE resource_id=$1 "
                   "ORDER BY version DESC LIMIT 1",
                   {resource_id});
          if (rows.empty()) {
            return std::optional<target::BridgeCredentialRecord>{};
          }
          const auto row = rows.one_row();
          target::BridgeCredentialRecord record{row["id"].as<std::string>(),
                                                row["resource_id"].as<std::string>(),
                                                read_digest(row["digest"]),
                                                time_field(row, "created_at"),
                                                row["revoked"].as<bool>()};
          record.version = row["version"].as<std::uint64_t>();
          return std::optional<target::BridgeCredentialRecord>{std::move(record)};
        });
  }
  target::Outcome<std::optional<target::ResourceId>>
  adapter_id_by_instance(const target::InstanceId& instance) {
    return invoke<std::optional<target::ResourceId>>(
        [&]() -> target::Outcome<std::optional<target::ResourceId>> {
          if (owner() != target::Service::Runtime) {
            return error(target::ErrorCode::PermissionDenied);
          }
          if (instance.value.empty() || !valid_sql_text(instance.value)) {
            return error(target::ErrorCode::InvalidArgument);
          }
          const auto rows =
              exec("SELECT id FROM kiarabot.adapters WHERE instance=$1", {instance.value});
          if (rows.empty()) {
            return std::optional<target::ResourceId>{};
          }
          return std::optional<target::ResourceId>{
              target::ResourceId{rows.one_row()["id"].as<std::string>()}};
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
  bool observed_owner() const noexcept {
    return owner_ == target::Service::Runtime || owner_ == target::Service::Scheduler ||
           owner_ == target::Service::Model;
  }
  target::Status mark_operation_dirty(const target::OperationId& id) {
    if (observed_owner()) {
      if (dirty_operations_.size() >= 1000 && !dirty_operations_.contains(id.value)) {
        return error(target::ErrorCode::RateLimited);
      }
      dirty_operations_.insert(id.value);
    }
    return std::monostate{};
  }
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
  std::set<std::string> dirty_operations_;
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
target::Outcome<std::vector<target::ReplayResult>> list_records(target::UnitOfWork& work,
                                                                RecordKind kind, std::size_t limit,
                                                                const std::string& after) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->list_live(kind, limit, after);
  }
  return error(target::ErrorCode::Internal);
}
target::Outcome<std::size_t> active_binding_count(target::UnitOfWork& work,
                                                  const std::string& adapter_instance) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->active_binding_count(adapter_instance);
  }
  return error(target::ErrorCode::Internal);
}
target::Outcome<bool> bridge_credential_active(target::UnitOfWork& work,
                                               const std::string& resource_id) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->bridge_credential_active(resource_id);
  }
  return error(target::ErrorCode::Internal);
}
target::Outcome<std::optional<target::BridgeCredentialRecord>>
bridge_credential_by_digest(target::UnitOfWork& work, const target::Sha256Digest& digest) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->bridge_credential_by_digest(digest);
  }
  return error(target::ErrorCode::Internal);
}
target::Status insert_bridge_credential(target::UnitOfWork& work,
                                        const target::BridgeCredentialRecord& record) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->insert_bridge_credential(record);
  }
  return error(target::ErrorCode::Internal);
}
target::Status revoke_bridge_credentials(target::UnitOfWork& work, const std::string& resource_id,
                                         target::TimePoint now) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->revoke_bridge_credentials(resource_id, now);
  }
  return error(target::ErrorCode::Internal);
}
target::Outcome<std::optional<target::BridgeCredentialRecord>>
latest_bridge_credential(target::UnitOfWork& work, const std::string& resource_id) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->latest_bridge_credential(resource_id);
  }
  return error(target::ErrorCode::Internal);
}
target::Status revoke_bridge_credential_if_current(target::UnitOfWork& work,
                                                   const std::string& resource_id,
                                                   std::uint64_t expected_version,
                                                   target::TimePoint now) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->revoke_bridge_credential_if_current(resource_id, expected_version, now);
  }
  return error(target::ErrorCode::Internal);
}
target::Outcome<std::optional<target::ResourceId>>
find_adapter_id(target::UnitOfWork& work, const target::InstanceId& instance) {
  if (auto* common = dynamic_cast<CommonWork*>(&work)) {
    return common->adapter_id_by_instance(instance);
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

target::Outcome<CommonWorkAccess> begin_common_access(const std::shared_ptr<Session>& session,
                                                      bool resource_snapshot) {
  if (!session->permit.try_acquire_for(std::chrono::milliseconds(250))) {
    return error(target::ErrorCode::Conflict);
  }
  if (session->disabled) {
    session->permit.release();
    return error(target::ErrorCode::UpstreamUnavailable);
  }
  try {
    auto work = std::make_unique<CommonWork>(session, resource_snapshot);
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
