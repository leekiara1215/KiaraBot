#include "runtime_events.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/event_delivery.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"
#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

inline constexpr std::string_view kDbNow =
    "(extract(epoch FROM clock_timestamp())*1000000000)::bigint";
inline constexpr std::string_view kRetentionNanos = "604800000000000"; // seven days
inline constexpr std::size_t kSweepBatch = 100;

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
bool valid_id(std::string_view value) {
  return !value.empty() && value.size() <= 128 && valid_sql_text(value);
}
bool valid_text_field(std::string_view value, std::size_t maximum) {
  return value.size() <= maximum && valid_sql_text(value);
}
// Event repository calls run inside the owning Runtime UoW; a Failure poisons that transaction.
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
    return failure(target::ErrorCode::Internal);
  }
  return std::move(*value);
}
target::ConversationKey conversation(const pqxx::row& row, const char* column) {
  const auto parsed = target::conversation_key_from_text(row[column].as<std::string>());
  if (!parsed) {
    throw std::runtime_error("corrupt Runtime conversation key");
  }
  return *parsed;
}
target::EventPayloadKind payload_kind(int value) {
  if (value < 0 || value > static_cast<int>(target::EventPayloadKind::AdapterStatusChanged)) {
    throw std::runtime_error("corrupt Runtime event payload kind");
  }
  return static_cast<target::EventPayloadKind>(value);
}
target::EventRecord read_event(const pqxx::row& row) {
  target::EventRecord event;
  event.key = {{row["adapter"].as<std::string>()}, row["event_id"].as<std::string>()};
  event.fingerprint = read_digest(row["fingerprint"]);
  event.conversation = conversation(row, "conversation_key");
  event.payload = payload_kind(row["payload_kind"].as<int>());
  event.has_conversation = row["has_conversation"].as<bool>();
  event.has_sequence = row["has_sequence"].as<bool>();
  if (event.has_sequence) {
    event.sequence = row["sequence"].as<std::uint64_t>();
  }
  const auto envelope_bytes = row["serialized_envelope"].as<pqxx::bytes>();
  event.serialized_envelope.assign(reinterpret_cast<const char*>(envelope_bytes.data()),
                                   envelope_bytes.size());
  event.occurred_at = time_field(row, "occurred_at");
  if (!row["retain_until"].is_null()) {
    event.retain_until = time_field(row, "retain_until");
  }
  return event;
}
target::DeliveryRecord read_delivery_record(const pqxx::row& row) {
  target::DeliveryRecord record;
  record.delivery.event.source.value = row["adapter"].as<std::string>();
  record.delivery.event.event_id = row["event_id"].as<std::string>();
  record.delivery.instance.value = row["instance"].as<std::string>();
  record.ordered = row["ordered"].as<bool>();
  record.trace_id = row["trace_id"].as<std::string>();
  record.attempts = static_cast<std::uint32_t>(row["attempts"].as<int>());
  if (!row["current_attempt"].is_null()) {
    record.current_attempt = target::AttemptId{row["current_attempt"].as<std::string>()};
  }
  return record;
}
target::DeliveryState delivery_state(int value) {
  if (value < static_cast<int>(target::DeliveryState::Persisted) ||
      value > static_cast<int>(target::DeliveryState::DeadLetter)) {
    throw std::runtime_error("corrupt Runtime delivery state");
  }
  return static_cast<target::DeliveryState>(value);
}
target::StoredDelivery read_stored(const pqxx::row& row) {
  target::StoredDelivery stored;
  stored.record = read_delivery_record(row);
  stored.state = delivery_state(row["state"].as<int>());
  stored.next_attempt_at = time_field(row, "next_attempt_at");
  if (!row["dead_letter_reason"].is_null()) {
    const auto reason = row["dead_letter_reason"].as<int>();
    if (reason < static_cast<int>(target::ErrorCode::InvalidArgument) ||
        reason > static_cast<int>(target::ErrorCode::Internal)) {
      throw std::runtime_error("corrupt Runtime dead letter reason");
    }
    stored.dead_letter_reason = static_cast<target::ErrorCode>(reason);
  }
  stored.updated_at = time_field(row, "updated_at");
  return stored;
}
target::DeliveryAttempt read_attempt(const pqxx::row& row) {
  target::DeliveryAttempt attempt;
  attempt.delivery.event.source.value = row["adapter"].as<std::string>();
  attempt.delivery.event.event_id = row["event_id"].as<std::string>();
  attempt.delivery.instance.value = row["instance"].as<std::string>();
  attempt.attempt = target::AttemptId{row["attempt_id"].as<std::string>()};
  attempt.fence = target::Fence{
      {row["instance"].as<std::string>()},      {row["runtime_epoch"].as<std::string>()},
      {row["execution_id"].as<std::string>()},  {row["execution_generation"].as<std::uint64_t>()},
      {row["connection_id"].as<std::string>()}, {row["connection_generation"].as<std::uint64_t>()}};
  attempt.started_at = time_field(row, "started_at");
  attempt.lease_until = time_field(row, "lease_until");
  attempt.attempt_number = row["attempt_number"].as<std::uint32_t>();
  return attempt;
}
target::AttemptState attempt_state(int value) {
  if (value < static_cast<int>(target::AttemptState::Active) ||
      value > static_cast<int>(target::AttemptState::Expired)) {
    throw std::runtime_error("corrupt Runtime delivery attempt state");
  }
  return static_cast<target::AttemptState>(value);
}
target::Outcome<target::DeliveryAttempt> load_attempt(pqxx::transaction_base& transaction,
                                                      std::string_view attempt_id,
                                                      target::AttemptState* state = nullptr) {
  const auto rows = transaction.exec("SELECT * FROM kiarabot.delivery_attempts WHERE attempt_id=$1",
                                     pqxx::params{attempt_id});
  if (rows.empty()) {
    return conflict();
  }
  const auto row = rows.one_row();
  if (state) {
    *state = attempt_state(row["state"].as<int>());
  }
  return read_attempt(row);
}
// Ordered delivery head moves to the next non-terminal target in the same instance+conversation
// group; a group without pending targets keeps a NULL head so new arrivals can set it.
void advance_ordered_head(pqxx::transaction_base& transaction, std::string_view instance,
                          std::string_view conversation_key, bool had_pending) {
  if (!had_pending) {
    return;
  }
  const auto next = transaction.exec(
      "SELECT t.ordinal FROM kiarabot.delivery_targets t "
      "JOIN kiarabot.events e ON e.adapter=t.adapter AND e.event_id=t.event_id "
      "WHERE t.instance=$1 AND e.conversation_key=$2 AND t.ordered AND t.state IN (0,1,2) "
      "ORDER BY t.ordinal LIMIT 1",
      pqxx::params{instance, conversation_key});
  transaction.exec("UPDATE kiarabot.ordered_delivery_heads SET head_ordinal=$3, updated_at=" +
                       std::string(kDbNow) + " WHERE instance=$1 AND conversation_key=$2",
                   pqxx::params{instance, conversation_key,
                                next.empty() ? std::optional<std::int64_t>{}
                                             : std::optional<std::int64_t>{
                                                   next.one_row()[0].as<std::int64_t>()}});
}
void settle_event_if_terminal(pqxx::transaction_base& transaction, std::string_view adapter,
                              std::string_view event_id, target::TimePoint now) {
  const auto pending = transaction.exec(
      "SELECT count(*) FROM kiarabot.delivery_targets WHERE adapter=$1 AND event_id=$2 "
      "AND state NOT IN (3,4)",
      pqxx::params{adapter, event_id});
  if (pending.one_row()[0].as<std::size_t>() != 0) {
    return;
  }
  const auto retain = timestamp(now + target::kMinimumReceiptRetention);
  transaction.exec(
      "UPDATE kiarabot.events SET state=1, retain_until=$3 WHERE adapter=$1 AND event_id=$2",
      pqxx::params{adapter, event_id, retain});
}
void retire_attempt(pqxx::transaction_base& transaction, std::string_view attempt_id,
                    target::AttemptState state) {
  transaction.exec("UPDATE kiarabot.delivery_attempts SET state=$2 WHERE attempt_id=$1",
                   pqxx::params{attempt_id, static_cast<int>(state)});
}
void mark_failed(pqxx::transaction_base& transaction, const pqxx::row& target_row,
                 const target::DeliveryAttempt& attempt, bool retryable, target::ErrorCode reason,
                 target::TimePoint now, const target::RetryPolicy& policy, double jitter) {
  const auto record = read_delivery_record(target_row);
  const auto decision = target::fail_delivery(record, retryable, reason, now, jitter, policy);
  const auto conversation_key = conversation(target_row, "conversation_key");
  if (decision.state == target::DeliveryState::DeadLetter) {
    transaction.exec(
        "UPDATE kiarabot.delivery_targets SET state=4, current_attempt=$3, "
        "dead_letter_reason=$4, updated_at=$5, retain_until=$6 WHERE adapter=$1 AND event_id=$2 "
        "AND instance=$7",
        pqxx::params{record.delivery.event.source.value, record.delivery.event.event_id,
                     attempt.attempt.value, static_cast<int>(*decision.dead_letter_reason),
                     timestamp(now), timestamp(now + target::kMinimumReceiptRetention),
                     record.delivery.instance.value});
    retire_attempt(transaction, attempt.attempt.value, target::AttemptState::Expired);
    advance_ordered_head(transaction, record.delivery.instance.value,
                         target::conversation_key_text(conversation_key), record.ordered);
    settle_event_if_terminal(transaction, record.delivery.event.source.value,
                             record.delivery.event.event_id, now);
    return;
  }
  transaction.exec(
      "UPDATE kiarabot.delivery_targets SET state=1, current_attempt=NULL, "
      "next_attempt_at=$3, updated_at=$4 WHERE adapter=$1 AND event_id=$2 AND instance=$5",
      pqxx::params{record.delivery.event.source.value, record.delivery.event.event_id,
                   timestamp(*decision.next_attempt_at), timestamp(now),
                   record.delivery.instance.value});
  retire_attempt(transaction, attempt.attempt.value, target::AttemptState::Expired);
}
} // namespace

