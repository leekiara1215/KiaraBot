#include "storage_records.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/domain/target/namespace_rules.hpp"
#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

inline constexpr std::string_view kDbNow =
    "(extract(epoch FROM clock_timestamp())*1000000000)::bigint";
inline constexpr std::string_view kReceiptWhere =
    "subject=$1 AND method=$2 AND resource_kind=$3 AND resource_id=$4 AND idempotency_key=$5";

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
target::Failure not_found() {
  return failure(target::ErrorCode::NotFound, target::Recovery::ReadCurrent);
}
target::Failure storage_failure() {
  return failure(target::ErrorCode::Internal);
}
bool valid_text(std::string_view value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum && valid_sql_text(value);
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
std::string scope_id(const target::NamespaceScope& scope) {
  if (const auto* shared = std::get_if<target::SharedNamespace>(&scope)) {
    return shared->shared_namespace.value;
  }
  return std::get<target::PrivateNamespace>(scope).instance.value;
}
target::TimePoint db_now(pqxx::transaction_base& transaction) {
  return time_field(transaction.exec("SELECT " + std::string{kDbNow} + " AS now").one_row(), "now");
}
std::string scope_installation(const target::NamespaceScope& scope) {
  if (const auto* shared = std::get_if<target::SharedNamespace>(&scope)) {
    return shared->installation.value;
  }
  return {};
}
// Private namespaces are lazily created with the authenticated instance as stable identity; a
// missing installation row means the instance is not a registered Runtime instance.
std::optional<std::string> ensure_namespace(pqxx::transaction_base& transaction,
                                            const target::NamespaceScope& scope,
                                            target::TimePoint now) {
  const auto id = scope_id(scope);
  if (!valid_text(id, 128)) {
    return std::nullopt;
  }
  const auto rows =
      transaction.exec("SELECT namespace_id,installation_id,kind FROM kiarabot.namespaces"
                       " WHERE namespace_id=$1 FOR UPDATE",
                       pqxx::params{id});
  if (!rows.empty()) {
    const auto kind = rows.one_row()["kind"].as<int>();
    const bool shared = std::holds_alternative<target::SharedNamespace>(scope);
    if ((shared && kind != 1) || (!shared && kind != 0)) {
      return std::nullopt;
    }
    return id;
  }
  if (std::holds_alternative<target::SharedNamespace>(scope)) {
    return std::nullopt; // shared namespaces must be created through the management API
  }
  const auto instances =
      transaction.exec("SELECT installation FROM kiarabot.instances WHERE id=$1", pqxx::params{id});
  if (instances.empty()) {
    return std::nullopt;
  }
  transaction.exec(
      "INSERT INTO kiarabot.namespaces(namespace_id,installation_id,kind,owner_instance,disabled,"
      "version,created_at,updated_at) VALUES($1,$2,0,$1,false,1,$3,$3)",
      pqxx::params{id, instances.one_row()[0].as<std::string>(), timestamp(now)});
  return id;
}
std::string namespace_installation(pqxx::transaction_base& transaction,
                                   const std::string& namespace_id) {
  return transaction
      .exec("SELECT installation_id FROM kiarabot.namespaces WHERE namespace_id=$1",
            pqxx::params{namespace_id})
      .one_row()[0]
      .as<std::string>();
}
void adjust_counter(pqxx::transaction_base& transaction, int kind, const std::string& id,
                    std::int64_t keys, std::int64_t bytes, target::TimePoint now) {
  const auto existing =
      transaction.exec("SELECT key_count,byte_count FROM kiarabot.namespace_quota_counters"
                       " WHERE scope_kind=$1 AND scope_id=$2 FOR UPDATE",
                       pqxx::params{kind, id});
  if (existing.empty()) {
    if (keys < 0 || bytes < 0) {
      throw std::runtime_error("negative namespace counter");
    }
    transaction.exec(
        "INSERT INTO kiarabot.namespace_quota_counters(scope_kind,scope_id,key_count,byte_count,"
        "updated_at) VALUES($1,$2,$3,$4,$5)",
        pqxx::params{kind, id, keys, bytes, timestamp(now)});
    return;
  }
  const auto row = existing.one_row();
  const auto next_keys = row["key_count"].as<std::int64_t>() + keys;
  const auto next_bytes = row["byte_count"].as<std::int64_t>() + bytes;
  if (next_keys < 0 || next_bytes < 0) {
    throw std::runtime_error("negative namespace counter");
  }
  transaction.exec(
      "UPDATE kiarabot.namespace_quota_counters SET key_count=$3,byte_count=$4,updated_at=$5"
      " WHERE scope_kind=$1 AND scope_id=$2",
      pqxx::params{kind, id, next_keys, next_bytes, timestamp(now)});
}
void adjust_counters(pqxx::transaction_base& transaction, const std::string& namespace_id,
                     std::int64_t keys, std::int64_t bytes, target::TimePoint now) {
  adjust_counter(transaction, 0, namespace_id, keys, bytes, now);
  adjust_counter(transaction, 1, namespace_installation(transaction, namespace_id), keys, bytes,
                 now);
}
// Expired values are lazily removed under the namespace row lock and their quota occupancy is
// released in the same transaction before any read/write decision.
std::optional<target::NamespaceValue> read_value(pqxx::transaction_base& transaction,
                                                 const std::string& namespace_id,
                                                 const std::string& key, target::TimePoint now) {
  const auto rows =
      transaction.exec("SELECT value,version,expires_at,updated_at FROM kiarabot.namespace_values"
                       " WHERE namespace_id=$1 AND key=$2 FOR UPDATE",
                       pqxx::params{namespace_id, key});
  if (rows.empty()) {
    return std::nullopt;
  }
  const auto row = rows.one_row();
  if (!row["expires_at"].is_null()) {
    const auto expires = time_field(row, "expires_at");
    if (expires <= now) {
      const auto size = static_cast<std::int64_t>(row["value"].as<pqxx::bytes>().size());
      transaction.exec("DELETE FROM kiarabot.namespace_values WHERE namespace_id=$1 AND key=$2",
                       pqxx::params{namespace_id, key});
      adjust_counters(transaction, namespace_id, -1, -size, now);
      return std::nullopt;
    }
  }
  target::NamespaceValue value;
  const auto bytes = row["value"].as<pqxx::bytes>();
  value.bytes.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                     reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size());
  value.version = {row["version"].as<std::uint64_t>()};
  if (!row["expires_at"].is_null()) {
    value.expires_at = time_field(row, "expires_at");
  }
  return value;
}
target::SharedNamespaceRecord read_namespace(const pqxx::row& row) {
  return {{row["namespace_id"].as<std::string>()}, {row["installation_id"].as<std::string>()},
          {row["version"].as<std::uint64_t>()},    row["disabled"].as<bool>(),
          time_field(row, "created_at"),           time_field(row, "updated_at")};
}
target::NamespaceReceiptResult read_receipt_result(pqxx::transaction_base& transaction,
                                                   std::int64_t result_id) {
  const auto rows = transaction.exec(
      "SELECT result_kind,instance_id,installation_id,scope_kind,scope_id,key,deleted,version,"
      "expires_at,value,namespace_id,namespace_installation,namespace_version,namespace_disabled,"
      "namespace_created_at,namespace_updated_at,members_version FROM"
      " kiarabot.namespace_receipt_results WHERE result_id=$1",
      pqxx::params{result_id});
  if (rows.empty()) {
    throw std::runtime_error("storage receipt without typed result");
  }
  const auto row = rows.one_row();
  const auto kind = row["result_kind"].as<int>();
  if (kind == 0) {
    target::StorageMutationReceipt result;
    if (row["scope_kind"].as<int>() == 0) {
      result.scope = target::PrivateNamespace{{row["scope_id"].as<std::string>()}};
    } else {
      result.scope = target::SharedNamespace{{row["namespace_installation"].as<std::string>()},
                                             {row["scope_id"].as<std::string>()}};
    }
    result.key = row["key"].as<std::string>();
    result.deleted = row["deleted"].as<bool>();
    if (!row["version"].is_null()) {
      result.version = target::ResourceVersion{row["version"].as<std::uint64_t>()};
    }
    if (!row["expires_at"].is_null()) {
      result.expires_at = time_field(row, "expires_at");
    }
    if (!row["value"].is_null()) {
      const auto bytes = row["value"].as<pqxx::bytes>();
      result.value.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                          reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size());
    }
    return result;
  }
  if (kind == 1) {
    target::SharedNamespaceReceipt result;
    result.namespace_record = {{row["namespace_id"].as<std::string>()},
                               {row["namespace_installation"].as<std::string>()},
                               {row["namespace_version"].as<std::uint64_t>()},
                               row["namespace_disabled"].as<bool>(),
                               time_field(row, "namespace_created_at"),
                               time_field(row, "namespace_updated_at")};
    return result;
  }
  return target::SharedMembersReceipt{{row["members_version"].as<std::uint64_t>()}};
}
} // namespace

