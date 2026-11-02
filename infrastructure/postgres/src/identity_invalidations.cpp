// clang-format off
#include "identity_security.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "identity_access.hpp"
#include "record_codec.hpp"
// clang-format on

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

using identity::conflict;
using identity::invalid;
using identity::query;
using identity::require_stored;

constexpr std::size_t kMaximumReadBatch = 256;
constexpr std::size_t kMaximumPruneBatch = 1024;

bool valid_epoch(const std::string& epoch) {
  return !epoch.empty() && epoch.size() <= 64 && valid_sql_text(epoch);
}
std::vector<target::ResourceRef> read_scopes(pqxx::transaction_base& transaction,
                                             const std::string& epoch, std::uint64_t sequence) {
  const auto rows = transaction.exec(
      "SELECT resource_kind,resource_id FROM kiarabot.identity_invalidation_scopes WHERE "
      "epoch=$1 AND sequence=$2 ORDER BY position",
      pqxx::params{epoch, sequence});
  std::vector<target::ResourceRef> scope;
  for (const auto& row : rows) {
    scope.push_back({static_cast<target::ResourceKind>(row["resource_kind"].as<int>()),
                     {row["resource_id"].as<std::string>()}});
  }
  return scope;
}
std::optional<target::InvalidationStreamState> load_state(pqxx::transaction_base& transaction) {
  const auto rows = transaction.exec(
      "SELECT epoch,next_sequence FROM kiarabot.identity_invalidation_state WHERE singleton=true");
  if (rows.empty()) {
    return std::nullopt;
  }
  return target::InvalidationStreamState{rows.one_row()["epoch"].as<std::string>(),
                                         rows.one_row()["next_sequence"].as<std::uint64_t>()};
}
target::InvalidationRecord read_record(pqxx::transaction_base& transaction, const pqxx::row& row) {
  target::InvalidationRecord record;
  record.epoch = row["epoch"].as<std::string>();
  record.sequence = row["sequence"].as<std::uint64_t>();
  record.created_at = time_field(row, "created_at");
  record.event.principal = {row["principal"].as<std::string>()};
  record.event.authorization_version = {row["authorization_version"].as<std::uint64_t>()};
  record.event.scope = read_scopes(transaction, record.epoch, record.sequence);
  require_stored(
      std::holds_alternative<std::monostate>(target::validate_invalidation_event(record.event)));
  return record;
}
class Invalidations final : public target::IdentityInvalidationRepository {
public:
  explicit Invalidations(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::InvalidationStreamState>> state() override {
    return query<std::optional<target::InvalidationStreamState>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::InvalidationStreamState>> {
          return load_state(transaction);
        });
  }
  target::Status initialize(const target::InvalidationStreamState& state) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      if (!valid_epoch(state.epoch) || state.next_sequence != 0) {
        return invalid();
      }
      transaction.exec("INSERT INTO kiarabot.identity_invalidation_state VALUES(true,$1,0) "
                       "ON CONFLICT(singleton) DO NOTHING",
                       pqxx::params{state.epoch});
      return std::monostate{};
    });
  }
  target::Outcome<std::optional<std::uint64_t>> oldest_sequence(const std::string& epoch) override {
    return query<std::optional<std::uint64_t>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::optional<std::uint64_t>> {
          if (!valid_epoch(epoch)) {
            return invalid();
          }
          const auto rows = transaction.exec(
              "SELECT MIN(sequence) FROM kiarabot.identity_invalidations WHERE epoch=$1",
              pqxx::params{epoch});
          if (rows.empty() || rows.one_row()[0].is_null()) {
            return std::optional<std::uint64_t>{};
          }
          return std::optional<std::uint64_t>{rows.one_row()[0].template as<std::uint64_t>()};
        });
  }
  target::Outcome<std::vector<target::InvalidationRecord>>
  read_after(const std::string& epoch, std::uint64_t sequence, std::size_t limit) override {
    return query<std::vector<target::InvalidationRecord>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::vector<target::InvalidationRecord>> {
          if (!valid_epoch(epoch) || limit == 0 || limit > kMaximumReadBatch) {
            return invalid();
          }
          const auto rows = transaction.exec(
              "SELECT epoch,sequence,principal,authorization_version,created_at "
              "FROM kiarabot.identity_invalidations WHERE epoch=$1 AND sequence>$2 "
              "ORDER BY sequence LIMIT $3",
              pqxx::params{epoch, sequence, static_cast<std::int64_t>(limit)});
          std::vector<target::InvalidationRecord> result;
          for (const auto& row : rows) {
            result.push_back(read_record(transaction, row));
          }
          return result;
        });
  }
  target::Outcome<target::InvalidationRecord> append(const target::InvalidationEvent& event,
                                                     target::TimePoint now) override {
    return query<target::InvalidationRecord>(
        sql_, [&](auto& transaction) -> target::Outcome<target::InvalidationRecord> {
          const auto valid = target::validate_invalidation_event(event);
          if (const auto* failure = std::get_if<target::Failure>(&valid)) {
            return *failure;
          }
          const auto state_rows = transaction.exec(
              "SELECT epoch,next_sequence FROM kiarabot.identity_invalidation_state "
              "WHERE singleton=true FOR UPDATE");
          if (state_rows.empty()) {
            return conflict();
          }
          const auto epoch = state_rows.one_row()["epoch"].template as<std::string>();
          const auto current = state_rows.one_row()["next_sequence"].template as<std::uint64_t>();
          if (current == std::numeric_limits<std::uint64_t>::max()) {
            return target::rejected(target::ErrorCode::Internal, target::Recovery::Diagnose);
          }
          const auto sequence = current + 1;
          transaction.exec("UPDATE kiarabot.identity_invalidation_state SET next_sequence=$1 "
                           "WHERE singleton=true",
                           pqxx::params{sequence});
          transaction.exec("INSERT INTO kiarabot.identity_invalidations VALUES($1,$2,$3,$4,$5)",
                           pqxx::params{epoch, sequence, event.principal.value,
                                        event.authorization_version.value, timestamp(now)});
          for (std::size_t index = 0; index < event.scope.size(); ++index) {
            transaction.exec(
                "INSERT INTO kiarabot.identity_invalidation_scopes VALUES($1,$2,$3,$4,$5)",
                pqxx::params{epoch, sequence, static_cast<std::int64_t>(index),
                             static_cast<int>(event.scope[index].kind),
                             event.scope[index].id.value});
          }
          return target::InvalidationRecord{epoch, sequence, event, now};
        });
  }
  target::Outcome<std::size_t> prune(target::TimePoint now, std::size_t limit) override {
    return query<std::size_t>(sql_, [&](auto& transaction) -> target::Outcome<std::size_t> {
      if (limit == 0 || limit > kMaximumPruneBatch) {
        return invalid();
      }
      const auto state = load_state(transaction);
      if (!state) {
        return std::size_t{0};
      }
      const auto oldest_allowed = state->next_sequence > target::kInvalidationCapacity
                                      ? state->next_sequence - target::kInvalidationCapacity
                                      : 0;
      const auto expired = now > target::TimePoint{} + target::kInvalidationRetention
                               ? now - target::kInvalidationRetention
                               : target::TimePoint{};
      const auto rows = transaction.exec(
          "DELETE FROM kiarabot.identity_invalidations WHERE (epoch,sequence) IN ("
          "SELECT epoch,sequence FROM kiarabot.identity_invalidations "
          "WHERE created_at<$1 OR (epoch=$2 AND sequence<=$3) "
          "ORDER BY sequence LIMIT $4) RETURNING sequence",
          pqxx::params{timestamp(expired), state->epoch, static_cast<std::uint64_t>(oldest_allowed),
                       static_cast<std::int64_t>(limit)});
      return static_cast<std::size_t>(rows.size());
    });
  }

private:
  SqlWork& sql_;
};
} // namespace

std::unique_ptr<target::IdentityInvalidationRepository> invalidation_repository(SqlWork& sql) {
  return std::make_unique<Invalidations>(sql);
}
} // namespace kiarabot::infrastructure::postgres::detail