target::Outcome<target::EventAcceptanceResult>
RuntimeEvents::accept(const target::EventRecord& event,
                      const std::vector<target::DeliveryRecord>& targets,
                      const target::EventCapacityLimits& limits) {
  return query<target::EventAcceptanceResult>(
      sql_,
      [&](pqxx::transaction_base& transaction) -> target::Outcome<target::EventAcceptanceResult> {
        if (!valid_id(event.key.source.value) || !valid_id(event.key.event_id) ||
            event.serialized_envelope.size() > 1024 * 1024) {
          return invalid();
        }
        std::vector<target::InstanceId> instances;
        for (const auto& target : targets) {
          if (!valid_id(target.delivery.instance.value) ||
              !valid_text_field(target.trace_id, 128) || target.delivery.event != event.key) {
            return invalid();
          }
          instances.push_back(target.delivery.instance);
        }
        std::sort(instances.begin(), instances.end(),
                  [](const auto& left, const auto& right) { return left.value < right.value; });
        if (std::adjacent_find(instances.begin(), instances.end(),
                               [](const auto& left, const auto& right) {
                                 return left.value == right.value;
                               }) != instances.end()) {
          return invalid();
        }
        const auto existing = transaction.exec(
            "SELECT fingerprint FROM kiarabot.events WHERE adapter=$1 AND event_id=$2 FOR UPDATE",
            pqxx::params{event.key.source.value, event.key.event_id});
        if (!existing.empty()) {
          if (read_digest(existing.one_row()["fingerprint"]) == event.fingerprint) {
            return target::EventAcceptanceResult{true, {}, 0};
          }
          return conflict();
        }
        const auto used_events =
            transaction.exec("SELECT count(*) FROM kiarabot.events WHERE state=0")
                .one_row()[0]
                .as<std::size_t>();
        if (used_events >= limits.events) {
          return failure(target::ErrorCode::RateLimited, target::Recovery::Backoff);
        }
        for (const auto& instance : instances) {
          const auto used = transaction
                                .exec("SELECT count(*) FROM kiarabot.delivery_targets WHERE "
                                      "instance=$1 AND state IN (0,1,2)",
                                      pqxx::params{instance.value})
                                .one_row()[0]
                                .as<std::size_t>();
          if (used >= limits.instance_deliveries) {
            return failure(target::ErrorCode::RateLimited, target::Recovery::Backoff);
          }
        }
        target::SequenceWatermark watermark;
        const auto stored = transaction.exec(
            "SELECT has_sequence, last_sequence, degraded FROM kiarabot.conversation_watermarks "
            "WHERE conversation_key=$1 FOR UPDATE",
            pqxx::params{target::conversation_key_text(event.conversation)});
        if (!stored.empty()) {
          const auto& row = stored.one_row();
          if (row["has_sequence"].as<bool>()) {
            watermark.last_sequence = row["last_sequence"].as<std::uint64_t>();
          }
          watermark.degraded = row["degraded"].as<bool>();
        }
        const auto decision =
            target::observe_sequence(watermark.last_sequence.has_value() || watermark.degraded
                                         ? std::optional<target::SequenceWatermark>{watermark}
                                         : std::nullopt,
                                     event.has_sequence ? event.sequence : std::nullopt);
        if (decision.verdict == target::SequenceVerdict::OutOfOrder) {
          return conflict();
        }
        const auto sequence_parameter =
            decision.watermark.last_sequence
                ? std::optional<std::uint64_t>{*decision.watermark.last_sequence}
                : std::optional<std::uint64_t>{};
        transaction.exec(
            "INSERT INTO kiarabot.conversation_watermarks("
            "conversation_key,has_sequence,last_sequence,degraded,updated_at) "
            "VALUES($1,$2,$3,$4," +
                std::string(kDbNow) +
                ") ON CONFLICT(conversation_key) DO UPDATE SET "
                "has_sequence=EXCLUDED.has_sequence,last_sequence=EXCLUDED.last_sequence,"
                "degraded=EXCLUDED.degraded,updated_at=EXCLUDED.updated_at",
            pqxx::params{target::conversation_key_text(event.conversation),
                         decision.watermark.last_sequence.has_value(), sequence_parameter,
                         decision.watermark.degraded});
        const bool terminal = targets.empty();
        const auto retain_sql =
            terminal ? "(" + std::string(kDbNow) + ") + " + std::string(kRetentionNanos)
                     : std::string("NULL");
        transaction.exec(
            "INSERT INTO kiarabot.events(adapter,event_id,fingerprint,conversation_key,"
            "payload_kind,has_conversation,sequence,has_sequence,serialized_envelope,occurred_at,"
            "retain_until,state,received_at) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10," +
                retain_sql + ",$11," + std::string(kDbNow) + ")",
            pqxx::params{event.key.source.value, event.key.event_id, digest(event.fingerprint),
                         target::conversation_key_text(event.conversation),
                         static_cast<int>(event.payload), event.has_conversation,
                         event.has_sequence ? event.sequence : std::optional<std::uint64_t>{},
                         event.has_sequence, pqxx::binarystring{event.serialized_envelope},
                         timestamp(event.occurred_at), terminal ? 1 : 0});
        const auto conversation_key = target::conversation_key_text(event.conversation);
        for (const auto& target : targets) {
          const auto inserted = transaction.exec(
              "INSERT INTO kiarabot.delivery_targets(adapter,event_id,instance,ordered,trace_id,"
              "state,attempts,next_attempt_at,current_attempt,dead_letter_reason,updated_at,"
              "retain_until) VALUES($1,$2,$3,$4,$5,0,0,$6,NULL,NULL," +
                  std::string(kDbNow) + ",NULL) RETURNING ordinal",
              pqxx::params{event.key.source.value, event.key.event_id,
                           target.delivery.instance.value, target.ordered, target.trace_id,
                           timestamp(event.occurred_at)});
          if (target.ordered) {
            transaction.exec(
                "INSERT INTO kiarabot.ordered_delivery_heads("
                "instance,conversation_key,head_ordinal,updated_at) VALUES($1,$2,$3," +
                    std::string(kDbNow) +
                    ") ON CONFLICT(instance,conversation_key) DO UPDATE SET "
                    "head_ordinal=COALESCE(kiarabot.ordered_delivery_heads.head_ordinal,"
                    "EXCLUDED.head_ordinal),updated_at=EXCLUDED.updated_at",
                pqxx::params{target.delivery.instance.value, conversation_key,
                             inserted.one_row()[0].as<std::int64_t>()});
          }
        }
        return target::EventAcceptanceResult{false, decision, targets.size()};
      });
}

