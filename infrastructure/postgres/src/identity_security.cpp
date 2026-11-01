#include "identity_security.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include "identity_access.hpp"
#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
#include "record_codec.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;

using identity::conflict;
using identity::invalid;
using identity::query;
using identity::require_stored;
using identity::valid_id;
using identity::valid_next_version;

bool text_valid(const target::CredentialVersionRecord& record) {
  if (!valid_sql_text_fields(
          {record.ref.id.value, record.subject.value, record.binding.id.value}) ||
      record.material.valueless_by_exception()) {
    return false;
  }
  if (const auto* password = std::get_if<target::PasswordVerifier>(&record.material)) {
    return valid_sql_text(password->encoded);
  }
  if (const auto* bootstrap = std::get_if<target::WorkloadBootstrapVerifier>(&record.material)) {
    return valid_sql_text_fields({bootstrap->verifier.key.value, bootstrap->binding.id.value});
  }
  if (const auto* certificate = std::get_if<target::CertificateBinding>(&record.material)) {
    return valid_sql_text_fields({certificate->subject.value, certificate->registered_san});
  }
  return true;
}
std::optional<target::CredentialVersionRecord>
load_credential(pqxx::transaction_base& transaction, const target::CredentialVersionRef& ref) {
  const auto rows = transaction.exec("SELECT * FROM kiarabot.credential_versions WHERE "
                                     "id=$1 AND version=$2",
                                     pqxx::params{ref.id.value, ref.version.value});
  if (rows.empty()) {
    return {};
  }
  const auto row = rows.one_row();
  target::CredentialVersionRecord record{
      ref,
      {row["subject"].as<std::string>()},
      target::PendingCredentialMaterial{},
      static_cast<target::CredentialState>(row["state"].as<int>()),
      time_field(row, "created_at"),
      optional_time(row, "revoked_at"),
      {static_cast<target::ResourceKind>(row["binding_kind"].as<int>()),
       {row["binding_id"].as<std::string>()}},
      optional_time(row, "expires_at")};
  const pqxx::params key{ref.id.value, ref.version.value};
  switch (row["material"].as<int>()) {
  case 0: {
    const auto material = transaction
                              .exec("SELECT kind FROM kiarabot.credential_pending WHERE "
                                    "id=$1 AND version=$2",
                                    key)
                              .one_row();
    record.material = target::PendingCredentialMaterial{
        static_cast<target::CredentialKind>(material["kind"].as<int>())};
    break;
  }
  case 1: {
    const auto material = transaction
                              .exec("SELECT encoded,hash_policy_version FROM "
                                    "kiarabot.credential_passwords WHERE id=$1 AND version=$2",
                                    key)
                              .one_row();
    record.material =
        target::PasswordVerifier{material["encoded"].as<std::string>(),
                                 {material["hash_policy_version"].as<std::uint64_t>()}};
    break;
  }
  case 2: {
    const auto material = transaction
                              .exec("SELECT * FROM kiarabot.credential_bootstraps WHERE "
                                    "id=$1 AND version=$2",
                                    key)
                              .one_row();
    std::vector<target::Service> audiences;
    for (const auto& audience :
         transaction.exec("SELECT audience FROM kiarabot.credential_audiences "
                          "WHERE id=$1 AND version=$2 ORDER BY position",
                          key)) {
      audiences.push_back(static_cast<target::Service>(audience[0].as<int>()));
    }
    record.material = target::WorkloadBootstrapVerifier{
        {target::TokenPurpose::WorkloadBootstrap,
         {material["verifier_key"].as<std::string>()},
         read_digest(material["verifier_tag"])},
        {static_cast<target::ResourceKind>(material["binding_kind"].as<int>()),
         {material["binding_id"].as<std::string>()}},
        std::move(audiences),
        time_field(material, "expires_at")};
    break;
  }
  case 3: {
    const auto material = transaction
                              .exec("SELECT * FROM kiarabot.credential_certificates WHERE "
                                    "id=$1 AND version=$2",
                                    key)
                              .one_row();
    std::optional<target::Service> platform;
    if (!material["platform_service"].is_null()) {
      platform = static_cast<target::Service>(material["platform_service"].as<int>());
    }
    record.material = target::CertificateBinding{
        {material["subject"].as<std::string>()},      read_digest(material["fingerprint"]),
        material["registered_san"].as<std::string>(), platform,
        time_field(material, "not_before"),           time_field(material, "not_after")};
    break;
  }
  default:
    require_stored(false);
  }
  require_stored(text_valid(record) && std::holds_alternative<std::monostate>(
                                           target::validate_credential_record(record)));
  return record;
}
void insert_material(pqxx::transaction_base& transaction,
                     const target::CredentialVersionRecord& record) {
  const auto& ref = record.ref;
  if (const auto* pending = std::get_if<target::PendingCredentialMaterial>(&record.material)) {
    transaction.exec(
        "INSERT INTO kiarabot.credential_pending VALUES($1,$2,0,$3)",
        pqxx::params{ref.id.value, ref.version.value, static_cast<int>(pending->kind)});
  } else if (const auto* password = std::get_if<target::PasswordVerifier>(&record.material)) {
    transaction.exec("INSERT INTO kiarabot.credential_passwords VALUES($1,$2,1,$3,$4)",
                     pqxx::params{ref.id.value, ref.version.value, password->encoded,
                                  password->hash_policy_version.value});
  } else if (const auto* bootstrap =
                 std::get_if<target::WorkloadBootstrapVerifier>(&record.material)) {
    transaction.exec("INSERT INTO kiarabot.credential_bootstraps VALUES($1,$2,2,$3,$4::bytea,"
                     "$5,$6,$7)",
                     pqxx::params{ref.id.value, ref.version.value, bootstrap->verifier.key.value,
                                  digest(bootstrap->verifier.tag),
                                  static_cast<int>(bootstrap->binding.kind),
                                  bootstrap->binding.id.value, timestamp(bootstrap->expires_at)});
    std::size_t position = 0;
    for (const auto audience : bootstrap->audiences) {
      transaction.exec(
          "INSERT INTO kiarabot.credential_audiences VALUES($1,$2,$3,$4)",
          pqxx::params{ref.id.value, ref.version.value, position++, static_cast<int>(audience)});
    }
  } else {
    const auto& certificate = std::get<target::CertificateBinding>(record.material);
    const auto platform = certificate.platform_service
                              ? std::optional<int>(static_cast<int>(*certificate.platform_service))
                              : std::nullopt;
    transaction.exec("INSERT INTO kiarabot.credential_certificates VALUES($1,$2,3,$3,$4::bytea,"
                     "$5,$6,$7,$8)",
                     pqxx::params{ref.id.value, ref.version.value, certificate.subject.value,
                                  digest(certificate.fingerprint), certificate.registered_san,
                                  platform, timestamp(certificate.not_before),
                                  timestamp(certificate.not_after)});
  }
}
class Credentials final : public target::IdentityCredentialRepository {
public:
  explicit Credentials(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::CredentialVersionRecord>>
  current(const target::CredentialId& id) override {
    return query<std::optional<target::CredentialVersionRecord>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::CredentialVersionRecord>> {
          if (!valid_id(id.value)) {
            return invalid();
          }
          const auto heads = transaction.exec(
              "SELECT version FROM kiarabot.credential_heads WHERE id=$1", pqxx::params{id.value});
          if (heads.empty()) {
            return std::optional<target::CredentialVersionRecord>{};
          }
          auto record =
              load_credential(transaction, {id, {heads.one_row()[0].template as<std::uint64_t>()}});
          require_stored(record.has_value());
          return record;
        });
  }
  target::Outcome<std::optional<target::CredentialVersionRecord>>
  version(const target::CredentialVersionRef& ref) override {
    return query<std::optional<target::CredentialVersionRecord>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::CredentialVersionRecord>> {
          if (!valid_id(ref.id.value) || ref.version.value == 0) {
            return invalid();
          }
          return load_credential(transaction, ref);
        });
  }
  target::Outcome<std::optional<target::CredentialVersionRecord>>
  by_certificate(const target::Sha256Digest& fingerprint) override {
    return query<std::optional<target::CredentialVersionRecord>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::CredentialVersionRecord>> {
          const auto heads =
              transaction.exec("SELECT id,version FROM kiarabot.credential_heads WHERE "
                               "leaf_fingerprint=$1::bytea",
                               pqxx::params{digest(fingerprint)});
          if (heads.empty()) {
            return std::optional<target::CredentialVersionRecord>{};
          }
          const auto head = heads.one_row();
          auto record =
              load_credential(transaction, {{head["id"].template as<std::string>()},
                                            {head["version"].template as<std::uint64_t>()}});
          require_stored(record.has_value());
          const auto* certificate = std::get_if<target::CertificateBinding>(&record->material);
          require_stored(certificate && certificate->fingerprint == fingerprint);
          return record;
        });
  }
  target::Outcome<std::optional<target::CredentialVersionRecord>>
  by_bootstrap(const target::TokenVerifier& verifier) override {
    return query<std::optional<target::CredentialVersionRecord>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::CredentialVersionRecord>> {
          if (verifier.purpose != target::TokenPurpose::WorkloadBootstrap ||
              !valid_id(verifier.key.value)) {
            return invalid();
          }
          const auto heads =
              transaction.exec("SELECT id,version,material FROM kiarabot.credential_heads WHERE "
                               "bootstrap_key=$1 AND bootstrap_tag=$2::bytea",
                               pqxx::params{verifier.key.value, digest(verifier.tag)});
          if (heads.empty()) {
            return std::optional<target::CredentialVersionRecord>{};
          }
          const auto head = heads.one_row();
          auto record =
              load_credential(transaction, {{head["id"].template as<std::string>()},
                                            {head["version"].template as<std::uint64_t>()}});
          require_stored(record.has_value());
          require_stored(head["material"].template as<int>() == 2);
          const auto* bootstrap = std::get_if<target::WorkloadBootstrapVerifier>(&record->material);
          require_stored(bootstrap && bootstrap->verifier.key == verifier.key &&
                         bootstrap->verifier.tag == verifier.tag);
          return record;
        });
  }
  target::Outcome<std::optional<target::CredentialVersionRecord>>
  by_platform(const target::PlatformPeer& peer) override {
    return query<std::optional<target::CredentialVersionRecord>>(
        sql_,
        [&](auto& transaction) -> target::Outcome<std::optional<target::CredentialVersionRecord>> {
          if (!valid_id(peer.subject.value) || !target::valid_service(peer.service)) {
            return invalid();
          }
          const auto heads = transaction.exec(
              "SELECT h.id,h.version,h.material,h.leaf_fingerprint FROM kiarabot.credential_heads "
              "h "
              "LEFT JOIN kiarabot.credential_certificates c ON c.id=h.id AND c.version=h.version "
              "WHERE h.subject=$1 AND (c.platform_service=$2 OR (h.material=3 AND c.id IS NULL))",
              pqxx::params{peer.subject.value, static_cast<int>(peer.service)});
          if (heads.empty()) {
            return std::optional<target::CredentialVersionRecord>{};
          }
          require_stored(heads.size() == 1);
          const auto head = heads.one_row();
          auto record =
              load_credential(transaction, {{head["id"].template as<std::string>()},
                                            {head["version"].template as<std::uint64_t>()}});
          require_stored(record.has_value() && head["material"].template as<int>() == 3);
          const auto* certificate = std::get_if<target::CertificateBinding>(&record->material);
          require_stored(certificate && record->subject == peer.subject &&
                         certificate->subject == peer.subject &&
                         certificate->platform_service == peer.service &&
                         certificate->fingerprint == read_digest(head["leaf_fingerprint"]));
          return record;
        });
  }
  target::Outcome<std::optional<target::LoginIdentity>>
  login(const std::string& normalized_login) override {
    return query<std::optional<target::LoginIdentity>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::optional<target::LoginIdentity>> {
          if (!valid_id(normalized_login)) {
            return invalid();
          }
          const auto rows = transaction.exec("SELECT * FROM kiarabot.login_identities WHERE "
                                             "normalized_login=$1",
                                             pqxx::params{normalized_login});
          if (rows.empty()) {
            return std::optional<target::LoginIdentity>{};
          }
          const auto row = rows.one_row();
          target::LoginIdentity result{row["normalized_login"].template as<std::string>(),
                                       {row["subject"].template as<std::string>()},
                                       {row["password_credential"].template as<std::string>()},
                                       {row["version"].template as<std::uint64_t>()}};
          require_stored(valid_id(result.normalized_login) && valid_id(result.subject.value) &&
                         valid_id(result.password_credential.value) && result.version.value > 0);
          return std::optional<target::LoginIdentity>(std::move(result));
        });
  }
  target::Status save_login(const target::LoginIdentity& login,
                            std::optional<target::ResourceVersion> expected) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      if (!valid_id(login.normalized_login) || !valid_id(login.subject.value) ||
          !valid_id(login.password_credential.value) ||
          !valid_next_version(login.version, expected)) {
        return invalid();
      }
      pqxx::params parameters{login.normalized_login, login.subject.value,
                              login.password_credential.value, login.version.value};
      pqxx::result rows;
      if (expected) {
        parameters.append(expected->value);
        rows =
            transaction.exec("UPDATE kiarabot.login_identities SET password_credential=$3,"
                             "version=$4 WHERE normalized_login=$1 AND subject=$2 AND version=$5 "
                             "RETURNING normalized_login",
                             parameters);
      } else {
        rows = transaction.exec("INSERT INTO kiarabot.login_identities VALUES($1,$2,$3,$4) ON "
                                "CONFLICT DO NOTHING RETURNING normalized_login",
                                parameters);
      }
      return rows.empty() ? target::Status{conflict()} : target::Status{std::monostate{}};
    });
  }
  target::Status save(const target::CredentialVersionRecord& record,
                      std::optional<target::ResourceVersion> expected) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      if (!text_valid(record) || !valid_next_version(record.ref.version, expected)) {
        return invalid();
      }
      auto validation = target::validate_credential_record(record);
      if (std::holds_alternative<target::Failure>(validation)) {
        return validation;
      }
      std::optional<std::string> fingerprint;
      if (const auto* certificate = std::get_if<target::CertificateBinding>(&record.material)) {
        fingerprint = digest(certificate->fingerprint);
      }
      std::optional<std::string> bootstrap_key;
      std::optional<std::string> bootstrap_tag;
      if (const auto* bootstrap =
              std::get_if<target::WorkloadBootstrapVerifier>(&record.material)) {
        bootstrap_key = bootstrap->verifier.key.value;
        bootstrap_tag = digest(bootstrap->verifier.tag);
      }
      pqxx::params parameters{record.ref.id.value,
                              record.subject.value,
                              record.ref.version.value,
                              fingerprint,
                              static_cast<int>(record.material.index()),
                              bootstrap_key,
                              bootstrap_tag};
      pqxx::result rows;
      if (expected) {
        parameters.append(expected->value);
        rows = transaction.exec(
            "UPDATE kiarabot.credential_heads SET version=$3,"
            "leaf_fingerprint=$4::bytea,material=$5,bootstrap_key=$6,bootstrap_tag=$7::bytea "
            "WHERE id=$1 AND subject=$2 AND version=$8 RETURNING id",
            parameters);
      } else {
        rows = transaction.exec("INSERT INTO kiarabot.credential_heads "
                                "(id,subject,version,leaf_fingerprint,material,bootstrap_key,"
                                "bootstrap_tag) VALUES($1,$2,$3,"
                                "$4::bytea,$5,$6,$7::bytea) ON CONFLICT DO NOTHING RETURNING id",
                                parameters);
      }
      if (rows.empty()) {
        return conflict();
      }
      transaction.exec(
          "INSERT INTO kiarabot.credential_versions VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
          pqxx::params{record.ref.id.value, record.ref.version.value, record.subject.value,
                       static_cast<int>(record.material.index()), static_cast<int>(record.state),
                       timestamp(record.created_at),
                       record.revoked_at ? std::optional<std::string>(timestamp(*record.revoked_at))
                                         : std::nullopt,
                       record.expires_at ? std::optional<std::string>(timestamp(*record.expires_at))
                                         : std::nullopt,
                       static_cast<int>(record.binding.kind), record.binding.id.value});
      insert_material(transaction, record);
      return std::monostate{};
    });
  }

