#include "model_store.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "record_codec.hpp"

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
  return {code, target::Acceptance::NotAccepted, recovery, {}, {}};
}
target::Failure conflict() {
  return error(target::ErrorCode::Conflict);
}
target::Failure invalid() {
  return error(target::ErrorCode::InvalidArgument);
}
std::optional<std::string> optional_stamp(const std::optional<target::TimePoint>& value) {
  return value ? std::optional<std::string>(timestamp(*value)) : std::nullopt;
}
std::string blob(const std::vector<std::byte>& value) {
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
std::string blob(const std::array<std::byte, 24>& value) {
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
std::vector<std::byte> bytes_field(const pqxx::field& field) {
  const auto data = field.as<pqxx::bytes>();
  std::vector<std::byte> result(data.size());
  for (std::size_t index = 0; index < data.size(); ++index) {
    result[index] = static_cast<std::byte>(static_cast<unsigned char>(data.data()[index]));
  }
  return result;
}
target::ModelConfiguration read_config(const pqxx::row& row) {
  target::ModelConfiguration config;
  config.model_id = {row["model_id"].as<std::string>()};
  config.alias = row["alias"].as<std::string>();
  const auto kind = target::model_provider_kind(row["provider_kind"].as<std::string>());
  if (!kind) {
    throw std::runtime_error("corrupt stored Model provider kind");
  }
  config.provider_kind = *kind;
  config.endpoint = row["endpoint"].as<std::string>();
  config.provider_model = row["provider_model"].as<std::string>();
  config.timeout_ms = static_cast<std::uint32_t>(row["timeout_ms"].as<int>());
  config.max_concurrency = static_cast<std::uint32_t>(row["max_concurrency"].as<int>());
  config.state = static_cast<target::LifecycleState>(row["lifecycle_state"].as<int>());
  config.version = {row["config_version"].as<std::uint64_t>()};
  config.created_at = time_field(row, "created_at");
  config.observed_at = time_field(row, "observed_at");
  return config;
}
target::ProviderSecretRecord read_secret(const pqxx::row& row) {
  target::ProviderSecretRecord secret;
  secret.model_id = {row["model_id"].as<std::string>()};
  secret.secret_version = {row["secret_version"].as<std::uint64_t>()};
  secret.root_key_version = row["root_key_version"].as<std::uint64_t>();
  secret.aead = static_cast<std::uint8_t>(row["aead"].as<int>());
  const auto nonce = bytes_field(row["nonce"]);
  if (nonce.size() != secret.nonce.size()) {
    throw std::runtime_error("corrupt stored provider secret nonce");
  }
  for (std::size_t index = 0; index < nonce.size(); ++index) {
    secret.nonce[index] = nonce[index];
  }
  secret.sealed_secret = bytes_field(row["sealed_secret"]);
  secret.created_at = time_field(row, "created_at");
  return secret;
}
target::ModelIntent read_call(const pqxx::row& row) {
  target::ModelIntent call;
  call.owner = {row["owner"].as<std::string>()};
  call.logical_call_id = row["logical_call_id"].as<std::string>();
  call.model = {row["model"].as<std::string>()};
  call.config_version = {row["config_version"].as<std::uint64_t>()};
  call.request_hash = read_digest(row["request_hash"]);
  call.state = static_cast<target::ModelCallState>(row["state"].as<int>());
  call.version = {row["version"].as<std::uint64_t>()};
  call.created_at = time_field(row, "created_at");
  call.observed_at = time_field(row, "observed_at");
  if (!row["secret_version"].is_null()) {
    call.secret_version = {row["secret_version"].as<std::uint64_t>()};
  }
  if (!row["input_tokens"].is_null()) {
    call.input_tokens = row["input_tokens"].as<std::uint64_t>();
  }
  if (!row["output_tokens"].is_null()) {
    call.output_tokens = row["output_tokens"].as<std::uint64_t>();
  }
  call.usage_known = !row["usage_known"].is_null() && row["usage_known"].as<bool>();
  call.may_have_billed = !row["may_have_billed"].is_null() && row["may_have_billed"].as<bool>();
  if (!row["provider_request_id"].is_null()) {
    call.provider_request_id = row["provider_request_id"].as<std::string>();
  }
  if (!row["latency_ms"].is_null()) {
    call.latency_ms = static_cast<std::uint32_t>(row["latency_ms"].as<int>());
  }
  if (!row["error"].is_null()) {
    call.error = static_cast<target::ErrorCode>(row["error"].as<int>());
  }
  if (!row["disposition"].is_null()) {
    call.disposition = static_cast<target::ModelCallDisposition>(row["disposition"].as<int>());
  }
  if (!row["resolved_at"].is_null()) {
    call.resolved_at = time_field(row, "resolved_at");
  }
  if (!row["resolved_by"].is_null()) {
    call.resolved_by = row["resolved_by"].as<std::string>();
  }
  return call;
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
    return error(target::ErrorCode::Internal);
  }
  return std::move(*value);
}

class ModelConfigurationRepositoryImpl final : public target::ModelConfigurationRepository {
public:
  explicit ModelConfigurationRepositoryImpl(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::ModelConfiguration>>
  find(const target::ResourceId& model_id) override {
    if (model_id.value.empty() || !valid_sql_text(model_id.value)) {
      return invalid();
    }
    return query<std::optional<target::ModelConfiguration>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::optional<target::ModelConfiguration>> {
          const auto rows =
              transaction.exec("SELECT * FROM kiarabot.model_configs WHERE model_id=$1",
                               pqxx::params{model_id.value});
          if (rows.empty()) {
            return std::optional<target::ModelConfiguration>{};
          }
          return std::optional<target::ModelConfiguration>{read_config(rows.one_row())};
        });
  }
  target::Outcome<std::optional<target::ModelConfiguration>>
  find_by_alias(const std::string& alias) override {
    if (alias.empty() || !valid_sql_text(alias)) {
      return invalid();
    }
    return query<std::optional<target::ModelConfiguration>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::optional<target::ModelConfiguration>> {
          const auto rows = transaction.exec("SELECT * FROM kiarabot.model_configs WHERE alias=$1",
                                             pqxx::params{alias});
          if (rows.empty()) {
            return std::optional<target::ModelConfiguration>{};
          }
          return std::optional<target::ModelConfiguration>{read_config(rows.one_row())};
        });
  }
  target::Outcome<std::vector<target::ModelConfiguration>> list(std::size_t limit,
                                                                const std::string& after) override {
    if (limit == 0 || limit > 100 || !valid_sql_text(after)) {
      return invalid();
    }
    return query<std::vector<target::ModelConfiguration>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::vector<target::ModelConfiguration>> {
          const auto rows = transaction.exec(
              "SELECT * FROM kiarabot.model_configs WHERE model_id>$2 ORDER BY model_id LIMIT $1",
              pqxx::params{static_cast<int>(limit), after});
          std::vector<target::ModelConfiguration> result;
          result.reserve(rows.size());
          for (const auto& row : rows) {
            result.push_back(read_config(row));
          }
          return result;
        });
  }
  target::Status save(const target::ModelConfiguration& value,
                      std::optional<target::ResourceVersion> expected) override {
    if (value.model_id.value.empty() || value.version.value == 0 ||
        !valid_sql_text_fields(
            {value.model_id.value, value.alias, value.endpoint, value.provider_model}) ||
        !target::valid_model_alias(value.alias) || !target::valid_model_endpoint(value.endpoint) ||
        !target::valid_provider_model(value.provider_model) ||
        !target::valid_model_timeout(value.timeout_ms) ||
        !target::valid_model_concurrency(value.max_concurrency)) {
      return invalid();
    }
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      pqxx::params parameters{value.model_id.value,
                              value.alias,
                              std::string{target::model_provider_kind_name(value.provider_kind)},
                              value.endpoint,
                              value.provider_model,
                              static_cast<int>(value.timeout_ms),
                              static_cast<int>(value.max_concurrency),
                              static_cast<int>(value.state),
                              static_cast<std::int64_t>(value.version.value),
                              timestamp(value.created_at),
                              timestamp(value.observed_at)};
      if (!expected) {
        const auto inserted = transaction.exec(
            "INSERT INTO kiarabot.model_configs(model_id,alias,provider_kind,endpoint,"
            "provider_model,timeout_ms,max_concurrency,lifecycle_state,config_version,"
            "created_at,observed_at) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) "
            "ON CONFLICT DO NOTHING RETURNING model_id",
            parameters);
        return inserted.empty() ? conflict() : target::Status{std::monostate{}};
      }
      parameters.append(static_cast<std::int64_t>(expected->value));
      const auto updated = transaction.exec(
          "UPDATE kiarabot.model_configs SET alias=$2,provider_kind=$3,endpoint=$4,"
          "provider_model=$5,timeout_ms=$6,max_concurrency=$7,lifecycle_state=$8,"
          "config_version=$9,created_at=$10,observed_at=$11 "
          "WHERE model_id=$1 AND config_version=$12 RETURNING model_id",
          parameters);
      return updated.empty() ? conflict() : target::Status{std::monostate{}};
    });
  }