target::Outcome<std::vector<target::DeliveryClaim>>
RuntimeEvents::claim(const target::DeliveryClaimRequest& request) {
  return query<std::vector<target::DeliveryClaim>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::DeliveryClaim>> {
        if (!valid_id(request.instance.value) || !target::valid_fence(request.fence) ||
            request.fence.instance != request.instance || request.limit == 0 ||
            request.limit > 100 || request.attempt_ids.size() < request.limit ||
            request.lease <= std::chrono::seconds::zero() ||
            request.lease > target::kDeliveryLease) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT t.*, e.fingerprint, e.conversation_key, e.payload_kind, e.has_conversation, "
            "e.sequence, e.has_sequence, e.serialized_envelope, e.occurred_at, e.retain_until "
            "FROM kiarabot.delivery_targets t "
            "JOIN kiarabot.events e ON e.adapter=t.adapter AND e.event_id=t.event_id "
            "WHERE t.instance=$1 AND t.state IN (0,1) AND t.next_attempt_at <= $2 "
            "AND (NOT t.ordered OR EXISTS (SELECT 1 FROM kiarabot.ordered_delivery_heads h "
            "  WHERE h.instance=t.instance AND h.conversation_key=e.conversation_key "
            "  AND h.head_ordinal=t.ordinal)) "
            "ORDER BY t.ordinal LIMIT $3 FOR UPDATE OF t",
            pqxx::params{request.instance.value, timestamp(request.now), request.limit});
        std::vector<target::DeliveryClaim> claims;
        std::size_t index = 0;
        for (const auto& row : rows) {
          const auto attempt_id = request.attempt_ids.at(index++);
          const auto attempt_number = row["attempts"].as<std::uint32_t>() + 1;
          transaction.exec(
              "INSERT INTO kiarabot.delivery_attempts(attempt_id,adapter,event_id,instance,"
              "runtime_epoch,execution_id,execution_generation,connection_id,"
              "connection_generation,started_at,lease_until,attempt_number,state) "
              "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,0)",
              pqxx::params{attempt_id.value, row["adapter"].as<std::string>(),
                           row["event_id"].as<std::string>(), row["instance"].as<std::string>(),
                           request.fence.runtime_epoch.value, request.fence.execution.value,
                           request.fence.execution_generation.value, request.fence.connection.value,
                           request.fence.connection_generation.value, timestamp(request.now),
                           timestamp(request.now + request.lease),
                           static_cast<int>(attempt_number)});
          transaction.exec(
              "UPDATE kiarabot.delivery_targets SET state=2, attempts=$3, current_attempt=$4, "
              "updated_at=" +
                  std::string(kDbNow) + " WHERE adapter=$1 AND event_id=$2 AND instance=$5",
              pqxx::params{row["adapter"].as<std::string>(), row["event_id"].as<std::string>(),
                           static_cast<int>(attempt_number), attempt_id.value,
                           row["instance"].as<std::string>()});
          target::DeliveryClaim claim;
          claim.attempt = {
              read_delivery_record(row).delivery, attempt_id,    request.fence, request.now,
              request.now + request.lease,        attempt_number};
          claim.event = read_event(row);
          claim.trace_id = row["trace_id"].as<std::string>();
          claim.ordered = row["ordered"].as<bool>();
          claims.push_back(std::move(claim));
        }
        return claims;
      });
}

