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
#include "identity_authorization_checks.hpp"
#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/core/application/target/identity_delegation_application.hpp"
#include "kiarabot/core/application/target/identity_invalidation_application.hpp"
#include "kiarabot/core/application/target/identity_principal_application.hpp"
#include "kiarabot/core/application/target/identity_principal_directory_application.hpp"
#include "kiarabot/core/domain/target/identity_state_rules.hpp"
#include "kiarabot/infrastructure/identity_crypto/delegation_crypto.hpp"
#include "kiarabot/infrastructure/identity_crypto/factories.hpp"
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"
#include "kiarabot/infrastructure/identity_crypto/principal_fingerprint.hpp"
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
        {1}},
       {{"40000000-0000-4000-8000-000000000003"},
        control,
        {},
        {{t::Permission::IdentityPrincipalManage,
          t::ResourceRef{t::ResourceKind::Adapter, {"60000000-0000-4000-8000-000000000003"}}},
         {t::Permission::IdentityPrincipalRead,
          t::ResourceRef{t::ResourceKind::Adapter, {"60000000-0000-4000-8000-000000000003"}}}},
        {1}},
       {{"40000000-0000-4000-8000-000000000004"},
        control,
        {},
        {{t::Permission::IdentityPrincipalManage,
          t::ResourceRef{t::ResourceKind::Principal, {user.value}}},
         {t::Permission::IdentityPrincipalRead,
          t::ResourceRef{t::ResourceKind::Principal, {user.value}}}},
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
  phase = "durable invalidation stream";
  auto restarted_admin = std::make_unique<t::IdentityAdminApplication>(*factory, *certificates);
  const auto restarted_peer = take(restarted_admin->authenticate_peer(
      {tls.registration.fingerprint, context(clock, "peer-invalidation").deadline}, tls.leaf_der));
  const t::ResourceRef invalidation_scope{t::ResourceKind::Model,
                                          {"70000000-0000-4000-8000-000000000001"}};
  const t::AssignmentId invalidation_grant{"70000000-0000-4000-8000-000000000002"};
  {
    auto work = take(factory->begin());
    t::AssignmentStateRecord grant;
    grant.assignment.id = invalidation_grant;
    grant.assignment.subject = control;
    grant.assignment.version = {1};
    grant.assignment.explicit_permissions.push_back(
        {t::Permission::IdentityInvalidationRead, invalidation_scope});
    grant.state = t::AssignmentState::Active;
    take(work->exact_assignments().save_state(grant, std::nullopt));
    check(take(work->authorizations().find(control)).has_value(),
          "missing authorization state for invalidation grant");
    take(work->authorizations().save({control, {2}, true}, std::optional<t::ResourceVersion>{1}));
    take(t::append_identity_invalidation(
        *work, *random, {control, {2}, {invalidation_scope}, false}, take(work->current_time())));
    check(work->commit() == t::CommitResult::Committed,
          "invalidation/grant transaction did not commit");
  }
  auto invalidations_app =
      take(t::make_identity_invalidation_application(*factory, *random, {.batch = 8}));
  const auto snapshot = take(invalidations_app->snapshot(
      {context(clock, "snapshot-invalidation"), restarted_peer, {invalidation_scope}, {}}));
  check(snapshot.assignments.size() == 1 && snapshot.authorization_version.value == 2 &&
            snapshot.cursor.source_service == t::kInvalidationSourceService &&
            !snapshot.cursor.epoch.empty() && snapshot.cursor.sequence == 1,
        "real snapshot lost committed cursor/authorization evidence");
  const auto initial_watch = take(invalidations_app->open_watch(
      {context(clock, "watch-invalidation"), restarted_peer, {invalidation_scope}, {}}));
  check(initial_watch.resync_required && initial_watch.watermark.epoch == snapshot.cursor.epoch,
        "initial watch did not request a full resync");
  {
    auto work = take(factory->begin());
    take(work->authorizations().save({control, {3}, true}, std::optional<t::ResourceVersion>{2}));
    take(t::append_identity_invalidation(
        *work, *random, {control, {3}, {invalidation_scope}, false}, take(work->current_time())));
    check(work->commit() == t::CommitResult::Committed, "second invalidation did not commit");
  }
  const auto polled = take(invalidations_app->poll({context(clock, "poll-invalidation"),
                                                    restarted_peer,
                                                    {invalidation_scope},
                                                    snapshot.cursor.epoch,
                                                    snapshot.cursor.sequence,
                                                    8}));
  check(!polled.resync_required && polled.records.size() == 1 &&
            polled.records.front().sequence == 2 &&
            polled.records.front().event.authorization_version.value == 3,
        "ordered committed invalidation was not streamed after the cursor");
  // Durable restart keeps the SAME epoch and sequence: continuity is real, not fabricated.
  invalidations_app.reset();
  restarted_admin.reset();
  factory.reset();
  factory = take(pg::open_identity_security(config));
  auto restarted_work = take(factory->begin());
  const auto state = take(restarted_work->invalidations()->state());
  check(state.has_value() && state->epoch == snapshot.cursor.epoch && state->next_sequence == 2,
        "invalidation epoch/sequence did not survive a real restart");
  const auto retained =
      take(restarted_work->invalidations()->read_after(snapshot.cursor.epoch, 0, 8));
  check(retained.size() == 2 && retained.front().sequence == 1 && retained.back().sequence == 2,
        "durable invalidation log lost ordered records across restart");
  restarted_work->rollback();
  phase = "real online authorization";
  pg::test::identity_authorization_checks(*factory, *delegations, clock, user, user_credential,
                                          refreshed.tokens.session, control);
  phase = "principal credential management";
  {
    // The online-authorization fixture deliberately disables Control at its end; this capability
    // runs as the same real Control peer, so the test restores that authoritative principal.
    auto work = take(factory->begin());
    auto restored = take(work->principals().find(control));
    check(restored.has_value(), "fixture Control principal missing");
    const auto previous = restored->version;
    ++restored->version.value;
    restored->state = t::LifecycleState::Active;
    take(work->principals().save(*restored, previous));
    check(work->commit() == t::CommitResult::Committed, "fixture Control reactivation failed");
  }
  admin = std::make_unique<t::IdentityAdminApplication>(*factory, *certificates);
  const t::IdentitySecurityOptions replay_options{{"token-key"},
                                                  {{"token-key"}},
                                                  {"request-key"},
                                                  {"receipt-key"},
                                                  dummy,
                                                  std::chrono::hours{24},
                                                  {100},
                                                  10,
                                                  10,
                                                  100};
  app = take(t::make_identity_security_application(
      {*factory, *passwords, *tokens, *receipts, *random, clock, *delegations}, replay_options));
  auto fingerprints = take(crypto::make_principal_fingerprints());
  auto principal_app = take(t::make_identity_principal_application(
      {*factory, *tokens, *passwords, *receipts, *random, *fingerprints},
      {.token_key = {"token-key"},
       .request_commitment_key = {"request-key"},
       .receipt_encryption_key = {"receipt-key"},
       .bootstrap_credential_lifetime = std::chrono::hours{24},
       .password_credential_lifetime = std::chrono::hours{365 * 24},
       .secret_receipts = {100},
       .maximum_allowed_services = 16}));
  const t::ResourceRef workload_binding{t::ResourceKind::Adapter,
                                        {"60000000-0000-4000-8000-000000000003"}};
  auto registered = take(principal_app->register_principal({context(clock, "principal-register"),
                                                            peer(),
                                                            t::PrincipalKind::Adapter,
                                                            workload_binding,
                                                            {t::Service::Runtime}}));
  check(registered.principal.state == t::LifecycleState::Disabled &&
            registered.initial_credential.state == t::CredentialStatusState::Pending &&
            !registered.initial_credential.expires_at,
        "registration did not create a disabled/pending principal");
  {
    // Pending allowed_services must survive a real PG round trip before issuance.
    auto work = take(factory->begin());
    auto stored = take(work->credentials().current(registered.initial_credential.ref.id));
    check(stored.has_value(), "registered pending credential disappeared");
    const auto* pending = std::get_if<t::PendingCredentialMaterial>(&stored->material);
    check(pending && pending->audiences == std::vector<t::Service>{t::Service::Runtime},
          "pending credential audiences did not round trip");
    work->rollback();
  }
  auto workload_delivery = take(principal_app->rotate_credential(
      {context(clock, "principal-rotate"), peer(), registered.initial_credential.ref.id, {1}}));
  check(workload_delivery.credential.version.value == 2 && workload_delivery.secret &&
            !workload_delivery.secret->bytes().empty(),
        "workload credential rotation failed");
  auto workload_replay = take(principal_app->rotate_credential(
      {context(clock, "principal-rotate"), peer(), registered.initial_credential.ref.id, {1}}));
  check(workload_replay.secret &&
            std::ranges::equal(workload_delivery.secret->bytes(), workload_replay.secret->bytes()),
        "workload delivery replay did not decrypt original secret");
  {
    auto work = take(factory->begin());
    auto principal_row = take(work->principals().find(registered.principal.id));
    check(principal_row.has_value() && principal_row->state == t::LifecycleState::Active &&
              principal_row->version.value == 2,
          "first rotation did not activate the principal");
    work->rollback();
  }
  take(principal_app->revoke_credential(
      {context(clock, "principal-revoke"), peer(), registered.initial_credential.ref.id, {2}}));
  {
    auto work = take(factory->begin());
    auto revoked = take(work->credentials().current(registered.initial_credential.ref.id));
    check(revoked.has_value() && revoked->state == t::CredentialState::Revoked,
          "workload credential revoke did not persist");
    auto owner = take(work->principals().find(registered.principal.id));
    check(owner.has_value(), "workload principal disappeared");
    const auto gate =
        t::require_current_credential(*owner, *revoked, revoked->ref, take(work->current_time()));
    check(std::holds_alternative<t::Failure>(gate) &&
              std::get<t::Failure>(gate).code == t::ErrorCode::Unauthenticated,
          "revoked workload credential still passes current-credential gate");
    work->rollback();
  }
  phase = "human password rotation and revoke";
  auto human_delivery = take(principal_app->rotate_credential(
      {context(clock, "principal-rotate-human"), peer(), user_credential, {1}}));
  check(human_delivery.credential.version.value == 2 && human_delivery.secret &&
            human_delivery.secret->bytes().size() >= 32,
        "human password rotation did not return a one-time server password");
  auto rotated_login = take(app->login(
      {context(clock, "login-rotated-password"), peer(), "alice", human_delivery.secret->bytes()}));
  check(rotated_login.tokens.session.value.size() > 0, "login with rotated human password failed");
  expect(app->login({context(clock, "login-old-password"), peer(), "alice", password->bytes()}),
         t::ErrorCode::Unauthenticated);
  take(principal_app->revoke_credential(
      {context(clock, "principal-revoke-human"), peer(), user_credential, {2}}));
  expect(app->login({context(clock, "login-revoked-password"), peer(), "alice",
                     human_delivery.secret->bytes()}),
         t::ErrorCode::Unauthenticated);

  phase = "principal directory listing";
  // Seed a second human plus the exact grants that make both principals visible to the real
  // Control peer without granting a wildcard: Control gets identity.principal.read on the second
  // principal resource, and the second human gets a visible ViewRole on itself.
  const t::SubjectId second_human{"20000000-0000-4000-8000-000000000009"};
  const t::ResourceRef second_resource{t::ResourceKind::Principal, {second_human.value}};
  {
    auto work = take(factory->begin());
    take(work->principals().save(
        t::Principal{
            second_human, t::PrincipalKind::Human, second_resource, {1}, t::LifecycleState::Active},
        std::nullopt));
    take(work->exact_assignments().save(
        t::ExactAssignment{{"40000000-0000-4000-8000-000000000005"},
                           control,
                           {},
                           {{t::Permission::IdentityPrincipalRead, second_resource}},
                           {1}},
        std::nullopt));
    take(
        work->exact_assignments().save(t::ExactAssignment{{"40000000-0000-4000-8000-000000000006"},
                                                          second_human,
                                                          {{t::FixedRole::Viewer, second_resource}},
                                                          {},
                                                          {1}},
                                       std::nullopt));
    check(work->commit() == t::CommitResult::Committed, "directory seed commit failed");
  }
  auto directory = take(t::make_identity_principal_directory_application(*factory));
  auto first_page = take(directory->list_principals(
      {context(clock, "principal-directory-first"), peer(), 1, "", std::nullopt}));
  check(first_page.principals.size() == 1 && first_page.page_size == 1 &&
            first_page.checked_at > t::TimePoint{} && !first_page.next_page_token.empty(),
        "principal directory first page malformed");
  check(first_page.principals.front().principal.kind == t::PrincipalKind::Human,
        "principal directory returned a non-human principal");
  auto second_page =
      take(directory->list_principals({context(clock, "principal-directory-second"), peer(), 1,
                                       first_page.next_page_token, std::nullopt}));
  check(second_page.principals.size() == 1 &&
            second_page.principals.front().principal.id.value == second_human.value &&
            second_page.principals.front().roles ==
                std::vector<t::FixedRole>{t::FixedRole::Viewer} &&
            second_page.next_page_token.empty(),
        "principal directory keyset page/roles projection mismatch");
  auto empty_page = take(directory->list_principals(
      {context(clock, "principal-directory-empty"), peer(), 20, "", t::LifecycleState::Disabled}));
  check(empty_page.principals.empty() && empty_page.checked_at > t::TimePoint{} &&
            empty_page.next_page_token.empty(),
        "principal directory explicit disabled filter must be an empty authoritative page");
  expect(directory->list_principals(
             {context(clock, "principal-directory-invalid"), peer(), 101, "", std::nullopt}),
         t::ErrorCode::InvalidArgument);
  expect(directory->list_principals({context(clock, "principal-directory-token"), peer(), 1,
                                     "not-a-cursor", std::nullopt}),
         t::ErrorCode::InvalidArgument);
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