target::Outcome<std::optional<target::NamespaceValue>>
NamespaceRecords::get(const target::NamespaceScope& scope, const std::string& key,
                      target::TimePoint now) {
  return query<std::optional<target::NamespaceValue>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::NamespaceValue>> {
        if (!valid_text(key, target::kMaximumStorageKeyBytes) || !positive(now)) {
          return invalid();
        }
        const auto namespace_id = ensure_namespace(transaction, scope, now);
        if (!namespace_id) {
          return not_found();
        }
        return read_value(transaction, *namespace_id, key, now);
      });
}

target::Outcome<target::ResourceVersion>
NamespaceRecords::compare_exchange(const target::NamespaceScope& scope, const std::string& key,
                                   const target::CompareCondition& expected,
                                   const target::NamespaceValue& value, target::TimePoint now) {
  return query<target::ResourceVersion>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::ResourceVersion> {
        if (!valid_text(key, target::kMaximumStorageKeyBytes) || !positive(now) ||
            !target::valid_storage_value(value.bytes.size()) || value.version.value == 0) {
          return invalid();
        }
        const auto namespace_id = ensure_namespace(transaction, scope, now);
        if (!namespace_id) {
          return not_found();
        }
        const auto current = read_value(transaction, *namespace_id, key, now);
        const auto* absent = std::get_if<target::ExpectedAbsent>(&expected);
        if (absent) {
          if (current) {
            return conflict();
          }
          const auto bytes = value.bytes.size();
          const auto rows = transaction.exec(
              "INSERT INTO kiarabot.namespace_values(namespace_id,key,value,version,expires_at,"
              "updated_at) VALUES($1,$2,$3,$4,$5,$6) RETURNING version",
              pqxx::params{*namespace_id, key, pqxx::binarystring{value.bytes.data(), bytes},
                           value.version.value,
                           value.expires_at
                               ? std::optional<std::string>{timestamp(*value.expires_at)}
                               : std::nullopt,
                           timestamp(now)});
          return target::ResourceVersion{rows.one_row()[0].as<std::uint64_t>()};
        }
        const auto& expected_version = std::get<target::ResourceVersion>(expected);
        if (!current || current->version != expected_version) {
          return conflict();
        }
        const auto bytes = value.bytes.size();
        const auto rows = transaction.exec(
            "UPDATE kiarabot.namespace_values SET value=$3,version=$4,expires_at=$5,updated_at=$6"
            " WHERE namespace_id=$1 AND key=$2 AND version=$7 RETURNING version",
            pqxx::params{*namespace_id, key, pqxx::binarystring{value.bytes.data(), bytes},
                         value.version.value,
                         value.expires_at ? std::optional<std::string>{timestamp(*value.expires_at)}
                                          : std::nullopt,
                         timestamp(now), expected_version.value});
        if (rows.empty()) {
          return conflict();
        }
        static_cast<void>(bytes);
        return target::ResourceVersion{rows.one_row()[0].as<std::uint64_t>()};
      });
}

