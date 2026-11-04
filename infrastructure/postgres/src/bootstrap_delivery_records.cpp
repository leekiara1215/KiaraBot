#include "bootstrap_delivery_records.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <pqxx/pqxx>

#include "record_codec.hpp"

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
bool valid_identifier(std::string_view value) {
  return !value.empty() && value.size() <= 128 && valid_text(value);
}
// The delivery relation stores the wire ResourceKind numbering (ACCOUNT=1, INSTANCE=5). Only the
// two bootstrap families are legal; every other value is corruption, not an authorizable delivery.
bool valid_wire_kind(int kind) {
  return kind == 1 || kind == 5;
}
int wire_kind(target::ResourceKind kind) {
  return static_cast<int>(kind) + 1;
}
target::ResourceKind domain_kind(int kind) {
  return static_cast<target::ResourceKind>(kind - 1);
}
bool valid_state(int state) {
  return state >= 0 && state <= static_cast<int>(target::BootstrapDeliveryState::Revoked);
}
bool valid_record(const target::BootstrapDeliveryRecord& record) {
  return valid_identifier(record.delivery_id) && valid_identifier(record.resource.id.value) &&
         valid_identifier(record.recipient_id) && valid_identifier(record.operation.id.value) &&
         record.operation.owner == target::Service::Runtime &&
         record.operation.resource == record.resource && record.credential_version > 0 &&
         valid_state(static_cast<int>(record.state)) && valid_text(record.receipt_id) &&
         record.receipt_id.size() <= 128;
}
template <typename Value, typename Function>
target::Outcome<Value> query(SqlWork& sql, Function function) {
  std::optional<Value> value;
  const auto status = sql.run([&](pqxx::transaction_base& transaction) -> target::Status {
    auto result = function(transaction);
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
constexpr std::string_view kFields =
    "delivery_id,resource_kind,resource_id,recipient_id,credential_version,state,operation_id,"
    "created_at,observed_at,receipt_id";
target::BootstrapDeliveryRecord read_row(const pqxx::row& row) {
  const auto kind = row["resource_kind"].as<int>();
  if (!valid_wire_kind(kind) || !valid_state(row["state"].as<int>())) {
    throw std::runtime_error("invalid stored bootstrap delivery");
  }
  target::BootstrapDeliveryRecord record;
  record.delivery_id = row["delivery_id"].as<std::string>();
  record.resource = {domain_kind(kind), {row["resource_id"].as<std::string>()}};
  record.recipient_id = row["recipient_id"].as<std::string>();
  record.credential_version = row["credential_version"].as<std::uint64_t>();
  record.state = static_cast<target::BootstrapDeliveryState>(row["state"].as<int>());
  record.operation = {
      target::Service::Runtime, {row["operation_id"].as<std::string>()}, record.resource};
  record.created_at = time_field(row, "created_at");
  record.observed_at = time_field(row, "observed_at");
  record.receipt_id = row["receipt_id"].as<std::string>();
  if (!valid_record(record)) {
    throw std::runtime_error("invalid stored bootstrap delivery");
  }
  return record;
}
pqxx::params record_key(const target::ResourceRef& resource, const std::string& delivery_id) {
  return {wire_kind(resource.kind), resource.id.value, delivery_id};
}
} // namespace

core::target::Outcome<std::optional<core::target::BootstrapDeliveryRecord>>
BootstrapDeliveryRecords::find(const core::target::ResourceRef& resource,
                               const std::string& delivery_id) {
  if (!valid_identifier(resource.id.value) || !valid_identifier(delivery_id)) {
    return invalid();
  }
  return query<std::optional<core::target::BootstrapDeliveryRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> core::target::Outcome<std::optional<core::target::BootstrapDeliveryRecord>> {
        const auto rows =
            transaction.exec("SELECT " + std::string{kFields} +
                                 " FROM kiarabot.bootstrap_deliveries "
                                 "WHERE resource_kind=$1 AND resource_id=$2 AND delivery_id=$3",
                             record_key(resource, delivery_id));
        if (rows.empty()) {
          return std::optional<core::target::BootstrapDeliveryRecord>{};
        }
        return std::optional<core::target::BootstrapDeliveryRecord>{read_row(rows.one_row())};
      });
}

core::target::Outcome<std::optional<core::target::BootstrapDeliveryRecord>>
BootstrapDeliveryRecords::current(const core::target::ResourceRef& resource) {
  if (!valid_identifier(resource.id.value)) {
    return invalid();
  }
  return query<std::optional<core::target::BootstrapDeliveryRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> core::target::Outcome<std::optional<core::target::BootstrapDeliveryRecord>> {
        const auto rows = transaction.exec(
            "SELECT " + std::string{kFields} +
                " FROM kiarabot.bootstrap_deliveries WHERE resource_kind=$1 AND resource_id=$2 "
                "ORDER BY credential_version DESC LIMIT 1",
            pqxx::params{wire_kind(resource.kind), resource.id.value});
        if (rows.empty()) {
          return std::optional<core::target::BootstrapDeliveryRecord>{};
        }
        return std::optional<core::target::BootstrapDeliveryRecord>{read_row(rows.one_row())};
      });
}

core::target::Status
BootstrapDeliveryRecords::insert(const core::target::BootstrapDeliveryRecord& record) {
  if (!valid_record(record)) {
    return invalid();
  }
  return query<std::monostate>(
      sql_, [&](pqxx::transaction_base& transaction) -> core::target::Status {
        transaction.exec("INSERT INTO kiarabot.bootstrap_deliveries(" + std::string{kFields} +
                             ") "
                             "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                         pqxx::params{record.delivery_id, wire_kind(record.resource.kind),
                                      record.resource.id.value, record.recipient_id,
                                      record.credential_version, static_cast<int>(record.state),
                                      record.operation.id.value, timestamp(record.created_at),
                                      timestamp(record.observed_at), record.receipt_id});
        return std::monostate{};
      });
}

core::target::Status
BootstrapDeliveryRecords::transition(const core::target::BootstrapDeliveryRecord& next,
                                     core::target::BootstrapDeliveryState expected_state) {
  if (!valid_record(next) || !valid_state(static_cast<int>(expected_state))) {
    return invalid();
  }
  return query<std::monostate>(
      sql_, [&](pqxx::transaction_base& transaction) -> core::target::Status {
        const auto updated = transaction.exec(
            "UPDATE kiarabot.bootstrap_deliveries SET state=$4,observed_at=$5,receipt_id=$6 "
            "WHERE resource_kind=$1 AND resource_id=$2 AND delivery_id=$3 AND state=$7",
            pqxx::params{wire_kind(next.resource.kind), next.resource.id.value, next.delivery_id,
                         static_cast<int>(next.state), timestamp(next.observed_at), next.receipt_id,
                         static_cast<int>(expected_state)});
        if (updated.affected_rows() != 1) {
          return conflict();
        }
        return std::monostate{};
      });
}

} // namespace kiarabot::infrastructure::postgres::detail