target::Outcome<target::DeliveryReceipt>
RuntimeEvents::acknowledge(const target::DeliveryAcknowledgement& acknowledgement,
                           target::TimePoint now) {
  return query<target::DeliveryReceipt>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::DeliveryReceipt> {
        const auto supplied = acknowledgement.attempt;
        if (!valid_id(supplied.attempt.value) || !valid_id(supplied.delivery.event.source.value) ||
            !valid_id(supplied.delivery.event.event_id) ||
            !valid_id(supplied.delivery.instance.value) || !target::valid_fence(supplied.fence) ||
            supplied.fence.instance != supplied.delivery.instance) {
          return invalid();
        }
        if (!acknowledgement.accepted && !acknowledgement.failure) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT t.*, e.conversation_key FROM kiarabot.delivery_targets t "
            "JOIN kiarabot.events e ON e.adapter=t.adapter AND e.event_id=t.event_id "
            "WHERE t.adapter=$1 AND t.event_id=$2 AND t.instance=$3 FOR UPDATE OF t",
            pqxx::params{supplied.delivery.event.source.value, supplied.delivery.event.event_id,
                         supplied.delivery.instance.value});
        if (rows.empty()) {
          return failure(target::ErrorCode::NotFound);
        }
        const auto& target_row = rows.one_row();
        const auto state = delivery_state(target_row["state"].as<int>());
        if (!target_row["current_attempt"].is_null()) {
          const auto current_id = target_row["current_attempt"].as<std::string>();
          auto current = load_attempt(transaction, current_id);
          if (const auto* rejected = std::get_if<target::Failure>(&current)) {
            return *rejected;
          }
          const auto disposition = target::classify_acknowledgement(
              state, std::get<target::DeliveryAttempt>(current), supplied, now);
          if (disposition == target::DeliveryDisposition::Duplicate) {
            return target::DeliveryReceipt{std::get<target::DeliveryAttempt>(current),
                                           target::DeliveryDisposition::Duplicate, std::nullopt,
                                           now};
          }
          if (state != target::DeliveryState::Dispatched ||
              *disposition != target::DeliveryDisposition::Acknowledged) {
            return target::DeliveryReceipt{std::get<target::DeliveryAttempt>(current),
                                           target::DeliveryDisposition::Stale,
                                           target::Failure{target::ErrorCode::Conflict,
                                                           target::Acceptance::NotAccepted,
                                                           target::Recovery::ReadCurrent,
                                                           {},
                                                           {}},
                                           now};
          }
          const auto current_attempt = std::get<target::DeliveryAttempt>(current);
          if (acknowledgement.accepted) {
            retire_attempt(transaction, current_attempt.attempt.value,
                           target::AttemptState::Acknowledged);
            transaction.exec(
                "UPDATE kiarabot.delivery_targets SET state=3, updated_at=$4, retain_until=$5 "
                "WHERE adapter=$1 AND event_id=$2 AND instance=$3",
                pqxx::params{supplied.delivery.event.source.value, supplied.delivery.event.event_id,
                             supplied.delivery.instance.value, timestamp(now),
                             timestamp(now + target::kMinimumReceiptRetention)});
            advance_ordered_head(transaction, supplied.delivery.instance.value,
                                 target_row["conversation_key"].as<std::string>(),
                                 read_delivery_record(target_row).ordered);
            settle_event_if_terminal(transaction, supplied.delivery.event.source.value,
                                     supplied.delivery.event.event_id, now);
            return target::DeliveryReceipt{
                current_attempt, target::DeliveryDisposition::Acknowledged, std::nullopt, now};
          }
          if (!acknowledgement.failure) {
            return invalid();
          }
          retire_attempt(transaction, current_attempt.attempt.value,
                         target::AttemptState::Cancelled);
          mark_failed(transaction, target_row, current_attempt, acknowledgement.retryable,
                      acknowledgement.failure->code, now, acknowledgement.retry,
                      acknowledgement.jitter_sample);
          return target::DeliveryReceipt{current_attempt, target::DeliveryDisposition::Rejected,
                                         acknowledgement.failure, now};
        }
        return target::DeliveryReceipt{supplied, target::DeliveryDisposition::Stale,
                                       target::Failure{target::ErrorCode::Conflict,
                                                       target::Acceptance::NotAccepted,
                                                       target::Recovery::ReadCurrent,
                                                       {},
                                                       {}},
                                       now};
      });
}

