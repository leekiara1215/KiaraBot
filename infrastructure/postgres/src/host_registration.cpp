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

namespace kiarabot::infrastructure::postgres::detail {
namespace {
constexpr std::string_view kGateColumns =
    "host_id,maintenance_version,gate_version,admission,operation_id,updated_at";
target::HostAdmissionGateRecord read_gate(const pqxx::row& row) {
  target::HostAdmissionGateRecord value;
  value.host.value = row["host_id"].as<std::string>();
  value.maintenance_version = row["maintenance_version"].as<std::uint64_t>();
  value.gate_version = row["gate_version"].as<std::uint64_t>();
  value.admission_open = row["admission"].as<int>() == 1;
  value.operation = target::OperationRef{
      target::Service::Host, target::OperationId{row["operation_id"].as<std::string>()},
      target::ResourceRef{target::ResourceKind::Host, value.host}};
  value.updated_at = from_ns(row["updated_at"].as<long long>());
  return value;
}
bool same_intent(const target::HostAdmissionGateRecord& left,
                 const target::HostAdmissionGateRecord& right) {
  return left.host == right.host && left.operation == right.operation &&
         left.maintenance_version == right.maintenance_version &&
         left.admission_open == right.admission_open;
}
} // namespace

target::Outcome<std::optional<target::HostAdmissionGateRecord>>
HostAdmissionGateRecords::current(const target::ResourceId& host) {
  return query<std::optional<target::HostAdmissionGateRecord>>(
      sql_,
      [&](pqxx::transaction_base& tx)
          -> target::Outcome<std::optional<target::HostAdmissionGateRecord>> {
        const auto rows = tx.exec("SELECT " + std::string{kGateColumns} +
                                      " FROM kiarabot.host_admission_gates WHERE host_id=$1",
                                  pqxx::params{host.value});
        if (rows.empty()) {
          return std::optional<target::HostAdmissionGateRecord>{};
        }
        return std::optional<target::HostAdmissionGateRecord>{read_gate(rows[0])};
      });
}

target::Outcome<target::HostAdmissionGateRecord>
HostAdmissionGateRecords::save(const target::HostAdmissionGateRecord& value,
                               std::optional<std::uint64_t> expected_gate_version) {
  if (value.host.value.empty() || value.operation.owner != target::Service::Host ||
      value.operation.id.value.empty() ||
      value.operation.resource != target::ResourceRef{target::ResourceKind::Host, value.host} ||
      value.maintenance_version == 0 || value.updated_at <= target::TimePoint{}) {
    return failed(target::ErrorCode::InvalidArgument, target::Recovery::CorrectInput);
  }
  return query<target::HostAdmissionGateRecord>(
      sql_, [&](pqxx::transaction_base& tx) -> target::Outcome<target::HostAdmissionGateRecord> {
        const auto rows =
            tx.exec("SELECT " + std::string{kGateColumns} +
                        " FROM kiarabot.host_admission_gates WHERE host_id=$1 FOR UPDATE",
                    pqxx::params{value.host.value});
        if (rows.empty()) {
          if (expected_gate_version && *expected_gate_version != 0) {
            return conflict();
          }
          const auto inserted =
              tx.exec("INSERT INTO kiarabot.host_admission_gates(" + std::string{kGateColumns} +
                          ") VALUES($1,$2,1,$3,$4,$5) RETURNING " + std::string{kGateColumns},
                      pqxx::params{value.host.value, value.maintenance_version,
                                   value.admission_open ? 1 : 2, value.operation.id.value,
                                   ns(value.updated_at)});
          return read_gate(inserted[0]);
        }
        const auto current = read_gate(rows[0]);
        // A replay of the exact original intent (possibly with a lost ACK) returns the same gate
        // instead of allocating a second version; any other change needs the current version.
        if (same_intent(current, value) && current.gate_version > 0) {
          return current;
        }
        if (!expected_gate_version || *expected_gate_version != current.gate_version) {
          return conflict();
        }
        const auto next = current.gate_version + 1;
        if (next == 0) {
          return failed(target::ErrorCode::Internal);
        }
        const auto updated = tx.exec(
            "UPDATE kiarabot.host_admission_gates SET maintenance_version=$2,gate_version=$3,"
            "admission=$4,operation_id=$5,updated_at=$6 WHERE host_id=$1 RETURNING " +
                std::string{kGateColumns},
            pqxx::params{value.host.value, value.maintenance_version, next,
                         value.admission_open ? 1 : 2, value.operation.id.value,
                         ns(value.updated_at)});
        return read_gate(updated[0]);
      });
}

} // namespace kiarabot::infrastructure::postgres::detail