target::Status NamespaceRecords::compare_remove(const target::NamespaceScope& scope,
                                                const std::string& key,
                                                const target::CompareCondition& expected,
                                                target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_text(key, target::kMaximumStorageKeyBytes) || !positive(now)) {
      return invalid();
    }
    if (std::holds_alternative<target::ExpectedAbsent>(expected)) {
      return invalid();
    }
    const auto namespace_id = ensure_namespace(transaction, scope, now);
    if (!namespace_id) {
      return not_found();
    }
    const auto current = read_value(transaction, *namespace_id, key, now);
    const auto& expected_version = std::get<target::ResourceVersion>(expected);
    if (!current || current->version != expected_version) {

      return conflict();
    }
    const auto rows = transaction.exec(
        "DELETE FROM kiarabot.namespace_values WHERE namespace_id=$1 AND key=$2 AND version=$3"
        " RETURNING 1",
        pqxx::params{*namespace_id, key, expected_version.value});
    if (rows.affected_rows() != 1) {
      return conflict();
    }
    return std::monostate{};
  });
}

target::Outcome<std::optional<target::SharedNamespaceRecord>>
SharedNamespaceRecords::find(const target::ResourceId& namespace_id) {
  return query<std::optional<target::SharedNamespaceRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::SharedNamespaceRecord>> {
        if (!valid_text(namespace_id.value, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT namespace_id,installation_id,version,disabled,created_at,updated_at"
            " FROM kiarabot.namespaces WHERE namespace_id=$1 AND kind=1",
            pqxx::params{namespace_id.value});
        if (rows.empty()) {
          return std::optional<target::SharedNamespaceRecord>{};
        }
        return std::optional<target::SharedNamespaceRecord>{read_namespace(rows.one_row())};
      });
}

