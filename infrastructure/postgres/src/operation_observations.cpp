#include "operation_observations.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace t = core::target;
constexpr std::int64_t kRetentionNs = 600000000000;

t::Failure invalid() {
  return t::rejected(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
t::Failure gap() {
  return t::rejected(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
}
bool bounded_text(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum && valid_sql_text(value);
}
bool allowed_kind(t::Service owner, t::ResourceKind kind) {
  if (owner == t::Service::Runtime) {
    return kind == t::ResourceKind::Account || kind == t::ResourceKind::Adapter ||
           kind == t::ResourceKind::Installation || kind == t::ResourceKind::Instance ||
           kind == t::ResourceKind::Host;
  }
  if (owner == t::Service::Model) {
    return kind == t::ResourceKind::Model;
  }
  return owner == t::Service::Scheduler &&
         (kind == t::ResourceKind::Task || kind == t::ResourceKind::Instance);
}
bool canonical_scope(t::Service owner, std::vector<t::ResourceRef>& scope) {
  if (scope.empty() || scope.size() > 127) {
    return false;
  }
  for (const auto& resource : scope) {
    if (!allowed_kind(owner, resource.kind) || !bounded_text(resource.id.value, 256)) {
      return false;
    }
  }
  std::sort(scope.begin(), scope.end(), [](const auto& left, const auto& right) {
    return std::pair{left.kind, left.id.value} < std::pair{right.kind, right.id.value};
  });
  return std::adjacent_find(scope.begin(), scope.end()) == scope.end();
}
std::int64_t database_now(pqxx::transaction_base& tx) {
  return tx.exec("SELECT (extract(epoch FROM clock_timestamp())*1000000000)::bigint")
      .one_row()[0]
      .as<std::int64_t>();
}
struct Head {
  std::string epoch;
  std::uint64_t sequence;
  std::uint64_t low_water;
  std::int64_t observed_at;
  std::size_t max_events;
  std::size_t max_bytes;
};
Head head(pqxx::transaction_base& tx, bool exclusive) {
  const auto row = tx.exec("SELECT epoch,sequence,low_water,observed_at,max_events,max_bytes "
                           "FROM kiarabot.operation_observation_head WHERE singleton " +
                           std::string(exclusive ? "FOR UPDATE" : "FOR SHARE"))
                       .one_row();
  return {row["epoch"].as<std::string>(),       row["sequence"].as<std::uint64_t>(),
          row["low_water"].as<std::uint64_t>(), row["observed_at"].as<std::int64_t>(),
          row["max_events"].as<std::size_t>(),  row["max_bytes"].as<std::size_t>()};
}
std::size_t prune_locked(pqxx::transaction_base& tx, const Head& current, std::size_t limit,
                         std::int64_t now) {
  // Largest retained sequence wins capacity admission; age/capacity eviction is a
  // prefix, so a single durable low-water boundary fully describes every gap.
  const auto rows =
      tx.exec("WITH ranked AS (SELECT sequence,observed_at,"
              "row_number() OVER(ORDER BY sequence DESC) AS ordinal,"
              "sum(encoded_bytes) OVER(ORDER BY sequence DESC) AS bytes FROM "
              "kiarabot.operation_observations), victims AS (SELECT sequence FROM ranked "
              "WHERE observed_at<=$1 OR ordinal>$2 OR bytes>$3 ORDER BY sequence LIMIT $4) "
              "DELETE FROM kiarabot.operation_observations o USING victims v "
              "WHERE o.sequence=v.sequence RETURNING o.sequence",
              pqxx::params{now - kRetentionNs, current.max_events, current.max_bytes, limit});
  auto low = current.low_water;
  for (const auto& row : rows) {
    low = std::max(low, row[0].as<std::uint64_t>());
  }
  if (low != current.low_water) {
    tx.exec("UPDATE kiarabot.operation_observation_head SET low_water=$1 WHERE singleton",
            pqxx::params{low});
  }
  return rows.size();
}
std::vector<t::OperationParticipant>
participants(pqxx::transaction_base& tx, const std::string& selector, pqxx::params parameters) {
  std::vector<t::OperationParticipant> result;
  const auto rows = tx.exec("SELECT participant,state,reason,next_attempt FROM kiarabot." +
                                selector + " ORDER BY participant",
                            parameters);
  if (rows.size() > 16) {
    throw std::runtime_error("invalid observation participants");
  }
  for (const auto& row : rows) {
    t::OperationParticipant value;
    value.participant = row["participant"].as<std::string>();
    value.state = static_cast<t::OperationState>(row["state"].as<int>());
    if (!row["reason"].is_null()) {
      value.reason = static_cast<t::ErrorCode>(row["reason"].as<int>());
    }
    value.next_attempt = optional_time(row, "next_attempt");
    result.push_back(std::move(value));
  }
  return result;
}
std::size_t encoded_bytes(const t::Operation& operation) {
  // Conservative bound for escaped JSON text plus numeric/envelope overhead.
  auto bytes = 512 + 6 * (operation.ref.id.value.size() + operation.ref.resource.id.value.size());
  for (const auto& participant : operation.participants) {
    bytes += 192 + 6 * participant.participant.size();
  }
  return bytes;
}
bool valid_snapshot(t::Service owner, const t::Operation& operation) {
  if (operation.ref.owner != owner || static_cast<int>(operation.ref.resource.kind) < 0 ||
      static_cast<int>(operation.ref.resource.kind) > 15 ||
      !bounded_text(operation.ref.id.value, 256) ||
      !bounded_text(operation.ref.resource.id.value, 256) || operation.version.value == 0 ||
      operation.participants.size() > 16) {
    return false;
  }
  return std::all_of(operation.participants.begin(), operation.participants.end(),
                     [](const auto& item) { return bounded_text(item.participant, 128); });
}
bool remaining_budget(pqxx::transaction_base& tx, std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) {
    return false;
  }
  const auto budget = std::min<std::int64_t>(500, remaining.count());
  tx.exec("SELECT set_config('statement_timeout',$1,true),set_config('lock_timeout',$2,true)",
          pqxx::params{std::to_string(budget) + "ms",
                       std::to_string(std::min<std::int64_t>(250, budget)) + "ms"});
  return true;
}
} // namespace

t::Status flush_operation_observations(SqlWork& work, t::Service owner,
                                       const std::set<std::string>& operation_ids) {
  if (operation_ids.empty()) {
    return std::monostate{};
  }
  return work.run([&](pqxx::transaction_base& tx) -> t::Status {
    if (operation_ids.size() > 1000) {
      return t::rejected(t::ErrorCode::RateLimited, t::Recovery::Backoff);
    }
    std::vector<t::Operation> snapshots;
    for (const auto& id : operation_ids) {
      const auto row = tx.exec("SELECT * FROM kiarabot.operations WHERE id=$1", pqxx::params{id});
      if (row.size() != 1) {
        return t::rejected(t::ErrorCode::Internal);
      }
      auto operation = read_operation(row.one_row());
      operation.participants =
          participants(tx, "operation_participants WHERE operation_id=$1", pqxx::params{id});
      if (!valid_snapshot(owner, operation)) {
        return t::rejected(t::ErrorCode::Internal);
      }
      snapshots.push_back(std::move(operation));
    }
    // This is the final lock class. No business repository is called from here on.
    auto current = head(tx, true);
    if (current.sequence > std::numeric_limits<std::uint64_t>::max() - snapshots.size()) {
      return t::rejected(t::ErrorCode::RateLimited, t::Recovery::Backoff);
    }
    const auto now = std::max(database_now(tx), current.observed_at);
    for (const auto& operation : snapshots) {
      const auto sequence = ++current.sequence;
      tx.exec("INSERT INTO kiarabot.operation_observations(sequence,id,owner,resource_kind,"
              "resource_id,version,state,execution_outcome,blocked_reason,next_attempt,"
              "observed_at,encoded_bytes) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)",
              pqxx::params{sequence, operation.ref.id.value, static_cast<int>(owner),
                           static_cast<int>(operation.ref.resource.kind),
                           operation.ref.resource.id.value, operation.version.value,
                           static_cast<int>(operation.state),
                           static_cast<int>(operation.execution_outcome),
                           operation.blocked_reason
                               ? std::optional<int>(static_cast<int>(*operation.blocked_reason))
                               : std::nullopt,
                           operation.next_attempt
                               ? std::optional<std::string>(timestamp(*operation.next_attempt))
                               : std::nullopt,
                           now, encoded_bytes(operation)});
      for (const auto& participant : operation.participants) {
        tx.exec("INSERT INTO kiarabot.operation_observation_participants"
                "(sequence,participant,state,reason,next_attempt) VALUES($1,$2,$3,$4,$5)",
                pqxx::params{sequence, participant.participant, static_cast<int>(participant.state),
                             participant.reason
                                 ? std::optional<int>(static_cast<int>(*participant.reason))
                                 : std::nullopt,
                             participant.next_attempt
                                 ? std::optional<std::string>(timestamp(*participant.next_attempt))
                                 : std::nullopt});
      }
    }
    tx.exec("UPDATE kiarabot.operation_observation_head SET sequence=$1,observed_at=$2 "
            "WHERE singleton",
            pqxx::params{current.sequence, now});
    prune_locked(tx, current, t::kOperationObservationCapacity + 1000, now);
    return std::monostate{};
  });
}

t::Outcome<t::OperationObservationPage>
query_operation_observations(SqlWork& work, t::Service owner,
                             const t::OperationObservationQuery& query) {
  auto scope = query.scope;
  if (!canonical_scope(owner, scope) || query.page_size > 100) {
    return invalid();
  }
  if (query.cursor) {
    auto previous_scope = query.cursor->scope;
    if (query.cursor->owner != owner || !bounded_text(query.cursor->epoch, 128) ||
        !canonical_scope(owner, previous_scope) || previous_scope != scope) {
      return invalid();
    }
  }
  t::OperationObservationPage result;
  const auto status = work.run([&](pqxx::transaction_base& tx) -> t::Status {
    if (!remaining_budget(tx, query.deadline)) {
      return t::rejected(t::ErrorCode::Timeout, t::Recovery::Backoff);
    }
    const auto current = head(tx, false);
    const auto now = database_now(tx);
    result.checked_at = t::TimePoint{std::chrono::nanoseconds{now}};
    result.next_cursor = {owner, current.epoch, scope, current.sequence};
    if (!query.cursor) {
      return std::monostate{};
    }
    const auto expired = tx.exec("SELECT max(sequence) FROM kiarabot.operation_observations "
                                 "WHERE observed_at<=$1",
                                 pqxx::params{now - kRetentionNs});
    const auto low_water =
        std::max(current.low_water, expired.one_row()[0].is_null()
                                        ? std::uint64_t{0}
                                        : expired.one_row()[0].as<std::uint64_t>());
    if (query.cursor->epoch != current.epoch || query.cursor->sequence < low_water ||
        query.cursor->sequence > current.sequence) {
      return gap();
    }
    const auto size = query.page_size == 0 ? 32U : query.page_size;
    pqxx::params parameters{query.cursor->sequence, current.sequence};
    std::string predicate;
    for (const auto& resource : scope) {
      if (!predicate.empty()) {
        predicate += " OR ";
      }
      const auto kind = parameters.size() + 1;
      parameters.append(static_cast<int>(resource.kind));
      const auto id = parameters.size() + 1;
      parameters.append(resource.id.value);
      predicate += "(resource_kind=$" + std::to_string(kind) + " AND resource_id=$" +
                   std::to_string(id) + ")";
    }
    const auto page_parameter = parameters.size() + 1;
    parameters.append(size + 1);
    if (!remaining_budget(tx, query.deadline)) {
      return t::rejected(t::ErrorCode::Timeout, t::Recovery::Backoff);
    }
    const auto rows =
        tx.exec("SELECT *,observed_at AS created_at,observed_at AS updated_at,"
                "NULL::bigint AS retain_until FROM kiarabot.operation_observations "
                "WHERE sequence>$1 AND sequence<=$2 AND (" +
                    predicate + ") ORDER BY sequence LIMIT $" + std::to_string(page_parameter),
                parameters);
    result.has_more = static_cast<std::size_t>(rows.size()) > size;
    for (const auto& row : rows) {
      if (result.changes.size() == size) {
        break;
      }
      if (!remaining_budget(tx, query.deadline)) {
        return t::rejected(t::ErrorCode::Timeout, t::Recovery::Backoff);
      }
      auto operation = read_operation(row);
      const auto sequence = row["sequence"].as<std::uint64_t>();
      operation.participants = participants(
          tx, "operation_observation_participants WHERE sequence=$1", pqxx::params{sequence});
      if (!valid_snapshot(owner, operation)) {
        return t::rejected(t::ErrorCode::Internal);
      }
      result.changes.push_back({std::move(operation), {owner, current.epoch, scope, sequence}});
    }
    if (result.has_more) {
      result.next_cursor.sequence = result.changes.back().cursor.sequence;
    }
    if (std::chrono::steady_clock::now() >= query.deadline) {
      return t::rejected(t::ErrorCode::Timeout, t::Recovery::Backoff);
    }
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<t::Failure>(&status)) {
    return *failure;
  }
  return result;
}

t::Outcome<std::size_t> prune_operation_observations(SqlWork& work, std::size_t limit) {
  if (limit == 0 || limit > 1000) {
    return invalid();
  }
  std::size_t count = 0;
  const auto status = work.run([&](pqxx::transaction_base& tx) -> t::Status {
    const auto current = head(tx, true);
    count = prune_locked(tx, current, limit, database_now(tx));
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<t::Failure>(&status)) {
    return *failure;
  }
  return count;
}

} // namespace kiarabot::infrastructure::postgres::detail