private:
  SqlWork& sql_;
};

class ProviderSecretRepositoryImpl final : public target::ProviderSecretRepository {
public:
  explicit ProviderSecretRepositoryImpl(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::ProviderSecretRecord>>
  current(const target::ResourceId& model_id) override {
    if (model_id.value.empty() || !valid_sql_text(model_id.value)) {
      return invalid();
    }
    return query<std::optional<target::ProviderSecretRecord>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::optional<target::ProviderSecretRecord>> {
          const auto rows = transaction.exec(
              "SELECT * FROM kiarabot.encrypted_provider_secrets WHERE model_id=$1 "
              "ORDER BY secret_version DESC LIMIT 1",
              pqxx::params{model_id.value});
          if (rows.empty()) {
            return std::optional<target::ProviderSecretRecord>{};
          }
          return std::optional<target::ProviderSecretRecord>{read_secret(rows.one_row())};
        });
  }
  target::Outcome<std::optional<target::ProviderSecretRecord>>
  find(const target::ResourceId& model_id, const target::ResourceVersion& version) override {
    if (model_id.value.empty() || !valid_sql_text(model_id.value) || version.value == 0) {
      return invalid();
    }
    return query<std::optional<target::ProviderSecretRecord>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::optional<target::ProviderSecretRecord>> {
          const auto rows = transaction.exec(
              "SELECT * FROM kiarabot.encrypted_provider_secrets "
              "WHERE model_id=$1 AND secret_version=$2",
              pqxx::params{model_id.value, static_cast<std::int64_t>(version.value)});
          if (rows.empty()) {
            return std::optional<target::ProviderSecretRecord>{};
          }
          return std::optional<target::ProviderSecretRecord>{read_secret(rows.one_row())};
        });
  }
  target::Status insert(const target::ProviderSecretRecord& value) override {
    if (value.model_id.value.empty() || !valid_sql_text(value.model_id.value) ||
        value.secret_version.value == 0 || value.root_key_version == 0 || value.aead != 1 ||
        value.sealed_secret.empty() || value.sealed_secret.size() > 8192) {
      return invalid();
    }
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      const auto inserted = transaction.exec(
          "INSERT INTO kiarabot.encrypted_provider_secrets(model_id,secret_version,"
          "root_key_version,aead,nonce,sealed_secret,created_at) "
          "VALUES($1,$2,$3,$4,$5,$6,$7) ON CONFLICT DO NOTHING RETURNING model_id",
          pqxx::params{value.model_id.value, static_cast<std::int64_t>(value.secret_version.value),
                       static_cast<std::int64_t>(value.root_key_version),
                       static_cast<int>(value.aead), pqxx::binarystring{blob(value.nonce)},
                       pqxx::binarystring{blob(value.sealed_secret)}, timestamp(value.created_at)});
      return inserted.empty() ? conflict() : target::Status{std::monostate{}};
    });
  }