target::Outcome<target::DeliveryReceipt>
RuntimeEvents::renew(const target::DeliveryAttempt& supplied, target::TimePoint now) {
  return query<target::DeliveryReceipt>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::DeliveryReceipt> {
        if (!valid_id(supplied.attempt.value) || !valid_id(supplied.delivery.event.source.value) ||
            !valid_id(supplied.delivery.event.event_id) ||
            !valid_id(supplied.delivery.instance.value) || !target::valid_fence(supplied.fence) ||
            supplied.fence.instance != supplied.delivery.instance) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT current_attempt, state FROM kiarabot.delivery_targets "
            "WHERE adapter=$1 AND event_id=$2 AND instance=$3 FOR UPDATE",
            pqxx::params{supplied.delivery.event.source.value, supplied.delivery.event.event_id,
                         supplied.delivery.instance.value});
        if (rows.empty()) {
          return failure(target::ErrorCode::NotFound);
        }
        const auto& row = rows.one_row();
        if (delivery_state(row["state"].as<int>()) != target::DeliveryState::Dispatched ||
            row["current_attempt"].is_null() ||
            row["current_attempt"].as<std::string>() != supplied.attempt.value) {
          return target::DeliveryReceipt{supplied, target::DeliveryDisposition::Stale,
                                         target::Failure{target::ErrorCode::Conflict,
                                                         target::Acceptance::NotAccepted,
                                                         target::Recovery::ReadCurrent,
                                                         {},
                                                         {}},
                                         now};
        }
        auto current = load_attempt(transaction, supplied.attempt.value);
        if (const auto* rejected = std::get_if<target::Failure>(&current)) {
          return *rejected;
        }
        const auto disposition =
            target::classify_renewal(std::get<target::DeliveryAttempt>(current), supplied, now);
        if (*disposition != target::DeliveryDisposition::Renewed) {
          return target::DeliveryReceipt{std::get<target::DeliveryAttempt>(current),
                                         target::DeliveryDisposition::Stale,
                                         target::Failure{target::ErrorCode::Conflict,
                                                         target::Acceptance::NotAccepted,
                                                         target::Recovery::ReadCurrent,
                                                         {},
                                                         {}},
                                         now};
        }
        auto renewed = std::get<target::DeliveryAttempt>(current);
        renewed.lease_until = target::renewed_lease_until(renewed, now);
        transaction.exec("UPDATE kiarabot.delivery_attempts SET lease_until=$2 WHERE attempt_id=$1",
                         pqxx::params{supplied.attempt.value, timestamp(renewed.lease_until)});
        return target::DeliveryReceipt{renewed, target::DeliveryDisposition::Renewed, std::nullopt,
                                       now};
      });
}

