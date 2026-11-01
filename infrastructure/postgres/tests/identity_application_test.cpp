#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <variant>

#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <pqxx/pqxx>
#include <sys/stat.h>

#include "identity_application_tls.hpp"
#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/core/application/target/identity_delegation_application.hpp"
#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"
#include "kiarabot/infrastructure/identity_crypto/verification_keys.hpp"
#include "kiarabot/infrastructure/postgres/security_factory.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace pg = kiarabot::infrastructure::postgres;
namespace crypto = kiarabot::infrastructure::identity_crypto;
const char* phase = "fixture safety";
class CheckFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
void check(bool condition, const char* message) {
  if (!condition) {
    throw CheckFailure(message);
  }
}
template <typename Value>
Value take(t::Outcome<Value> result,
           const std::source_location location = std::source_location::current()) {
  if (const auto* failure = std::get_if<t::Failure>(&result)) {
    std::cerr << "P03 assertion: " << phase << " stable code " << static_cast<int>(failure->code)
              << " at line " << location.line() << '\n';
  }
  check(std::holds_alternative<Value>(result), "unexpected application/adapter failure");
  return std::move(std::get<Value>(result));
}
template <typename Value> void expect(const t::Outcome<Value>& result, t::ErrorCode code) {
  check(std::holds_alternative<t::Failure>(result) && std::get<t::Failure>(result).code == code,
        "wrong stable application failure");
}
std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  check(input.good(), "missing owned fixture file");
  return {std::istreambuf_iterator<char>(input), {}};
}
std::string secret_file(const std::filesystem::path& path) {
  struct stat attributes {};
  check(::lstat(path.c_str(), &attributes) == 0 && S_ISREG(attributes.st_mode) &&
            attributes.st_uid == ::getuid() && (attributes.st_mode & 0777) == 0600,
        "unsafe fixture secret file");
  auto value = read_file(path);
  check(value.size() == 65 && value.back() == '\n', "invalid fixture secret shape");
  value.pop_back();
  check(std::all_of(
            value.begin(), value.end(),
            [](char item) { return (item >= '0' && item <= '9') || (item >= 'a' && item <= 'f'); }),
        "invalid fixture secret representation");
  return value;
}
std::string quote(std::string_view input) {
  std::string result = "'";
  for (char item : input) {
    if (item == '\'' || item == '\\') {
      result += '\\';
    }
    result += item;
  }
  return result + '\'';
}
std::string dsn(const pg::DatabaseConfig& config) {
  return "host=" + quote(config.host) + " port=" + std::to_string(config.port) +
         " dbname=" + quote(config.database) + " user=" + quote(config.user) +
         " password=" + quote(config.password) +
         " sslmode=verify-full channel_binding=require connect_timeout=3 sslrootcert=" +
         quote(config.root_certificate);
}
class FixtureClock final : public t::SecurityClockPort {
public:
  t::Outcome<t::SecurityClockSample> sample() override {
    // Test-only deployment evidence: the owned PG cluster and this process use the same local
    // kernel UTC clock. A startup DB comparison must pass before this sample is trusted.
    return t::SecurityClockSample{t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())},
                                  std::chrono::steady_clock::now(), std::chrono::milliseconds{2000},
                                  trusted_};
  }
  void attest_local_database(t::TimePoint database, t::TimePoint before, t::TimePoint after) {
    check(before <= after && database >= before - std::chrono::seconds{2} &&
              database <= after + std::chrono::seconds{2},
          "fixture UTC evidence mismatch");
    trusted_ = true;
  }

