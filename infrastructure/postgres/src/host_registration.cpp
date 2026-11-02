#include "host_registration.hpp"

#include <optional>
#include <stdexcept>
#include <string>

#include <pqxx/pqxx>

#include "kiarabot/core/domain/target/host_records.hpp"
#include "kiarabot/core/domain/target/invariants.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;
target::Failure failed(target::ErrorCode code,
                       target::Recovery recovery = target::Recovery::Diagnose) {
  return {code, target::Acceptance::NotAccepted, recovery, {}, {}};
}
target::Failure conflict() {
  return failed(target::ErrorCode::Conflict, target::Recovery::ReadCurrent);
}
long long ns(target::TimePoint value) {
  return static_cast<long long>(value.time_since_epoch().count());
}
target::TimePoint from_ns(long long value) {
  return target::TimePoint{std::chrono::nanoseconds{value}};
}
template <typename Value, typename Function>
target::Outcome<Value> query(SqlWork& sql, Function function) {
  std::optional<Value> value;
  const auto status = sql.run([&](pqxx::transaction_base& tx) -> target::Status {
    auto result = function(tx);
    if (const auto* failure = std::get_if<target::Failure>(&result))
      return *failure;
    value.emplace(std::move(std::get<Value>(result)));
    return std::monostate{};
  });
  if (const auto* failure = std::get_if<target::Failure>(&status))
    return *failure;
  if (!value)
    return failed(target::ErrorCode::Internal);
  return std::move(*value);
}
target::HostConnectionState read_connection(const pqxx::row& row) {
  target::HostConnectionState state;
  state.fence.host.value = row["host_id"].as<std::string>();
  state.fence.connection_generation = row["connection_generation"].as<std::uint64_t>();
  state.fence.runtime_epoch = row["runtime_epoch"].as<std::string>();
  state.registered_at = from_ns(row["registered_at"].as<long long>());
  state.last_heartbeat_at = from_ns(row["last_heartbeat_at"].as<long long>());
  state.online = row["online"].as<bool>();
  state.desired_config_version.value = row["desired_config_version"].as<std::uint64_t>();
  return state;
}
constexpr std::string_view kConnectionColumns =
    "host_id,connection_generation,runtime_epoch,registered_at,last_heartbeat_at,online,"
    "desired_config_version";
} // namespace

target::Outcome<std::optional<target::HostCertificateBinding>>
HostRegistration::certificate_binding(std::string_view certificate_sha256) {
  for (const auto& entry : allowlist_) {
    if (entry.certificate_sha256 == certificate_sha256) {
      return std::optional<target::HostCertificateBinding>{entry};
    }
  }
  return std::optional<target::HostCertificateBinding>{};
}
target::Outcome<std::optional<target::HostRegistration>>
HostRegistration::find_host(const target::ResourceId& host) {
  auto found = find_record(work_, RecordKind::Host, std::array<std::string, 1>{host.value});
  if (const auto* failure = std::get_if<target::Failure>(&found))
    return *failure;
  const auto& record = std::get<std::optional<target::ReplayResult>>(found);
  if (!record)
    return std::optional<target::HostRegistration>{};
  const auto* registration = std::get_if<target::HostRegistration>(&*record);
  if (registration == nullptr)
    return failed(target::ErrorCode::Internal);
  return std::optional<target::HostRegistration>{*registration};
}
target::Status HostRegistration::save_host(const target::HostRegistration& registration,
                                           std::optional<target::ResourceVersion> expected) {
  return save_record(work_, RecordKind::Host, registration, expected);
}
target::Outcome<std::optional<target::HostConnectionState>>
HostRegistration::connection(const target::ResourceId& host) {
  return query<std::optional<target::HostConnectionState>>(
      sql_,
      [&](pqxx::transaction_base& tx)
          -> target::Outcome<std::optional<target::HostConnectionState>> {
        const auto rows = tx.exec("SELECT " + std::string{kConnectionColumns} +
                                      " FROM kiarabot.host_connections WHERE host_id=$1",
                                  pqxx::params{host.value});
        if (rows.empty())
          return std::optional<target::HostConnectionState>{};
        return std::optional<target::HostConnectionState>{read_connection(rows[0])};
      });
}
target::Outcome<target::HostConnectionState>
HostRegistration::activate_connection(const target::HostConnectionState& connection) {
  return query<target::HostConnectionState>(
      sql_, [&](pqxx::transaction_base& tx) -> target::Outcome<target::HostConnectionState> {
        const auto rows =
            tx.exec("INSERT INTO kiarabot.host_connections(" + std::string{kConnectionColumns} +
                        ") VALUES($1,1,$2,$3,$4,true,$5) "
                        "ON CONFLICT(host_id) DO UPDATE SET "
                        "connection_generation=kiarabot.host_connections.connection_generation+1,"
                        "runtime_epoch=EXCLUDED.runtime_epoch,registered_at=EXCLUDED.registered_at,"
                        "last_heartbeat_at=EXCLUDED.last_heartbeat_at,online=true "
                        "RETURNING " +
                        std::string{kConnectionColumns},
                    pqxx::params{connection.fence.host.value, connection.fence.runtime_epoch,
                                 ns(connection.registered_at), ns(connection.last_heartbeat_at),
                                 connection.desired_config_version.value});
        return read_connection(rows[0]);
      });
}
target::Status HostRegistration::save_connection(const target::HostConnectionState& connection,
                                                 std::uint64_t expected_generation) {
  const auto status = sql_.run([&](pqxx::transaction_base& tx) -> target::Status {
    const auto rows =
        tx.exec("UPDATE kiarabot.host_connections SET last_heartbeat_at=$3,online=$4,"
                "desired_config_version=$5 WHERE host_id=$1 AND connection_generation=$2 "
                "RETURNING host_id",
                pqxx::params{connection.fence.host.value, expected_generation,
                             ns(connection.last_heartbeat_at), connection.online,
                             connection.desired_config_version.value});
    if (rows.empty())
      return conflict();
    return std::monostate{};
  });
  return status;
}
} // namespace kiarabot::infrastructure::postgres::detail