target::Outcome<target::DeliverySweep> RuntimeEvents::sweep(target::TimePoint now,
                                                            const target::RetryPolicy& policy,
                                                            double jitter_sample) {
  return query<target::DeliverySweep>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::DeliverySweep> {
        const auto rows = transaction.exec(
            "SELECT t.*, e.conversation_key, a.attempt_id FROM kiarabot.delivery_targets t "
            "JOIN kiarabot.events e ON e.adapter=t.adapter AND e.event_id=t.event_id "
            "JOIN kiarabot.delivery_attempts a ON a.attempt_id=t.current_attempt "
            "WHERE t.state=2 AND a.lease_until <= $1 "
            "ORDER BY t.ordinal LIMIT $2 FOR UPDATE OF t",
            pqxx::params{timestamp(now), static_cast<int>(kSweepBatch)});
        target::DeliverySweep result;
        for (const auto& row : rows) {
          auto attempt = load_attempt(transaction, row["attempt_id"].as<std::string>());
          if (const auto* rejected = std::get_if<target::Failure>(&attempt)) {
            return *rejected;
          }
          mark_failed(transaction, row, std::get<target::DeliveryAttempt>(attempt), true,
                      target::ErrorCode::Timeout, now, policy, jitter_sample);
          ++result.expired_attempts;
          const auto state = transaction
                                 .exec("SELECT state FROM kiarabot.delivery_targets WHERE "
                                       "adapter=$1 AND event_id=$2 AND instance=$3",
                                       pqxx::params{row["adapter"].as<std::string>(),
                                                    row["event_id"].as<std::string>(),
                                                    row["instance"].as<std::string>()})
                                 .one_row()[0]
                                 .as<int>();
          if (delivery_state(state) == target::DeliveryState::DeadLetter) {
            ++result.dead_lettered;
          } else {
            ++result.retrying;
          }
        }
        return result;
      });
}