target::Outcome<std::optional<target::SharedNamespaceRecord>>
SharedNamespaceRecords::by_installation(const target::InstallationId& installation) {
  return query<std::optional<target::SharedNamespaceRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::SharedNamespaceRecord>> {
        if (!valid_text(installation.value, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT namespace_id,installation_id,version,disabled,created_at,updated_at"
            " FROM kiarabot.namespaces WHERE installation_id=$1 AND kind=1",
            pqxx::params{installation.value});
        if (rows.empty()) {
          return std::optional<target::SharedNamespaceRecord>{};
        }
        return std::optional<target::SharedNamespaceRecord>{read_namespace(rows.one_row())};
      });
}

target::Status SharedNamespaceRecords::save(const target::SharedNamespaceRecord& record,
                                            std::optional<target::ResourceVersion> expected) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_text(record.namespace_id.value, 128) ||
        !valid_text(record.installation.value, 128) || record.version.value == 0 ||
        !positive(record.created_at) || !positive(record.updated_at)) {
      return invalid();
    }
    if (!expected) {
      const auto inserted = transaction.exec(
          "INSERT INTO kiarabot.namespaces(namespace_id,installation_id,kind,owner_instance,"
          "disabled,version,created_at,updated_at) VALUES($1,$2,1,NULL,$3,$4,$5,$6)"
          " ON CONFLICT DO NOTHING RETURNING 1",
          pqxx::params{record.namespace_id.value, record.installation.value, record.disabled,
                       record.version.value, timestamp(record.created_at),
                       timestamp(record.updated_at)});
      if (inserted.affected_rows() != 1) {
        return conflict();
      }
      return std::monostate{};
    }
    if (record.version.value <= expected->value) {
      return invalid();
    }
    const auto updated =
        transaction.exec("UPDATE kiarabot.namespaces SET disabled=$3,version=$4,updated_at=$5"
                         " WHERE namespace_id=$1 AND kind=1 AND version=$2 RETURNING 1",
                         pqxx::params{record.namespace_id.value, expected->value, record.disabled,
                                      record.version.value, timestamp(record.updated_at)});
    if (updated.affected_rows() != 1) {
      return conflict();
    }
    return std::monostate{};
  });
}