private:
  SqlWork& sql_;
};

class ModelCallRepositoryImpl final : public target::ModelCallRepository {
public:
  explicit ModelCallRepositoryImpl(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::ModelIntent>>
  find(const target::ModelCallKey& key) override {
    if (key.owner.value.empty() || key.logical_call_id.empty() ||
        !valid_sql_text_fields({key.owner.value, key.logical_call_id})) {
      return invalid();
    }
    return query<std::optional<target::ModelIntent>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::optional<target::ModelIntent>> {
          const auto rows = transaction.exec(
              "SELECT * FROM kiarabot.model_intents WHERE owner=$1 AND logical_call_id=$2",
              pqxx::params{key.owner.value, key.logical_call_id});
          if (rows.empty()) {
            return std::optional<target::ModelIntent>{};
          }
          return std::optional<target::ModelIntent>{read_call(rows.one_row())};
        });
  }
  target::Outcome<std::optional<target::ModelIntent>>
  find_by_call_id(const std::string& logical_call_id) override {
    if (logical_call_id.empty() || !valid_sql_text(logical_call_id)) {
      return invalid();
    }
    return query<std::optional<target::ModelIntent>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::optional<target::ModelIntent>> {
          const auto rows = transaction.exec(
              "SELECT * FROM kiarabot.model_intents WHERE logical_call_id=$1 LIMIT 2",
              pqxx::params{logical_call_id});
          if (rows.empty()) {
            return std::optional<target::ModelIntent>{};
          }
          if (rows.size() > 1) {
            return conflict();
          }
          return std::optional<target::ModelIntent>{read_call(rows.one_row())};
        });
  }
  target::Outcome<std::vector<target::ModelIntent>>
  list_unresolved_unknown(target::TimePoint now, std::size_t limit) override {
    if (limit == 0 || limit > 100) {
      return invalid();
    }
    return query<std::vector<target::ModelIntent>>(
        sql_,
        [&](pqxx::transaction_base& transaction)
            -> target::Outcome<std::vector<target::ModelIntent>> {
          const auto rows =
              transaction.exec("SELECT * FROM kiarabot.model_intents WHERE state=3 "
                               "AND (disposition IS NULL OR disposition=0) AND created_at<=$1 "
                               "ORDER BY created_at LIMIT $2",
                               pqxx::params{timestamp(now), static_cast<int>(limit)});
          std::vector<target::ModelIntent> result;
          result.reserve(rows.size());
          for (const auto& row : rows) {
            result.push_back(read_call(row));
          }
          return result;
        });
  }
  target::Status insert(const target::ModelIntent& value) override {
    if (!valid_call(value)) {
      return invalid();
    }
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      const auto inserted = transaction.exec(kInsertCall, insert_params(value));
      return inserted.empty() ? conflict() : target::Status{std::monostate{}};
    });
  }
  target::Status save(const target::ModelIntent& value,
                      std::optional<target::ResourceVersion> expected) override {
    if (!valid_call(value) || !expected || expected->value == 0) {
      return invalid();
    }
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      auto parameters = facts(value);
      parameters.append(value.owner.value);
      parameters.append(value.logical_call_id);
      parameters.append(static_cast<std::int64_t>(expected->value));
      const auto updated = transaction.exec(kUpdateCall, parameters);
      return updated.empty() ? conflict() : target::Status{std::monostate{}};
    });
  }
  target::Outcome<std::size_t> prune_terminal(target::TimePoint now, std::size_t limit) override {
    if (limit == 0 || limit > 100) {
      return invalid();
    }
    return query<std::size_t>(
        sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
          const auto rows = transaction.exec(
              "DELETE FROM kiarabot.model_intents WHERE ctid IN ("
              "SELECT ctid FROM kiarabot.model_intents WHERE "
              "(state IN (1,2) OR (state=3 AND disposition IS NOT NULL AND disposition<>0)) "
              "AND observed_at<=$1 LIMIT $2) RETURNING owner",
              pqxx::params{timestamp(now), static_cast<int>(limit)});
          return static_cast<std::size_t>(rows.size());
        });
  }

