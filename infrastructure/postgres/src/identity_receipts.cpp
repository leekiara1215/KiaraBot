#include <type_traits>
#include <utility>

#include "identity_access.hpp"
#include "identity_security.hpp"
#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
#include "session.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;
using identity::conflict;
using identity::invalid;
using identity::query;
using identity::require_stored;
using identity::valid_id;
inline constexpr std::string_view kDbNow =
    "(extract(epoch FROM clock_timestamp())*1000000000)::bigint";
inline constexpr std::string_view kScopeWhere =
    "scope_kind=$1 AND scope_actor=$2 AND scope_target=$3 AND idempotency_key=$4";

struct ScopeKey {
  int kind;
  std::string actor;
  std::string target;
  std::string key;
};
std::optional<ScopeKey> scope_key(const target::SecretReceiptScope& scope) {
  if (scope.valueless_by_exception()) {
    return {};
  }
  ScopeKey key = std::visit(
      [](const auto& value) -> ScopeKey {
        using Scope = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Scope, target::LoginReceiptScope>) {
          return {0, value.control_peer.value, value.normalized_login, value.idempotency_key};
        } else if constexpr (std::is_same_v<Scope, target::RefreshReceiptScope>) {
          return {1, value.control_peer.value, value.session.value, value.idempotency_key};
        } else {
          return {2, value.actor.value, value.credential.value, value.idempotency_key};
        }
      },
      scope);
  if (!valid_id(key.actor) || !valid_id(key.target) || !valid_id(key.key)) {
    return {};
  }
  return key;
}
pqxx::params scope_params(const ScopeKey& key) {
  return {key.kind, key.actor, key.target, key.key};
}
bool valid_binding(const target::SecretReceiptBinding& binding) {
  if (!scope_key(binding.scope) || binding.parameters.valueless_by_exception() ||
      !std::holds_alternative<std::monostate>(
          target::validate_secret_request_parameters(binding.scope, binding.parameters)) ||
      !valid_id(binding.subject.value) || !valid_id(binding.credential.id.value) ||
      binding.credential.version.value == 0 || !valid_id(binding.request.key.value) ||
      (binding.session && !valid_id(binding.session->value)) ||
      binding.retain_until <= binding.created_at) {
    return false;
  }
  // Unsigned subtraction is exact for the ordered signed64 endpoints, including opposite signs.
  const auto interval =
      static_cast<std::uint64_t>(binding.retain_until.time_since_epoch().count()) -
      static_cast<std::uint64_t>(binding.created_at.time_since_epoch().count());
  if (interval > 86400000000000ULL) {
    return false;
  }
  if (const auto* refresh = std::get_if<target::RefreshReceiptScope>(&binding.scope)) {
    return binding.session && *binding.session == refresh->session &&
           binding.result_rotation >
               std::get<target::RefreshRequestParameters>(binding.parameters).input_rotation;
  }
  if (const auto* delivery = std::get_if<target::CredentialDeliveryScope>(&binding.scope)) {
    return !binding.session && binding.credential.id == delivery->credential &&
           binding.result_rotation == 0;
  }
  return binding.session.has_value() && binding.result_rotation > 0;
}
std::vector<std::byte> binary(const pqxx::field& field) {
  const auto input = field.as<pqxx::bytes>();
  std::vector<std::byte> output;
  output.reserve(input.size());
  for (const auto value : input) {
    output.push_back(static_cast<std::byte>(value));
  }
  return output;
}
target::EncryptedSecretReceipt read_receipt(const pqxx::row& row) {
  const ScopeKey key{row["scope_kind"].as<int>(), row["scope_actor"].as<std::string>(),
                     row["scope_target"].as<std::string>(),
                     row["idempotency_key"].as<std::string>()};
  target::SecretReceiptScope scope;
  target::SecretRequestParameters parameters;
  if (key.kind == 0) {
    scope = target::LoginReceiptScope{{key.actor}, key.target, key.key};
    parameters = target::LoginRequestParameters{};
    require_stored(row["request_parameter"].as<std::uint64_t>() == 0);
  } else if (key.kind == 1) {
    scope = target::RefreshReceiptScope{{key.actor}, {key.target}, key.key};
    parameters = target::RefreshRequestParameters{row["request_parameter"].as<std::uint64_t>()};
  } else {
    require_stored(key.kind == 2);
    scope = target::CredentialDeliveryScope{{key.actor}, {key.target}, key.key};
    parameters =
        target::CredentialDeliveryParameters{{row["request_parameter"].as<std::uint64_t>()}};
  }
  std::optional<target::SessionId> session;
  if (!row["session"].is_null()) {
    session = target::SessionId{row["session"].as<std::string>()};
  }
  target::EncryptedSecretReceipt result{
      {std::move(scope),
       {row["subject"].as<std::string>()},
       session,
       {{row["credential_id"].as<std::string>()}, {row["credential_version"].as<std::uint64_t>()}},
       std::move(parameters),
       row["result_rotation"].as<std::uint64_t>(),
       {{row["request_key"].as<std::string>()}, read_digest(row["request_tag"])},
       time_field(row, "created_at"),
       time_field(row, "retain_until")},
      {{row["sealed_key"].as<std::string>()},
       row["format_version"].as<std::uint32_t>(),
       binary(row["nonce"]),
       binary(row["ciphertext"])}};
  require_stored(valid_binding(result.binding) && valid_id(result.sealed.key.value) &&
                 result.sealed.format_version > 0 && !result.sealed.nonce.empty() &&
                 !result.sealed.ciphertext.empty());
  return result;
}
std::optional<target::EncryptedSecretReceipt> find_receipt(pqxx::transaction_base& transaction,
                                                           const ScopeKey& key) {
  const auto rows =
      transaction.exec("SELECT * FROM kiarabot.secret_receipts WHERE " + std::string(kScopeWhere) +
                           " AND retain_until>" + std::string(kDbNow),
                       scope_params(key));
  if (rows.empty()) {
    return {};
  }
  return read_receipt(rows.one_row());
}
class SecretReceipts final : public target::IdentitySecretReceiptRepository {
public:
  explicit SecretReceipts(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::EncryptedSecretReceipt>>
  find(const target::SecretReceiptScope& scope) override {
    return query<std::optional<target::EncryptedSecretReceipt>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::EncryptedSecretReceipt>> {
          const auto key = scope_key(scope);
          if (!key) {
            return invalid();
          }
          return find_receipt(transaction, *key);
        });
  }
  target::Outcome<target::SecretReceiptWrite>
  insert(const target::EncryptedSecretReceipt& receipt,
         target::SecretReceiptCapacity capacity) override {
    return query<target::SecretReceiptWrite>(
        sql_, [&](auto& transaction) -> target::Outcome<target::SecretReceiptWrite> {
          const auto& binding = receipt.binding;
          const auto key = scope_key(binding.scope);
          if (!key || capacity.max_records == 0 || !valid_id(binding.request.key.value) ||
              binding.parameters.valueless_by_exception() ||
              !std::holds_alternative<std::monostate>(
                  target::validate_secret_request_parameters(binding.scope, binding.parameters))) {
            return invalid();
          }
          transaction.exec("SELECT pg_advisory_xact_lock($1,105)", pqxx::params{kWriterLockClass});
          auto existing = find_receipt(transaction, *key);
          if (existing) {
            if (existing->binding.request != binding.request ||
                existing->binding.parameters != binding.parameters) {
              return conflict();
            }
            return target::SecretReceiptWrite{std::move(*existing), true};
          }
          if (!valid_binding(binding) || !valid_id(receipt.sealed.key.value) ||
              receipt.sealed.format_version == 0 || receipt.sealed.nonce.empty() ||
              receipt.sealed.ciphertext.empty()) {
            return invalid();
          }
          // Only the same expired scope is removed. All remaining retained rows consume capacity,
          // including expired rows awaiting explicit pruning, so history cannot grow without bound.
          transaction.exec("DELETE FROM kiarabot.secret_receipts WHERE " +
                               std::string(kScopeWhere) +
                               " AND retain_until<=" + std::string(kDbNow),
                           scope_params(*key));
          const auto count = transaction.exec("SELECT count(*) FROM kiarabot.secret_receipts")
                                 .one_row()[0]
                                 .template as<std::uint64_t>();
          if (count >= capacity.max_records) {
            return target::rejected(target::ErrorCode::RateLimited, target::Recovery::Backoff);
          }
          std::uint64_t parameter = 0;
          if (const auto* refresh =
                  std::get_if<target::RefreshRequestParameters>(&binding.parameters)) {
            parameter = refresh->input_rotation;
          } else if (const auto* delivery =
                         std::get_if<target::CredentialDeliveryParameters>(&binding.parameters)) {
            parameter = delivery->expected_version.value;
          }
          // The complete parameter list reserves its final size before mixing inline scalar and
          // binary append paths in the fixed pqxx library. No incremental vector reallocation.
          const pqxx::params values{
              key->kind,
              key->actor,
              key->target,
              key->key,
              binding.subject.value,
              binding.session ? std::optional<std::string>(binding.session->value) : std::nullopt,
              binding.credential.id.value,
              binding.credential.version.value,
              parameter,
              binding.result_rotation,
              binding.request.key.value,
              digest(binding.request.tag),
              timestamp(binding.created_at),
              timestamp(binding.retain_until),
              receipt.sealed.key.value,
              receipt.sealed.format_version,
              pqxx::bytes_view(receipt.sealed.nonce.data(), receipt.sealed.nonce.size()),
              pqxx::bytes_view(receipt.sealed.ciphertext.data(), receipt.sealed.ciphertext.size())};
          const auto rows = transaction.exec(
              "INSERT INTO kiarabot.secret_receipts SELECT "
              "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12::bytea,$13,$14,$15,$16,$17,$18 "
              "WHERE $13::bigint<=" +
                  std::string(kDbNow) + " AND $14::bigint>" + std::string(kDbNow) +
                  " RETURNING scope_kind",
              values);
          if (rows.empty()) {
            return invalid();
          }
          return target::SecretReceiptWrite{receipt, false};
        });
  }
  target::Outcome<std::size_t> prune_expired(target::TimePoint, std::size_t limit) override {
    return query<std::size_t>(sql_, [&](auto& transaction) -> target::Outcome<std::size_t> {
      if (limit == 0 || limit > 1000) {
        return invalid();
      }
      const auto rows = transaction.exec(
          "DELETE FROM kiarabot.secret_receipts WHERE ctid IN "
          "(SELECT ctid FROM kiarabot.secret_receipts WHERE retain_until<=" +
              std::string(kDbNow) +
              " ORDER BY retain_until LIMIT $1 FOR UPDATE SKIP LOCKED) RETURNING scope_kind",
          pqxx::params{limit});
      return static_cast<std::size_t>(rows.size());
    });
  }

private:
  SqlWork& sql_;
};
} // namespace
std::unique_ptr<target::IdentitySecretReceiptRepository> secret_receipt_repository(SqlWork& sql) {
  return std::make_unique<SecretReceipts>(sql);
}
} // namespace kiarabot::infrastructure::postgres::detail