private:
  SqlWork& sql_;
};
class Bootstrap final : public target::IdentityBootstrapRepository {
public:
  explicit Bootstrap(SqlWork& sql) : sql_(sql) {}
  target::Outcome<target::AdministratorBootstrap> state() override {
    return query<target::AdministratorBootstrap>(
        sql_, [&](auto& transaction) -> target::Outcome<target::AdministratorBootstrap> {
          const auto row = transaction
                               .exec("SELECT * FROM kiarabot.administrator_bootstrap WHERE "
                                     "singleton")
                               .one_row();
          std::optional<target::SubjectId> administrator;
          if (!row["administrator"].is_null()) {
            administrator = target::SubjectId{row["administrator"].template as<std::string>()};
            require_stored(valid_id(administrator->value));
          }
          return target::AdministratorBootstrap{
              static_cast<target::BootstrapState>(row["state"].template as<int>()),
              {row["version"].template as<std::uint64_t>()},
              administrator,
              optional_time(row, "consumed_at")};
        });
  }
  target::Status consume(const target::AdministratorBootstrap& consumed,
                         target::ResourceVersion expected) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      if (consumed.state != target::BootstrapState::Consumed || !consumed.administrator ||
          !valid_id(consumed.administrator->value) || !consumed.consumed_at ||
          expected.value == 0 || !valid_next_version(consumed.version, expected)) {
        return invalid();
      }
      const auto exists = transaction.exec("SELECT singleton FROM kiarabot.administrator_bootstrap "
                                           "WHERE singleton");
      require_stored(!exists.empty());
      const auto rows =
          transaction.exec("UPDATE kiarabot.administrator_bootstrap SET state=1,"
                           "version=$1,administrator=$2,consumed_at=$3 WHERE singleton "
                           "AND state=0 AND version=$4 RETURNING singleton",
                           pqxx::params{consumed.version.value, consumed.administrator->value,
                                        timestamp(*consumed.consumed_at), expected.value});
      return rows.empty() ? target::Status{conflict()} : target::Status{std::monostate{}};
    });
  }

private:
  SqlWork& sql_;
};
} // namespace

std::unique_ptr<target::IdentityCredentialRepository> credential_repository(SqlWork& sql) {
  return std::make_unique<Credentials>(sql);
}
std::unique_ptr<target::IdentityBootstrapRepository> bootstrap_repository(SqlWork& sql) {
  return std::make_unique<Bootstrap>(sql);
}
} // namespace kiarabot::infrastructure::postgres::detail