target::Outcome<std::size_t> RuntimeEvents::recover(target::TimePoint now,
                                                    const target::RetryPolicy& policy,
                                                    double jitter_sample) {
  return query<std::size_t>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
        const auto rows = transaction.exec(
            "SELECT t.*, e.conversation_key, a.attempt_id FROM kiarabot.delivery_targets t "
            "JOIN kiarabot.events e ON e.adapter=t.adapter AND e.event_id=t.event_id "
            "JOIN kiarabot.delivery_attempts a ON a.attempt_id=t.current_attempt "
            "WHERE t.state=2 ORDER BY t.ordinal LIMIT $1 FOR UPDATE OF t",
            pqxx::params{static_cast<int>(kSweepBatch)});
        std::size_t recovered = 0;
        for (const auto& row : rows) {
          auto attempt = load_attempt(transaction, row["attempt_id"].as<std::string>());
          if (const auto* rejected = std::get_if<target::Failure>(&attempt)) {
            return *rejected;
          }
          mark_failed(transaction, row, std::get<target::DeliveryAttempt>(attempt), true,
                      target::ErrorCode::Timeout, now, policy, jitter_sample);
          ++recovered;
        }
        return recovered;
      });
}

target::Outcome<std::vector<target::DeadLetter>>
RuntimeEvents::list_dead_letters(const std::optional<target::InstanceId>& instance,
                                 std::size_t limit) {
  return query<std::vector<target::DeadLetter>>(
      sql_,
      [&](pqxx::transaction_base& transaction) -> target::Outcome<std::vector<target::DeadLetter>> {
        if (limit == 0 || limit > 1000 || (instance && !valid_id(instance->value))) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT t.*, a.attempt_id FROM kiarabot.delivery_targets t "
            "LEFT JOIN kiarabot.delivery_attempts a ON a.attempt_id=t.current_attempt "
            "WHERE t.state=4 AND ($1::text IS NULL OR t.instance=$1::text) "
            "ORDER BY t.ordinal LIMIT $2",
            pqxx::params{instance ? std::optional<std::string>{instance->value}
                                  : std::optional<std::string>{},
                         static_cast<int>(limit)});
        std::vector<target::DeadLetter> letters;
        for (const auto& row : rows) {
          if (row["attempt_id"].is_null()) {
            return failure(target::ErrorCode::Internal);
          }
          const auto attempt = load_attempt(transaction, row["attempt_id"].as<std::string>());
          if (const auto* rejected = std::get_if<target::Failure>(&attempt)) {
            return *rejected;
          }
          const auto stored = read_stored(row);
          letters.push_back(
              target::DeadLetter{stored.record.delivery, std::get<target::DeliveryAttempt>(attempt),
                                 stored.dead_letter_reason.value_or(target::ErrorCode::Internal),
                                 stored.updated_at.value_or(target::TimePoint{})});
        }
        return letters;
      });
}