target::Outcome<std::vector<target::SharedMemberRecord>>
SharedNamespaceRecords::members(const target::ResourceId& namespace_id) {
  return query<std::vector<target::SharedMemberRecord>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::vector<target::SharedMemberRecord>> {
        if (!valid_text(namespace_id.value, 128)) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "SELECT instance_id,can_read,can_write,version FROM kiarabot.namespace_members"
            " WHERE namespace_id=$1 ORDER BY instance_id",
            pqxx::params{namespace_id.value});
        std::vector<target::SharedMemberRecord> result;
        for (const auto& row : rows) {
          result.push_back({{row["instance_id"].as<std::string>(), row["can_read"].as<bool>(),
                             row["can_write"].as<bool>()},
                            {row["version"].as<std::uint64_t>()}});
        }
        return result;
      });
}

target::Status
SharedNamespaceRecords::save_members(const target::ResourceId& namespace_id,
                                     target::ResourceVersion expected,
                                     const std::vector<target::SharedMember>& members,
                                     target::ResourceVersion next_version, target::TimePoint now) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_text(namespace_id.value, 128) || expected.value == 0 || next_version.value == 0 ||
        next_version.value <= expected.value || !positive(now) ||
        !target::valid_shared_members(members)) {
      return invalid();
    }
    const auto locked = transaction.exec("SELECT version,installation_id FROM kiarabot.namespaces"
                                         " WHERE namespace_id=$1 AND kind=1 FOR UPDATE",
                                         pqxx::params{namespace_id.value});
    if (locked.empty()) {
      return not_found();
    }
    if (target::ResourceVersion{locked.one_row()["version"].as<std::uint64_t>()} != expected) {
      return conflict();
    }
    transaction.exec("DELETE FROM kiarabot.namespace_members WHERE namespace_id=$1",
                     pqxx::params{namespace_id.value});
    for (const auto& member : members) {
      if (!valid_text(member.instance.value, 128)) {
        return invalid();
      }
      transaction.exec(
          "INSERT INTO kiarabot.namespace_members(namespace_id,instance_id,can_read,can_write,"
          "version,joined_at) VALUES($1,$2,$3,$4,$5,$6)",
          pqxx::params{namespace_id.value, member.instance.value, member.can_read, member.can_write,
                       next_version.value, timestamp(now)});
    }
    transaction.exec(
        "UPDATE kiarabot.namespaces SET version=$2,updated_at=$3 WHERE namespace_id=$1",
        pqxx::params{namespace_id.value, next_version.value, timestamp(now)});
    return std::monostate{};
  });
}

target::Outcome<target::NamespaceQuota>
NamespaceQuotaRecords::usage(const target::NamespaceScope& scope,
                             const target::InstallationId& installation) {
  return query<target::NamespaceQuota>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<target::NamespaceQuota> {
        if (!valid_text(installation.value, 128)) {
          return invalid();
        }
        const auto id = scope_id(scope);
        const auto namespace_rows =
            transaction.exec("SELECT key_count,byte_count FROM kiarabot.namespace_quota_counters"
                             " WHERE scope_kind=0 AND scope_id=$1",
                             pqxx::params{id});
        const auto installation_rows =
            transaction.exec("SELECT key_count,byte_count FROM kiarabot.namespace_quota_counters"
                             " WHERE scope_kind=1 AND scope_id=$1",
                             pqxx::params{installation.value});
        target::NamespaceQuota quota;
        if (!namespace_rows.empty()) {
          quota.namespace_keys_used =
              static_cast<std::size_t>(namespace_rows.one_row()["key_count"].as<std::int64_t>());
          quota.namespace_bytes_used =
              static_cast<std::size_t>(namespace_rows.one_row()["byte_count"].as<std::int64_t>());
        }
        if (!installation_rows.empty()) {
          quota.installation_keys_used =
              static_cast<std::size_t>(installation_rows.one_row()["key_count"].as<std::int64_t>());
          quota.installation_bytes_used = static_cast<std::size_t>(
              installation_rows.one_row()["byte_count"].as<std::int64_t>());
        }
        return quota;
      });
}