private:
  bool trusted_ = false;
};
t::TimePoint database_time(t::IdentitySecurityUnitOfWorkFactory& factory) {
  auto work = take(factory.begin());
  const auto now = take(work->current_time());
  work->rollback();
  return now;
}
t::IdentityRequestContext context(FixtureClock& clock, const std::string& key) {
  return {"10000000-0000-4000-8000-000000000001", "10000000-0000-4000-8000-000000000002", key,
          take(clock.sample()).utc + std::chrono::seconds{30}};
}
crypto::DelegationConfig signing_config(t::TimePoint valid_until) {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> public_bio(BIO_new(BIO_s_mem()), BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> private_bio(BIO_new(BIO_s_mem()), BIO_free);
  check(key && public_bio && private_bio &&
            PEM_write_bio_PUBKEY(public_bio.get(), key.get()) == 1 &&
            PEM_write_bio_PrivateKey(private_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                     nullptr) == 1,
        "fixture signing key generation failed");
  char* public_data = nullptr;
  char* private_data = nullptr;
  const auto public_size = BIO_get_mem_data(public_bio.get(), &public_data);
  const auto private_size = BIO_get_mem_data(private_bio.get(), &private_data);
  check(public_size > 0 && private_size > 0, "fixture signing key encoding failed");
  auto secured = crypto::copy_secret(
      std::as_bytes(std::span{private_data, static_cast<std::size_t>(private_size)}));
  OPENSSL_cleanse(private_data, static_cast<std::size_t>(private_size));
  crypto::DelegationConfig config{
      "kiarabot-identity",
      {},
      {1},
      {t::ResourceKind::Catalog, {"60000000-0000-4000-8000-000000000001"}}};
  config.keys.push_back({{"delegation-key"},
                         std::string(public_data, static_cast<std::size_t>(public_size)),
                         take(std::move(secured)),
                         {1},
                         valid_until});
  return config;
}
void application_flow(const pg::DatabaseConfig& config) {
  phase = "real crypto setup";
  auto factory = take(pg::open_identity_security(config));
  auto random = take(crypto::make_random());
  auto passwords = take(crypto::make_password_crypto());
  auto password = take(random->opaque_token());
  auto wrong_password = take(random->opaque_token());
  auto verifier = take(passwords->derive(password->bytes()));
  auto dummy = take(passwords->derive(wrong_password->bytes()));
  std::vector<crypto::SymmetricKey> token_keys;
  token_keys.push_back({{"token-key"}, take(random->bytes(32))});
  token_keys.push_back({{"request-key"}, take(random->bytes(32))});
  auto tokens = take(crypto::make_token_crypto(std::move(token_keys)));
  std::vector<crypto::SymmetricKey> receipt_keys;
  receipt_keys.push_back({{"receipt-key"}, take(random->bytes(32))});
  auto receipts = take(crypto::make_secret_receipt_crypto(std::move(receipt_keys)));
  FixtureClock clock;
  const auto before = take(clock.sample()).utc;
  const auto now = database_time(*factory);
  clock.attest_local_database(now, before, take(clock.sample()).utc);
  const auto signing_key_valid_until = take(clock.sample()).utc + std::chrono::hours{1};
  auto delegation_bundle =
      take(crypto::make_delegation_crypto_bundle(signing_config(signing_key_valid_until), clock));
  auto& delegations = delegation_bundle.crypto;
  const auto material = take(delegation_bundle.verification_keys->snapshot());
  check(material.version.value == 1 && material.keys.size() == 1 &&
            material.keys.front().reference == t::CryptoKeyRef{"delegation-key"} &&
            material.keys.front().version.value == 1 &&
            material.keys.front().valid_until == signing_key_valid_until &&
            material.keyset_resource ==
                t::ResourceRef{t::ResourceKind::Catalog, {"60000000-0000-4000-8000-000000000001"}},
        "trusted signing/public material metadata diverged");
  const t::SubjectId user{"20000000-0000-4000-8000-000000000001"};
  const t::SubjectId control{"20000000-0000-4000-8000-000000000002"};
  const t::CredentialId user_credential{"30000000-0000-4000-8000-000000000001"};
  const t::CredentialId control_credential{"30000000-0000-4000-8000-000000000002"};
  phase = "real TLS evidence";
  const auto tls = pg::test::control_tls_evidence(control);
  auto certificates =
      take(crypto::make_peer_certificate_crypto({{tls.root_pem}, crypto::PeerSanKind::Uri}));
  const t::RegisteredPeer enrollment{
      t::PlatformPeer{t::Service::Control, control}, tls.registration, {control_credential, {1}}};
  take(certificates->verify(tls.leaf_der, enrollment, now));
  phase = "offline bootstrap";
  auto admin = std::make_unique<t::IdentityAdminApplication>(*factory, *certificates);
  const t::ResourceRef own_principal{t::ResourceKind::Principal, {user.value}};
  const t::OfflineIdentityBootstrap bootstrap{
      context(clock, "offline-bootstrap"),
      {1},
      user,
      user_credential,
      "alice",
      verifier,
      now + std::chrono::hours{24},
      control,
      control_credential,
      tls.registration,
      {{{"40000000-0000-4000-8000-000000000001"},
        user,
        {{t::FixedRole::Admin,
          {t::ResourceKind::Catalog, {"50000000-0000-4000-8000-000000000001"}}}},
        {},
        {1}}},
      {{{"40000000-0000-4000-8000-000000000002"},
        control,
        {},
        {{t::Permission::IdentityDelegationIssue, {t::ResourceKind::Principal, {control.value}}}},
        {1}}}};
  check(take(admin->bootstrap_offline(bootstrap)).marker.state == t::BootstrapState::Consumed,
        "offline bootstrap did not consume durable marker");
  expect(admin->bootstrap_offline(bootstrap), t::ErrorCode::Conflict);
  auto peer = [&]() {
    return take(admin->authenticate_peer(
        {tls.registration.fingerprint, context(clock, "peer").deadline}, tls.leaf_der));
  };
  const t::IdentitySecurityOptions options{{"token-key"},
                                           {{"token-key"}},
                                           {"request-key"},
                                           {"receipt-key"},
                                           dummy,
                                           std::chrono::hours{24},
                                           {100},
                                           10,
                                           10,
                                           100};
  auto app = take(t::make_identity_security_application(
      {*factory, *passwords, *tokens, *receipts, *random, clock, *delegations}, options));
  auto issuer = std::make_unique<t::IdentityDelegationApplication>(
      *app, *factory, *delegations, *random, clock,
      t::IdentityDelegationOptions{"kiarabot-identity", {"delegation-key"}});
  phase = "login rejection";
  expect(app->login({context(clock, "wrong-password"), peer(), "alice", wrong_password->bytes()}),
         t::ErrorCode::Unauthenticated);
  phase = "login and real encrypted replay";
  auto first =
      take(app->login({context(clock, "login-first"), peer(), "ALICE", password->bytes()}));
  auto first_replay =
      take(app->login({context(clock, "login-first"), peer(), "alice", password->bytes()}));
  check(first.tokens.session == first_replay.tokens.session && first.tokens.rotation == 1 &&
            std::ranges::equal(first.tokens.access->bytes(), first_replay.tokens.access->bytes()) &&
            std::ranges::equal(first.tokens.refresh->bytes(), first_replay.tokens.refresh->bytes()),
        "login replay did not decrypt original committed secrets");
  auto second =
      take(app->login({context(clock, "login-second"), peer(), "alice", password->bytes()}));
  check(first.tokens.session != second.tokens.session, "new login reused first session");
  phase = "online access and principal";
  const auto valid_first = take(app->validate_session(
      {context(clock, "access-first"), peer(), first.tokens.access->bytes()}));
  const auto valid_second = take(app->validate_session(
      {context(clock, "access-second"), peer(), second.tokens.access->bytes()}));
  check(valid_first.principal.id == user && valid_first.session.rotation == 1 &&
            valid_first.session.created_at < valid_first.session.expires_at,
        "real access validation lost session metadata");
  check(
      take(app->get_principal({context(clock, "principal"), peer(), first.tokens.access->bytes()}))
              .principal.id == user,
      "current principal mismatch");
  auto issue = [&](t::SecretView access) {
    auto issued = take(
        issuer->issue_delegation({{context(clock, "issue"), peer(), access},
                                  t::Service::Identity,
                                  {{t::Permission::IdentitySessionRead, own_principal},
                                   {t::Permission::IdentitySessionRevoke,
                                    {t::ResourceKind::Session, {second.tokens.session.value}}}}}));
    check(issued.claims.expires_at <= signing_key_valid_until,
          "real issued proof exceeds finite signing key validity");
    return issued;
  };
  phase = "real IssueDelegation";
  auto proof = issue(first.tokens.access->bytes());
  phase = "first snapshot page";
  const auto page1 = take(app->list_sessions(
      {context(clock, "list-first"), {peer(), proof.proof->bytes()}, user, {1, {}, {}}}));
  check(page1.sessions.size() == 1 && !page1.next_page_token.empty(),
        "first snapshot page missing continuation");
  phase = "token-only continuation page";
  const auto page2 = take(app->list_sessions({context(clock, "list-second"),
                                              {peer(), proof.proof->bytes()},
                                              user,
                                              {1, page1.next_page_token, {}}}));
  check(page2.sessions.size() == 1 && page2.next_page_token.empty() &&
            page2.sessions.front().id != page1.sessions.front().id &&
            page2.snapshot_id == page1.snapshot_id,
        "opaque token-only pagination duplicated or lost session");
  phase = "refresh and consumed-input real replay";
  auto refreshed = take(app->refresh_session(
      {context(clock, "refresh-first"), peer(), first.tokens.refresh->bytes()}));
  check(refreshed.tokens.session == first.tokens.session && refreshed.tokens.rotation == 2,
        "refresh did not CAS to next rotation");
  auto refresh_replay = take(app->refresh_session(
      {context(clock, "refresh-first"), peer(), first.tokens.refresh->bytes()}));
  check(refresh_replay.tokens.rotation == 2 &&
            std::ranges::equal(refreshed.tokens.access->bytes(),
                               refresh_replay.tokens.access->bytes()) &&
            std::ranges::equal(refreshed.tokens.refresh->bytes(),
                               refresh_replay.tokens.refresh->bytes()),
        "consumed refresh did not open original real AEAD receipt");
  expect(app->refresh_session(
             {context(clock, "different-key"), peer(), first.tokens.refresh->bytes()}),
         t::ErrorCode::Unauthenticated);
  expect(
      app->validate_session({context(clock, "old-access"), peer(), first.tokens.access->bytes()}),
      t::ErrorCode::Unauthenticated);
  check(take(app->validate_session(
                 {context(clock, "new-access"), peer(), refreshed.tokens.access->bytes()}))
                .session.rotation == 2,
        "new access token not authoritative after refresh");
  phase = "revoke with real proof and ordinary snapshot replay";
  auto revoke_proof = issue(refreshed.tokens.access->bytes());
  phase = "revoke target session";
  const auto revoked = take(app->revoke_session({context(clock, "revoke-second"),
                                                 {peer(), revoke_proof.proof->bytes()},
                                                 second.tokens.session}));
  check(revoked.session.revoked && revoked.session.created_at == valid_second.session.created_at &&
            revoked.session.version.value == valid_second.session.version.value + 1,
        "revoke result lost original creation fact");
  phase = "fresh real IssueDelegation after revoke";
  auto fresh_proof = issue(refreshed.tokens.access->bytes());
  phase = "ordinary revoke snapshot replay";
  const auto revoked_replay = take(app->revoke_session({context(clock, "revoke-second"),
                                                        {peer(), fresh_proof.proof->bytes()},
                                                        second.tokens.session}));
  check(revoked_replay.session.created_at == revoked.session.created_at &&
            revoked_replay.session.expires_at == revoked.session.expires_at &&
            revoked_replay.session.version == revoked.session.version &&
            revoked_replay.session.revoked,
        "ordinary revoke replay reconstructed mutable metadata");
  expect(app->validate_session(
             {context(clock, "revoked-access"), peer(), second.tokens.access->bytes()}),
         t::ErrorCode::Unauthenticated);
  expect(app->login({context(clock, "login-second"), peer(), "alice", password->bytes()}),
         t::ErrorCode::Unauthenticated);
  phase = "durable restart";
  issuer.reset();
  app.reset();
  admin.reset();
  factory.reset();
  factory = take(pg::open_identity_security(config));
  auto work = take(factory->begin());
  check(take(work->session_security().count_unexpired()) == 1 &&
            take(work->bootstrap().state()).state == t::BootstrapState::Consumed,
        "new factory lost committed application state");
  work->rollback();
}
void run(const std::filesystem::path& supplied, std::uint16_t port) {
  check(!std::filesystem::is_symlink(supplied), "unsafe fixture root");
  const auto root = std::filesystem::canonical(supplied);
  struct stat attributes {};
  check(root.filename().string().starts_with("kiarabot-p03-") &&
            ::stat(root.c_str(), &attributes) == 0 && attributes.st_uid == ::getuid() &&
            !std::filesystem::is_symlink(root / "p03-owned-marker") &&
            std::filesystem::is_regular_file(root / "p03-owned-marker"),
        "unsafe fixture ownership");
  const auto administrator_password = secret_file(root / "password");
  pg::DatabaseConfig administrator{t::Service::Identity,
                                   "localhost",
                                   port,
                                   "postgres",
                                   "p03_admin",
                                   administrator_password,
                                   (root / "server.pem").string(),
                                   {},
                                   {}};
  pqxx::connection deployment(dsn(administrator));
  {
    pqxx::read_transaction inspect(deployment);
    const auto actual = std::filesystem::canonical(
        inspect.exec("SHOW data_directory").one_row()[0].as<std::string>());
    check(!std::filesystem::is_symlink(root / "data") &&
              actual == std::filesystem::canonical(root / "data") && actual.parent_path() == root,
          "fixture backend directory mismatch");
    check(deployment.server_version() == 160004, "wrong fixture PostgreSQL version");
  }
  phase = "owned empty database deployment";
  const std::string migration = "p03_identity_migration";
  const std::string runtime = "p03_identity_runtime";
  const std::string database = "p04_identity_application";
  const auto migration_password = secret_file(root / ("password-" + migration));
  const auto runtime_password = secret_file(root / ("password-" + runtime));
  check(migration_password != runtime_password && migration_password != administrator_password &&
            runtime_password != administrator_password,
        "fixture role secrets reused");
  {
    pqxx::nontransaction create(deployment);
    for (const auto& role : {migration, runtime}) {
      create.exec("CREATE ROLE " + create.quote_name(role) +
                  " LOGIN NOINHERIT NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS "
                  "PASSWORD " +
                  create.quote(role == migration ? migration_password : runtime_password));
    }
    create.exec("CREATE DATABASE " + create.quote_name(database) + " OWNER " +
                create.quote_name(migration));
    create.exec("REVOKE ALL ON DATABASE " + create.quote_name(database) + " FROM PUBLIC");
    create.exec("GRANT CONNECT ON DATABASE " + create.quote_name(database) + " TO " +
                create.quote_name(runtime));
  }
  pg::DatabaseConfig config{
      t::Service::Identity,           "localhost", port,   database, migration, migration_password,
      (root / "server.pem").string(), migration,   runtime};
  take(pg::initialize_empty(config));
  config.user = runtime;
  config.password = runtime_password;
  application_flow(config);
}
} // namespace
int main(int argc, char** argv) {
  try {
    check(argc == 5, "fixture requires owned root, port, initializer and relay arguments");
    unsigned int port = 0;
    const std::string_view encoded(argv[2]);
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), port);
    check(parsed.ec == std::errc{} && parsed.ptr == encoded.data() + encoded.size() && port > 0 &&
              port <= 65535,
          "invalid fixture port");
    run(argv[1], static_cast<std::uint16_t>(port));
    std::cout
        << "P04 real Identity application + crypto + PostgreSQL: bootstrap, TLS evidence, login, "
           "refresh, validation, delegation, pagination, revoke and replay passed\n";
    return 0;
  } catch (const CheckFailure& error) {
    std::cerr << "P03 assertion: " << phase << ": " << error.what() << '\n';
  } catch (const pqxx::sql_error& error) {
    std::cerr << "P03 assertion: " << phase << ": database SQLSTATE " << error.sqlstate() << '\n';
  } catch (...) {
    std::cerr << "P03 assertion: " << phase << ": fixture dependency failure\n";
  }
  return 1;
}