private:
  static bool valid_call(const target::ModelIntent& value) {
    return !value.owner.value.empty() && !value.logical_call_id.empty() &&
           !value.model.value.empty() && value.config_version.value > 0 &&
           value.version.value > 0 && value.state >= target::ModelCallState::Started &&
           value.state <= target::ModelCallState::Unknown &&
           value.disposition >= target::ModelCallDisposition::Unspecified &&
           value.disposition <= target::ModelCallDisposition::Abandoned &&
           valid_sql_text_fields({value.owner.value, value.logical_call_id, value.model.value}) &&
           valid_sql_text(value.provider_request_id) && valid_sql_text(value.resolved_by);
  }
  static pqxx::params facts(const target::ModelIntent& value) {
    return pqxx::params{value.model.value,
                        static_cast<std::int64_t>(value.config_version.value),
                        digest(value.request_hash),
                        static_cast<int>(value.state),
                        static_cast<std::int64_t>(value.version.value),
                        timestamp(value.created_at),
                        timestamp(value.observed_at),
                        static_cast<std::int64_t>(value.secret_version.value),
                        static_cast<std::int64_t>(value.input_tokens),
                        static_cast<std::int64_t>(value.output_tokens),
                        value.usage_known,
                        value.may_have_billed,
                        value.provider_request_id,
                        static_cast<int>(value.latency_ms),
                        value.error ? std::optional<int>{static_cast<int>(*value.error)}
                                    : std::optional<int>{},
                        static_cast<int>(value.disposition),
                        optional_stamp(value.resolved_at),
                        value.resolved_by,
                        timestamp(target::model_retention_until(value.observed_at))};
  }
  static pqxx::params insert_params(const target::ModelIntent& value) {
    return pqxx::params{value.owner.value,
                        value.logical_call_id,
                        value.model.value,
                        static_cast<std::int64_t>(value.config_version.value),
                        digest(value.request_hash),
                        static_cast<int>(value.state),
                        static_cast<std::int64_t>(value.version.value),
                        timestamp(value.created_at),
                        timestamp(value.observed_at),
                        static_cast<std::int64_t>(value.secret_version.value),
                        static_cast<std::int64_t>(value.input_tokens),
                        static_cast<std::int64_t>(value.output_tokens),
                        value.usage_known,
                        value.may_have_billed,
                        value.provider_request_id,
                        static_cast<int>(value.latency_ms),
                        value.error ? std::optional<int>{static_cast<int>(*value.error)}
                                    : std::optional<int>{},
                        static_cast<int>(value.disposition),
                        optional_stamp(value.resolved_at),
                        value.resolved_by,
                        timestamp(target::model_retention_until(value.observed_at))};
  }
  static constexpr std::string_view kInsertCall =
      "INSERT INTO kiarabot.model_intents(owner,logical_call_id,model,config_version,request_hash,"
      "state,version,created_at,observed_at,secret_version,input_tokens,output_tokens,usage_known,"
      "may_have_billed,provider_request_id,latency_ms,error,disposition,resolved_at,resolved_by,"
      "retain_until) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,"
      "$20,$21) ON CONFLICT DO NOTHING RETURNING owner";
  static constexpr std::string_view kUpdateCall =
      "UPDATE kiarabot.model_intents SET model=$1,config_version=$2,request_hash=$3,state=$4,"
      "version=$5,created_at=$6,observed_at=$7,secret_version=$8,input_tokens=$9,"
      "output_tokens=$10,usage_known=$11,may_have_billed=$12,provider_request_id=$13,"
      "latency_ms=$14,error=$15,disposition=$16,resolved_at=$17,resolved_by=$18,retain_until=$19 "
      "WHERE owner=$20 AND logical_call_id=$21 AND version=$22 RETURNING owner";

  SqlWork& sql_;
};

} // namespace

std::unique_ptr<target::ModelConfigurationRepository> model_configuration_repository(SqlWork& sql) {
  return std::make_unique<ModelConfigurationRepositoryImpl>(sql);
}
std::unique_ptr<target::ProviderSecretRepository> provider_secret_repository(SqlWork& sql) {
  return std::make_unique<ProviderSecretRepositoryImpl>(sql);
}
std::unique_ptr<target::ModelCallRepository> model_call_repository(SqlWork& sql) {
  return std::make_unique<ModelCallRepositoryImpl>(sql);
}

} // namespace kiarabot::infrastructure::postgres::detail