target::Status NamespaceQuotaRecords::admit(const target::NamespaceScope& scope,
                                            const target::InstallationId& installation,
                                            target::NamespaceQuotaChange change) {
  return query<std::monostate>(sql_, [&](pqxx::transaction_base& transaction) -> target::Status {
    if (!valid_text(installation.value, 128) || !valid_text(scope_id(scope), 128) ||
        (change.keys == 0 && change.bytes == 0)) {
      return invalid();
    }
    const auto id = scope_id(scope);
    // Fixed lock order: namespace counter first, then installation aggregate.
    auto apply = [&](int kind, const std::string& counter_id, std::int64_t key_limit,
                     std::int64_t byte_limit) -> target::Status {
      const auto existing =
          transaction.exec("SELECT key_count,byte_count FROM kiarabot.namespace_quota_counters"
                           " WHERE scope_kind=$1 AND scope_id=$2 FOR UPDATE",
                           pqxx::params{kind, counter_id});
      std::int64_t keys = change.keys;
      std::int64_t bytes = change.bytes;
      if (!existing.empty()) {
        keys += existing.one_row()["key_count"].as<std::int64_t>();
        bytes += existing.one_row()["byte_count"].as<std::int64_t>();
      }
      if (keys < 0 || bytes < 0 || keys > key_limit || bytes > byte_limit) {
        return rate_limited();
      }
      const auto now = db_now(transaction);
      if (existing.empty()) {
        transaction.exec(
            "INSERT INTO kiarabot.namespace_quota_counters(scope_kind,scope_id,key_count,"
            "byte_count,updated_at) VALUES($1,$2,$3,$4,$5)",
            pqxx::params{kind, counter_id, keys, bytes, timestamp(now)});
      } else {
        transaction.exec("UPDATE kiarabot.namespace_quota_counters SET key_count=$3,byte_count=$4,"
                         "updated_at=$5 WHERE scope_kind=$1 AND scope_id=$2",
                         pqxx::params{kind, counter_id, keys, bytes, timestamp(now)});
      }
      return std::monostate{};
    };
    const auto namespace_limit = static_cast<std::int64_t>(target::kMaximumNamespaceKeys);
    const auto namespace_bytes = static_cast<std::int64_t>(target::kMaximumNamespaceBytes);
    auto first = apply(0, id, namespace_limit, namespace_bytes);
    if (const auto* rejected = std::get_if<target::Failure>(&first)) {
      return *rejected;
    }
    auto second =
        apply(1, installation.value, static_cast<std::int64_t>(target::kMaximumInstallationKeys),
              static_cast<std::int64_t>(target::kMaximumInstallationBytes));
    if (const auto* rejected = std::get_if<target::Failure>(&second)) {
      return *rejected;
    }
    return std::monostate{};
  });
}

target::Outcome<std::optional<target::NamespaceReceipt>>
StorageReceiptRecords::find(const target::ReceiptKey& key) {
  return query<std::optional<target::NamespaceReceipt>>(
      sql_,
      [&](pqxx::transaction_base& transaction)
          -> target::Outcome<std::optional<target::NamespaceReceipt>> {
        const auto rows = transaction.exec(
            "SELECT r.subject,r.method,r.resource_kind,r.resource_id,r.idempotency_key,"
            "r.fingerprint,r.result_id,r.created_at,r.retain_until,"
            "n.instance_id,n.installation_id FROM kiarabot.command_receipts r"
            " JOIN kiarabot.namespace_receipt_results n ON n.result_id=r.result_id"
            " WHERE " +
                std::string{kReceiptWhere},
            pqxx::params{key.subject.value, key.method, static_cast<int>(key.resource.kind),
                         key.resource.id.value, key.idempotency_key});
        if (rows.empty()) {
          return std::optional<target::NamespaceReceipt>{};
        }
        const auto row = rows.one_row();
        target::NamespaceReceipt receipt;
        receipt.key = {{row["subject"].as<std::string>()},
                       row["method"].as<std::string>(),
                       {static_cast<target::ResourceKind>(row["resource_kind"].as<int>()),
                        {row["resource_id"].as<std::string>()}},
                       row["idempotency_key"].as<std::string>()};
        receipt.fingerprint = read_digest(row["fingerprint"]);
        receipt.result = read_receipt_result(transaction, row["result_id"].as<std::int64_t>());
        receipt.instance = {row["instance_id"].as<std::string>()};
        receipt.installation = {row["installation_id"].as<std::string>()};
        receipt.created_at = time_field(row, "created_at");
        receipt.retain_until = time_field(row, "retain_until");
        return std::optional<target::NamespaceReceipt>{receipt};
      });
}