target::Outcome<std::optional<target::DeliveryRecord>>
RuntimeEvents::replay(const target::ReplayRequest& request) {
  return query<std::optional<target::DeliveryRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::DeliveryRecord>> {
        if (!valid_id(request.delivery.event.source.value) ||
            !valid_id(request.delivery.event.event_id) ||
            !valid_id(request.delivery.instance.value) || !valid_id(request.operation.value) ||
            !valid_text_field(request.trace_id, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT t.*, e.conversation_key FROM kiarabot.delivery_targets t "
            "JOIN kiarabot.events e ON e.adapter=t.adapter AND e.event_id=t.event_id "
            "WHERE t.adapter=$1 AND t.event_id=$2 AND t.instance=$3 FOR UPDATE OF t",
            pqxx::params{request.delivery.event.source.value, request.delivery.event.event_id,
                         request.delivery.instance.value});
        if (rows.empty()) {
          return std::optional<target::DeliveryRecord>{};
        }
        const auto& row = rows.one_row();
        const auto stored = read_stored(row);
        if (stored.state != target::DeliveryState::DeadLetter) {
          return conflict();
        }
        transaction.exec("UPDATE kiarabot.delivery_targets SET state=0, attempts=0, "
                         "next_attempt_at=$4, current_attempt=NULL, dead_letter_reason=NULL, "
                         "updated_at=$5, retain_until=NULL, trace_id=$6 "
                         "WHERE adapter=$1 AND event_id=$2 AND instance=$3",
                         pqxx::params{request.delivery.event.source.value,
                                      request.delivery.event.event_id,
                                      request.delivery.instance.value, timestamp(request.now),
                                      timestamp(request.now), request.trace_id});
        transaction.exec(
            "UPDATE kiarabot.events SET state=0, retain_until=NULL WHERE adapter=$1 AND "
            "event_id=$2",
            pqxx::params{request.delivery.event.source.value, request.delivery.event.event_id});
        if (stored.record.ordered) {
          transaction.exec("INSERT INTO kiarabot.ordered_delivery_heads("
                           "instance,conversation_key,head_ordinal,updated_at) VALUES($1,$2,$3," +
                               std::string(kDbNow) +
                               ") ON CONFLICT(instance,conversation_key) DO UPDATE SET "
                               "head_ordinal=COALESCE(kiarabot.ordered_delivery_heads.head_ordinal,"
                               "EXCLUDED.head_ordinal),updated_at=EXCLUDED.updated_at",
                           pqxx::params{request.delivery.instance.value,
                                        row["conversation_key"].as<std::string>(),
                                        row["ordinal"].as<std::int64_t>()});
        }
        auto replayed = read_delivery_record(row);
        replayed.attempts = 0;
        replayed.current_attempt.reset();
        return std::optional<target::DeliveryRecord>{replayed};
      });
}

target::Outcome<target::EventCapacity>
RuntimeEvents::capacity(const target::EventCapacityLimits& limits) {
  return query<target::EventCapacity>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::EventCapacity> {
        target::EventCapacity result;
        result.pending_events =
            transaction.exec("SELECT count(*) FROM kiarabot.events WHERE state=0")
                .one_row()[0]
                .as<std::size_t>();
        result.maximum_instance_deliveries =
            transaction
                .exec("SELECT COALESCE(max(c),0) FROM (SELECT count(*) AS c FROM "
                      "kiarabot.delivery_targets WHERE state IN (0,1,2) GROUP BY instance) counts")
                .one_row()[0]
                .as<std::size_t>();
        result.events_saturated = result.pending_events >= limits.events;
        result.deliveries_saturated =
            result.maximum_instance_deliveries >= limits.instance_deliveries;
        return result;
      });
}

target::Outcome<std::size_t> RuntimeEvents::prune_expired(target::TimePoint now,
                                                          std::size_t limit) {
  return query<std::size_t>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
        if (limit == 0 || limit > 1000) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "DELETE FROM kiarabot.events WHERE (adapter,event_id) IN ("
            "SELECT adapter,event_id FROM kiarabot.events WHERE state=1 AND retain_until IS NOT "
            "NULL AND retain_until <= $1 LIMIT $2) RETURNING adapter",
            pqxx::params{timestamp(now), static_cast<int>(limit)});
        return static_cast<std::size_t>(rows.size());
      });
}

} // namespace kiarabot::infrastructure::postgres::detail
