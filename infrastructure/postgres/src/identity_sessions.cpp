#include <algorithm>
#include <limits>
#include <utility>

#include "identity_access.hpp"
#include "identity_security.hpp"
#include "kiarabot/core/domain/target/identity_credential_rules.hpp"

namespace kiarabot::infrastructure::postgres::detail {
namespace {
namespace target = core::target;
using identity::conflict;
using identity::invalid;
using identity::query;
using identity::require_stored;
using identity::valid_id;
using identity::valid_next_version;

bool valid_verifier(const target::TokenVerifier& verifier, target::TokenPurpose purpose) {
  return verifier.purpose == purpose && valid_id(verifier.key.value);
}
bool valid_session(const target::SessionSecurity& session) {
  return valid_id(session.id.value) && valid_id(session.subject.value) &&
         valid_id(session.credential.id.value) && session.credential.version.value > 0 &&
         valid_verifier(session.access, target::TokenPurpose::Access) &&
         valid_verifier(session.refresh, target::TokenPurpose::Refresh) && session.rotation > 0 &&
         session.version.value > 0 && session.access_expires_at > session.created_at &&
         session.refresh_expires_at >= session.access_expires_at && session.control_peer &&
         target::valid_session_control_peer(*session.control_peer) &&
         valid_id(session.control_peer->subject.value) &&
         valid_id(session.control_peer->credential.value);
}
target::SessionSecurity read_session(const pqxx::row& row) {
  target::SessionSecurity session{
      {row["id"].as<std::string>()},
      {row["subject"].as<std::string>()},
      {{row["credential_id"].as<std::string>()}, {row["credential_version"].as<std::uint64_t>()}},
      {target::TokenPurpose::Access,
       {row["access_key"].as<std::string>()},
       read_digest(row["access_tag"])},
      {target::TokenPurpose::Refresh,
       {row["refresh_key"].as<std::string>()},
       read_digest(row["refresh_tag"])},
      time_field(row, "created_at"),
      time_field(row, "access_expires_at"),
      time_field(row, "refresh_expires_at"),
      row["rotation"].as<std::uint64_t>(),
      {row["version"].as<std::uint64_t>()},
      row["revoked"].as<bool>(),
      target::SessionControlPeer{{row["control_subject"].as<std::string>()},
                                 {row["control_credential"].as<std::string>()}}};
  require_stored(valid_session(session));
  return session;
}
target::CurrentRefreshHandle current_handle(const target::SessionSecurity& session) {
  return {session.id,      session.subject,  session.credential,
          session.refresh, session.rotation, session.refresh_expires_at};
}
target::ConsumedRefreshHandle read_consumed(const pqxx::row& row) {
  target::ConsumedRefreshHandle result{
      {{row["session"].as<std::string>()},
       {row["subject"].as<std::string>()},
       {{row["credential_id"].as<std::string>()}, {row["credential_version"].as<std::uint64_t>()}},
       {target::TokenPurpose::Refresh,
        {row["verifier_key"].as<std::string>()},
        read_digest(row["verifier_tag"])},
       row["input_rotation"].as<std::uint64_t>(),
       time_field(row, "input_expires_at")},
      {{row["control_peer"].as<std::string>()},
       {row["session"].as<std::string>()},
       row["idempotency_key"].as<std::string>()},
      time_field(row, "consumed_at"),
      time_field(row, "recovery_until")};
  require_stored(
      valid_id(result.input.session.value) && valid_id(result.input.subject.value) &&
      valid_id(result.input.credential.id.value) && result.input.credential.version.value > 0 &&
      valid_verifier(result.input.input_verifier, target::TokenPurpose::Refresh) &&
      result.input.input_rotation > 0 && valid_id(result.receipt_scope.control_peer.value) &&
      valid_id(result.receipt_scope.idempotency_key));
  return result;
}
bool same_consumed(const target::ConsumedRefreshHandle& left,
                   const target::ConsumedRefreshHandle& right) {
  return left.input.session == right.input.session && left.input.subject == right.input.subject &&
         left.input.credential == right.input.credential &&
         left.input.input_verifier.key == right.input.input_verifier.key &&
         left.input.input_verifier.tag == right.input.input_verifier.tag &&
         left.input.input_rotation == right.input.input_rotation &&
         left.input.input_expires_at == right.input.input_expires_at &&
         left.receipt_scope == right.receipt_scope && left.consumed_at == right.consumed_at &&
         left.recovery_until == right.recovery_until;
}
inline constexpr std::string_view kDbNow =
    "(extract(epoch FROM clock_timestamp())*1000000000)::bigint";

class Sessions final : public target::IdentitySessionSecurityRepository {
public:
  explicit Sessions(SqlWork& sql) : sql_(sql) {}
  target::Outcome<std::optional<target::SessionSecurity>>
  find(const target::SessionId& id) override {
    return query<std::optional<target::SessionSecurity>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::optional<target::SessionSecurity>> {
          if (!valid_id(id.value)) {
            return invalid();
          }
          const auto rows = transaction.exec("SELECT * FROM kiarabot.session_security WHERE id=$1",
                                             pqxx::params{id.value});
          if (rows.empty()) {
            return std::optional<target::SessionSecurity>{};
          }
          return std::optional<target::SessionSecurity>(read_session(rows.one_row()));
        });
  }
  target::Outcome<std::optional<target::SessionSecurity>>
  by_access(const target::TokenVerifier& verifier) override {
    return by_verifier(verifier, target::TokenPurpose::Access);
  }
  target::Outcome<std::optional<target::SessionSecurity>>
  by_refresh(const target::TokenVerifier& verifier) override {
    return by_verifier(verifier, target::TokenPurpose::Refresh);
  }
  target::Outcome<std::optional<target::RefreshHandle>>
  resolve_refresh(const target::TokenVerifier& input, const target::RefreshRecoveryLookup& lookup,
                  target::TimePoint) override {
    return query<std::optional<target::RefreshHandle>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::optional<target::RefreshHandle>> {
          if (!valid_verifier(input, target::TokenPurpose::Refresh) ||
              !valid_id(lookup.control_peer.value) || !valid_id(lookup.idempotency_key)) {
            return invalid();
          }
          const auto current =
              transaction.exec("SELECT * FROM kiarabot.session_security WHERE "
                               "refresh_key=$1 AND refresh_tag=$2::bytea AND refresh_expires_at>" +
                                   std::string(kDbNow),
                               pqxx::params{input.key.value, digest(input.tag)});
          if (!current.empty()) {
            return std::optional<target::RefreshHandle>(
                current_handle(read_session(current.one_row())));
          }
          const auto consumed = transaction.exec(
              "SELECT * FROM kiarabot.consumed_refresh WHERE "
              "verifier_key=$1 AND verifier_tag=$2::bytea AND control_peer=$3 AND "
              "idempotency_key=$4 "
              "AND recovery_until>" +
                  std::string(kDbNow) + " AND input_expires_at>" + std::string(kDbNow),
              pqxx::params{input.key.value, digest(input.tag), lookup.control_peer.value,
                           lookup.idempotency_key});
          if (consumed.empty()) {
            return std::optional<target::RefreshHandle>{};
          }
          return std::optional<target::RefreshHandle>(read_consumed(consumed.one_row()));
        });
  }
  target::Status
  record_refresh_consumption(const target::ConsumedRefreshHandle& consumed) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      const auto& input = consumed.input;
      if (!valid_id(input.session.value) || !valid_id(input.subject.value) ||
          !valid_id(input.credential.id.value) || input.credential.version.value == 0 ||
          !valid_verifier(input.input_verifier, target::TokenPurpose::Refresh) ||
          input.input_rotation == 0 || consumed.receipt_scope.session != input.session ||
          !valid_id(consumed.receipt_scope.control_peer.value) ||
          !valid_id(consumed.receipt_scope.idempotency_key)) {
        return invalid();
      }
      const auto owner = transaction.exec("SELECT subject FROM kiarabot.sessions WHERE id=$1",
                                          pqxx::params{input.session.value});
      if (owner.empty() || owner.one_row()[0].as<std::string>() != input.subject.value) {
        return conflict();
      }
      const auto existing = transaction.exec(
          "SELECT * FROM kiarabot.consumed_refresh WHERE "
          "verifier_key=$1 AND verifier_tag=$2::bytea FOR UPDATE",
          pqxx::params{input.input_verifier.key.value, digest(input.input_verifier.tag)});
      if (!existing.empty()) {
        return same_consumed(read_consumed(existing.one_row()), consumed)
                   ? target::Status{std::monostate{}}
                   : target::Status{conflict()};
      }
      const auto rows = transaction.exec(
          "INSERT INTO kiarabot.consumed_refresh SELECT "
          "$1,$2::bytea,$3,$4,$5,$6,$7,$8,1,$9,$10,$11,$12 "
          "WHERE $11::bigint<=" +
              std::string(kDbNow) + " AND $12::bigint>" + std::string(kDbNow) +
              " AND $12::numeric-$11::numeric<=86400000000000 AND $12::bigint<=$8::bigint "
              "RETURNING session",
          pqxx::params{input.input_verifier.key.value, digest(input.input_verifier.tag),
                       input.session.value, input.subject.value, input.credential.id.value,
                       input.credential.version.value, input.input_rotation,
                       timestamp(input.input_expires_at), consumed.receipt_scope.control_peer.value,
                       consumed.receipt_scope.idempotency_key, timestamp(consumed.consumed_at),
                       timestamp(consumed.recovery_until)});
      return rows.empty() ? target::Status{invalid()} : target::Status{std::monostate{}};
    });
  }
  target::Outcome<std::vector<target::SessionMetadata>>
  list_metadata(const target::SubjectId& subject, std::size_t max_records) override {
    return query<std::vector<target::SessionMetadata>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::vector<target::SessionMetadata>> {
          if (!valid_id(subject.value) || max_records == 0 ||
              max_records >= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            return invalid();
          }
          // One bounded read, no count/query race, no verifier columns or query-time timestamps.
          const auto rows =
              transaction.exec("SELECT id,subject,created_at,refresh_expires_at,rotation,"
                               "version,revoked,control_subject,control_credential FROM "
                               "kiarabot.session_security WHERE subject=$1 "
                               "ORDER BY id LIMIT $2",
                               pqxx::params{subject.value, max_records + 1});
          if (static_cast<std::size_t>(rows.size()) > max_records) {
            return target::rejected(target::ErrorCode::RateLimited, target::Recovery::Backoff);
          }
          std::vector<target::SessionMetadata> metadata;
          for (const auto& row : rows) {
            target::SessionMetadata entry{{row["id"].template as<std::string>()},
                                          {row["subject"].template as<std::string>()},
                                          time_field(row, "created_at"),
                                          time_field(row, "refresh_expires_at"),
                                          row["rotation"].template as<std::uint64_t>(),
                                          {row["version"].template as<std::uint64_t>()},
                                          row["revoked"].template as<bool>()};
            require_stored(valid_id(row["control_subject"].template as<std::string>()) &&
                           valid_id(row["control_credential"].template as<std::string>()) &&
                           valid_id(entry.id.value) && entry.subject == subject &&
                           entry.expires_at > entry.created_at && entry.rotation > 0 &&
                           entry.version.value > 0);
            metadata.push_back(std::move(entry));
          }
          return metadata;
        });
  }
  target::Outcome<std::size_t> count_unexpired() override {
    return query<std::size_t>(
        sql_, [&](pqxx::transaction_base& transaction) -> target::Outcome<std::size_t> {
          return transaction
              .exec("SELECT count(*) FROM kiarabot.session_security WHERE NOT revoked "
                    "AND refresh_expires_at>" +
                    std::string(kDbNow))
              .one_row()[0]
              .as<std::size_t>();
        });
  }
  target::Status save(const target::SessionSecurity& session,
                      std::optional<target::ResourceVersion> expected,
                      std::optional<std::uint64_t> expected_rotation) override {
    return sql_.run([&](pqxx::transaction_base& transaction) -> target::Status {
      if (!valid_session(session) || !valid_next_version(session.version, expected) ||
          expected.has_value() != expected_rotation.has_value() ||
          (expected_rotation && (*expected_rotation == 0 || session.rotation < *expected_rotation ||
                                 session.rotation - *expected_rotation > 1))) {
        return invalid();
      }
      const auto old = transaction.exec("SELECT created_at,control_subject,control_credential FROM "
                                        "kiarabot.session_security WHERE "
                                        "id=$1 FOR UPDATE",
                                        pqxx::params{session.id.value});
      if (!old.empty()) {
        const auto row = old.one_row();
        require_stored(!row["control_subject"].is_null() && !row["control_credential"].is_null() &&
                       valid_id(row["control_subject"].template as<std::string>()) &&
                       valid_id(row["control_credential"].template as<std::string>()));
      }
      if ((expected &&
           (old.empty() || time_field(old.one_row(), "created_at") != session.created_at ||
            old.one_row()["control_subject"].template as<std::string>() !=
                session.control_peer->subject.value ||
            old.one_row()["control_credential"].template as<std::string>() !=
                session.control_peer->credential.value)) ||
          (!expected && !old.empty())) {
        return conflict();
      }
      if (!expected) {
        const auto control = transaction.exec(
            "SELECT p.kind,c.platform_service FROM kiarabot.credential_heads h "
            "JOIN kiarabot.principals p ON p.id=h.subject "
            "JOIN kiarabot.credential_certificates c ON c.id=h.id AND c.version=h.version "
            "WHERE h.id=$1 AND h.subject=$2",
            pqxx::params{session.control_peer->credential.value,
                         session.control_peer->subject.value});
        if (control.empty() ||
            control.one_row()["kind"].template as<int>() !=
                static_cast<int>(target::PrincipalKind::PlatformService) ||
            control.one_row()["platform_service"].is_null() ||
            control.one_row()["platform_service"].template as<int>() !=
                static_cast<int>(target::Service::Control)) {
          return invalid();
        }
      }
      const target::LoginSession base{session.id,
                                      session.subject,
                                      session.version,
                                      session.rotation,
                                      session.refresh_expires_at,
                                      session.revoked};
      if (!store_live(transaction, RecordKind::Session, target::ReplayResult(base), expected)) {
        const auto identical = transaction.exec(
            "SELECT id FROM kiarabot.sessions WHERE id=$1 AND "
            "subject=$2 AND version=$3 AND rotation=$4 AND expires_at=$5 AND revoked=$6",
            pqxx::params{session.id.value, session.subject.value, session.version.value,
                         session.rotation, timestamp(session.refresh_expires_at), session.revoked});
        if (identical.empty()) {
          return conflict();
        }
      }
      pqxx::params parameters{session.id.value,
                              session.subject.value,
                              session.credential.id.value,
                              session.credential.version.value,
                              session.access.key.value,
                              digest(session.access.tag),
                              session.refresh.key.value,
                              digest(session.refresh.tag),
                              timestamp(session.created_at),
                              timestamp(session.access_expires_at),
                              timestamp(session.refresh_expires_at),
                              session.rotation,
                              session.version.value,
                              session.revoked,
                              session.control_peer->subject.value,
                              session.control_peer->credential.value};
      pqxx::result rows;
      if (expected) {
        parameters.append(expected->value);
        parameters.append(*expected_rotation);
        rows = transaction.exec(
            "UPDATE kiarabot.session_security SET credential_id=$3,"
            "credential_version=$4,access_key=$5,access_tag=$6::bytea,refresh_key=$7,"
            "refresh_tag=$8::bytea,access_expires_at=$10,refresh_expires_at=$11,rotation=$12,"
            "version=$13,revoked=$14 WHERE id=$1 AND subject=$2 AND created_at=$9 AND version=$17 "
            "AND rotation=$18 AND control_subject=$15 AND control_credential=$16 RETURNING id",
            parameters);
      } else {
        rows = transaction.exec(
            "INSERT INTO kiarabot.session_security VALUES($1,$2,$3,$4,$5,$6::bytea,"
            "$7,$8::bytea,$9,$10,$11,$12,$13,$14,$15,$16) ON CONFLICT DO NOTHING RETURNING id",
            parameters);
      }
      return rows.empty() ? target::Status{conflict()} : target::Status{std::monostate{}};
    });
  }

private:
  target::Outcome<std::optional<target::SessionSecurity>>
  by_verifier(const target::TokenVerifier& verifier, target::TokenPurpose purpose) {
    return query<std::optional<target::SessionSecurity>>(
        sql_, [&](auto& transaction) -> target::Outcome<std::optional<target::SessionSecurity>> {
          if (!valid_verifier(verifier, purpose)) {
            return invalid();
          }
          const std::string prefix = purpose == target::TokenPurpose::Access ? "access" : "refresh";
          const auto rows =
              transaction.exec("SELECT * FROM kiarabot.session_security WHERE " + prefix +
                                   "_key=$1 AND " + prefix + "_tag=$2::bytea",
                               pqxx::params{verifier.key.value, digest(verifier.tag)});
          if (rows.empty()) {
            return std::optional<target::SessionSecurity>{};
          }
          return std::optional<target::SessionSecurity>(read_session(rows.one_row()));
        });
  }
  SqlWork& sql_;
};
} // namespace
std::unique_ptr<target::IdentitySessionSecurityRepository>
session_security_repository(SqlWork& sql) {
  return std::make_unique<Sessions>(sql);
}
} // namespace kiarabot::infrastructure::postgres::detail