target::Outcome<target::NamespaceReceiptWrite>
StorageReceiptRecords::insert(const target::NamespaceReceipt& receipt,
                              target::NamespaceReceiptCapacity capacity) {
  return query<target::NamespaceReceiptWrite>(
      sql_,
      [&](pqxx::transaction_base& transaction) -> target::Outcome<target::NamespaceReceiptWrite> {
        if (!valid_sql_text_fields({receipt.key.subject.value, receipt.key.method,
                                    receipt.key.resource.id.value, receipt.key.idempotency_key,
                                    receipt.instance.value, receipt.installation.value}) ||
            !positive(receipt.created_at) || !positive(receipt.retain_until) ||
            receipt.retain_until < receipt.created_at + target::kMinimumReceiptRetention ||
            receipt.key.method.empty() || receipt.key.idempotency_key.empty()) {
          return invalid();
        }
        const auto existing = transaction.exec(
            "SELECT r.fingerprint,r.result_id,r.created_at,r.retain_until,n.instance_id,"
            "n.installation_id FROM kiarabot.command_receipts r"
            " JOIN kiarabot.namespace_receipt_results n ON n.result_id=r.result_id WHERE " +
                std::string{kReceiptWhere},
            pqxx::params{receipt.key.subject.value, receipt.key.method,
                         static_cast<int>(receipt.key.resource.kind), receipt.key.resource.id.value,
                         receipt.key.idempotency_key});
        if (!existing.empty()) {
          if (read_digest(existing.one_row()["fingerprint"]) != receipt.fingerprint) {
            return conflict();
          }
          auto stored = target::NamespaceReceipt{
              receipt.key,          receipt.fingerprint, receipt.result,      receipt.instance,
              receipt.installation, receipt.created_at,  receipt.retain_until};
          stored.result =
              read_receipt_result(transaction, existing.one_row()["result_id"].as<std::int64_t>());
          stored.created_at = time_field(existing.one_row(), "created_at");
          stored.retain_until = time_field(existing.one_row(), "retain_until");
          return target::NamespaceReceiptWrite{stored, true};
        }
        const auto instance_count =
            transaction
                .exec("SELECT count(*) FROM kiarabot.namespace_receipt_results"
                      " WHERE instance_id=$1",
                      pqxx::params{receipt.instance.value})
                .one_row()[0]
                .as<std::int64_t>();
        const auto installation_count =
            transaction
                .exec("SELECT count(*) FROM kiarabot.namespace_receipt_results"
                      " WHERE installation_id=$1",
                      pqxx::params{receipt.installation.value})
                .one_row()[0]
                .as<std::int64_t>();
        if (instance_count >= static_cast<std::int64_t>(capacity.instance_max) ||
            installation_count >= static_cast<std::int64_t>(capacity.installation_max)) {
          return rate_limited();
        }
        const auto result =
            transaction.exec("INSERT INTO kiarabot.replay_results(kind) VALUES(0) RETURNING id")
                .one_row()[0]
                .as<std::int64_t>();
        const auto* mutation = std::get_if<target::StorageMutationReceipt>(&receipt.result);
        const auto* namespace_receipt =
            std::get_if<target::SharedNamespaceReceipt>(&receipt.result);
        const auto* members = std::get_if<target::SharedMembersReceipt>(&receipt.result);
        if (mutation) {
          const auto shared = std::get_if<target::SharedNamespace>(&mutation->scope);
          transaction.exec(
              "INSERT INTO kiarabot.namespace_receipt_results(result_id,result_kind,instance_id,"
              "installation_id,scope_kind,scope_id,key,deleted,version,expires_at,value,"
              "namespace_id,namespace_installation) "
              "VALUES($1,0,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)",
              pqxx::params{
                  result, receipt.instance.value, receipt.installation.value, shared ? 1 : 0,
                  scope_id(mutation->scope), mutation->key, mutation->deleted,
                  mutation->version ? std::optional<std::uint64_t>{mutation->version->value}
                                    : std::nullopt,
                  mutation->expires_at
                      ? std::optional<std::string>{timestamp(*mutation->expires_at)}
                      : std::nullopt,
                  mutation->value.empty() ? std::optional<pqxx::binarystring>{}
                                          : std::optional<pqxx::binarystring>{pqxx::binarystring{
                                                mutation->value.data(), mutation->value.size()}},
                  shared ? std::optional<std::string>{shared->shared_namespace.value}
                         : std::nullopt,
                  shared ? std::optional<std::string>{shared->installation.value} : std::nullopt});
        } else if (namespace_receipt) {
          const auto& record = namespace_receipt->namespace_record;
          transaction.exec(
              "INSERT INTO kiarabot.namespace_receipt_results(result_id,result_kind,instance_id,"
              "installation_id,namespace_id,namespace_installation,namespace_version,"
              "namespace_disabled,namespace_created_at,namespace_updated_at) VALUES"
              "($1,1,$2,$3,$4,$5,$6,$7,$8,$9)",
              pqxx::params{result, receipt.instance.value, receipt.installation.value,
                           record.namespace_id.value, record.installation.value,
                           record.version.value, record.disabled, timestamp(record.created_at),
                           timestamp(record.updated_at)});
        } else if (members) {
          transaction.exec(
              "INSERT INTO kiarabot.namespace_receipt_results(result_id,result_kind,instance_id,"
              "installation_id,members_version) VALUES($1,2,$2,$3,$4)",
              pqxx::params{result, receipt.instance.value, receipt.installation.value,
                           members->version.value});
        } else {
          return invalid();
        }
        transaction.exec(
            "INSERT INTO kiarabot.command_receipts(subject,method,resource_kind,resource_id,"
            "idempotency_key,fingerprint,result_id,created_at,retain_until) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9)",
            pqxx::params{receipt.key.subject.value, receipt.key.method,
                         static_cast<int>(receipt.key.resource.kind), receipt.key.resource.id.value,
                         receipt.key.idempotency_key,
                         pqxx::binarystring{receipt.fingerprint.bytes.data(),
                                            receipt.fingerprint.bytes.size()},
                         result, timestamp(receipt.created_at), timestamp(receipt.retain_until)});
        auto stored = receipt;
        stored.result = read_receipt_result(transaction, result);
        return target::NamespaceReceiptWrite{std::move(stored), false};
      });
}

target::Outcome<std::size_t> StorageReceiptRecords::prune_expired(target::TimePoint now,
                                                                  std::size_t limit) {
  return query<std::size_t>(
      sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
        if (!positive(now) || limit == 0 || limit > 1000) {
          return invalid();
        }
        const auto rows = transaction.exec(
            "DELETE FROM kiarabot.command_receipts WHERE result_id IN (SELECT r.result_id FROM"
            " kiarabot.command_receipts r JOIN kiarabot.namespace_receipt_results n"
            " ON n.result_id=r.result_id WHERE r.retain_until<=" +
                std::string{kDbNow} + " ORDER BY r.retain_until LIMIT $1) RETURNING result_id",
            pqxx::params{static_cast<int>(limit)});
        for (const auto& row : rows) {
          transaction.exec("DELETE FROM kiarabot.replay_results WHERE id=$1",
                           pqxx::params{row[0].as<std::int64_t>()});
        }
        return static_cast<std::size_t>(rows.affected_rows());
      });
}

} // namespace kiarabot::infrastructure::postgres::detail
