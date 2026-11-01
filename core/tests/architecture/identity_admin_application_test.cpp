#include <cstdlib>
#include <functional>
#include <iostream>
#include <string_view>

#include "kiarabot/core/application/target/identity_admin_application.hpp"
#include "kiarabot/core/application/target/identity_delegation_application.hpp"
#include "kiarabot/core/domain/target/persistence_rules.hpp"

namespace t = kiarabot::core::target;
using std::chrono_literals::operator""s;
using std::chrono_literals::operator""h;
using std::chrono_literals::operator""min;
void check(bool value, std::string_view name) {
  if (!value) {
    std::cerr << name << '\n';
    std::exit(1);
  }
}
t::Failure unavailable() {
  return t::rejected(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
}
template <typename Value> bool error(const t::Outcome<Value>& result, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&result);
  return failure && failure->code == code;
}
// Scripted port callbacks, not a second database. Unexpected repository access fails loudly.
template <typename Key, typename Value> class ScriptRepository : public t::Repository<Key, Value> {
public:
  std::function<t::Outcome<std::optional<Value>>(const Key&)> read;
  std::function<t::Status(const Value&, std::optional<t::ResourceVersion>)> write;
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    check(static_cast<bool>(read), "unexpected repository read");
    return read(key);
  }
  t::Status save(const Value& value, std::optional<t::ResourceVersion> expected) override {
    check(static_cast<bool>(write), "unexpected repository write");
    return write(value, expected);
  }
};
class Credentials : public t::IdentityCredentialRepository {
public:
  std::function<t::Outcome<std::optional<t::CredentialVersionRecord>>()> lookup;
  std::function<t::Outcome<std::optional<t::CredentialVersionRecord>>(const t::CredentialId&)>
      lookup_id;
  std::function<t::Outcome<std::optional<t::CredentialVersionRecord>>()> lookup_certificate;
  std::function<t::Status(const t::CredentialVersionRecord&)> write;
  std::function<t::Status(const t::LoginIdentity&)> login_write;
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    if (lookup_id) {
      return lookup_id(id);
    }
    check(static_cast<bool>(lookup), "unexpected credential current");
    return lookup();
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_bootstrap(const t::TokenVerifier&) override {
    check(false, "unexpected bootstrap lookup");
    return unavailable();
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest&) override {
    if (lookup_certificate) {
      return lookup_certificate();
    }
    check(static_cast<bool>(lookup), "unexpected certificate lookup");
    return lookup();
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  version(const t::CredentialVersionRef&) override {
    check(false, "unexpected version");
    return unavailable();
  }
  t::Outcome<std::optional<t::LoginIdentity>> login(const std::string&) override {
    check(false, "unexpected login");
    return unavailable();
  }
  t::Status save(const t::CredentialVersionRecord& row,
                 std::optional<t::ResourceVersion> expected) override {
    check(!expected && static_cast<bool>(write), "credential create only");
    return write(row);
  }
  t::Status save_login(const t::LoginIdentity& row,
                       std::optional<t::ResourceVersion> expected) override {
    check(!expected && static_cast<bool>(login_write), "login create only");
    return login_write(row);
  }
};
class Assignments : public t::IdentityExactAssignmentRepository {
public:
  std::function<t::Status(const t::ExactAssignment&)> write;
  std::function<t::Outcome<std::vector<t::ExactAssignment>>()> read;
  std::function<t::Outcome<std::vector<t::ExactAssignment>>(const t::SubjectId&)> read_subject;
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& id) override {
    if (read_subject) {
      return read_subject(id);
    }
    check(static_cast<bool>(read), "unexpected grants query");
    return read();
  }
  t::Status save(const t::ExactAssignment& row,
                 std::optional<t::ResourceVersion> expected) override {
    check(!expected && static_cast<bool>(write), "grant create only");
    return write(row);
  }
};
class Bootstrap : public t::IdentityBootstrapRepository {
public:
  t::AdministratorBootstrap marker{t::BootstrapState::Required, {1}, std::nullopt, std::nullopt};
  int consumed = 0;
  t::Outcome<t::AdministratorBootstrap> state() override {
    return marker;
  }
  t::Status consume(const t::AdministratorBootstrap& row, t::ResourceVersion expected) override {
    check(row.state == t::BootstrapState::Consumed && expected.value == 1, "conditional marker");
    ++consumed;
    return std::monostate{};
  }
};
// Unused interfaces are never invoked. Returning a null reference would mask accidental access;
// instead terminate the test before any such access can occur.
template <typename Type> Type& unexpected() {
  std::abort();
}
class Sessions : public t::IdentitySessionSecurityRepository {
public:
  std::function<t::Outcome<std::optional<t::SessionSecurity>>(const t::SessionId&)> read;
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId& id) override {
    check(static_cast<bool>(read), "unexpected security session query");
    return read(id);
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_access(const t::TokenVerifier&) override {
    std::abort();
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_refresh(const t::TokenVerifier&) override {
    std::abort();
  }
  t::Outcome<std::optional<t::RefreshHandle>>
  resolve_refresh(const t::TokenVerifier&, const t::RefreshRecoveryLookup&, t::TimePoint) override {
    std::abort();
  }
  t::Status record_refresh_consumption(const t::ConsumedRefreshHandle&) override {
    std::abort();
  }
  t::Outcome<std::vector<t::SessionMetadata>> list_metadata(const t::SubjectId&,
                                                            std::size_t) override {
    std::abort();
  }
  t::Status save(const t::SessionSecurity&, std::optional<t::ResourceVersion>,
                 std::optional<std::uint64_t>) override {
    std::abort();
  }
  t::Outcome<std::size_t> count_unexpired() override {
    std::abort();
  }
};
struct Script {
  ScriptRepository<t::SubjectId, t::Principal> principals;
  ScriptRepository<t::SubjectId, t::AuthorizationState> authorizations;
  Credentials credentials;
  Assignments assignments;
  Sessions sessions;
  Bootstrap bootstrap;
  t::CommitResult commit = t::CommitResult::Committed;
  t::TimePoint now{100s};
  std::function<t::Outcome<t::TimePoint>()> sample_time;
  int active = 0;
  int commits = 0;
  std::function<t::Status(const t::AuditEntry&)> audit;
};
class Journal : public t::JournalRepository {
public:
  explicit Journal(Script& script) : script_(script) {}
  t::Status append_audit(const t::AuditEntry& row) override {
    check(static_cast<bool>(script_.audit), "unexpected audit");
    return script_.audit(row);
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    std::abort();
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    std::abort();
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    std::abort();
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    std::abort();
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    std::abort();
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    std::abort();
  }

private:
  Script& script_;
};
class Work : public t::IdentitySecurityUnitOfWork {
public:
  explicit Work(Script& script) : script_(script), journal_(script) {
    ++script_.active;
  }
  ~Work() override {
    if (active_) {
      --script_.active;
    }
  }
  t::Outcome<t::TimePoint> current_time() override {
    if (script_.sample_time) {
      return script_.sample_time();
    }
    return script_.now;
  }
  t::Repository<t::SubjectId, t::Principal>& principals() override {
    return script_.principals;
  }
  t::Repository<t::SubjectId, t::AuthorizationState>& authorizations() override {
    return script_.authorizations;
  }
  t::IdentityCredentialRepository& credentials() override {
    return script_.credentials;
  }
  t::IdentityExactAssignmentRepository& exact_assignments() override {
    return script_.assignments;
  }
  t::IdentityBootstrapRepository& bootstrap() override {
    return script_.bootstrap;
  }
  t::JournalRepository& journal() override {
    return journal_;
  }
  t::ReceiptRepository& receipts() override {
    return unexpected<t::ReceiptRepository>();
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    return unexpected<t::Repository<t::OperationId, t::Operation>>();
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    return unexpected<t::Repository<t::SessionId, t::LoginSession>>();
  }
  t::Repository<t::AssignmentId, t::ResourceGrant>& grants() override {
    return unexpected<t::Repository<t::AssignmentId, t::ResourceGrant>>();
  }
  t::IdentitySessionSecurityRepository& session_security() override {
    return script_.sessions;
  }
  t::IdentitySecretReceiptRepository& secret_receipts() override {
    return unexpected<t::IdentitySecretReceiptRepository>();
  }
  t::CommitResult commit() override {
    ++script_.commits;
    rollback();
    return script_.commit;
  }
  void rollback() noexcept override {
    if (active_) {
      active_ = false;
      --script_.active;
    }
  }

private:
  Script& script_;
  Journal journal_;
  bool active_ = true;
};
class Factory : public t::IdentitySecurityUnitOfWorkFactory {
public:
  Script script;
  bool fail = false;
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    if (fail) {
      return unavailable();
    }
    return std::unique_ptr<t::IdentitySecurityUnitOfWork>(std::make_unique<Work>(script));
  }
};
class Certificates : public t::PeerCertificatePort {
public:
  explicit Certificates(Script& script) : script_(script) {}
  std::function<void()> after_verify;
  bool fail = false;
  int calls = 0;
  t::Outcome<t::VerifiedPeer> verify(std::span<const std::byte>, const t::RegisteredPeer& peer,
                                     t::TimePoint now) override {
    // Orchestration test ONLY. Mature OpenSSL verification has separate real crypto tests.
    check(script_.active == 0, "no transaction during certificate verification");
    ++calls;
    if (fail) {
      return unavailable();
    }
    if (after_verify) {
      after_verify();
    }
    return t::VerifiedPeer{peer, now};
  }

private:
  Script& script_;
};
t::CredentialVersionRecord certificate(t::TimePoint now) {
  return {
      {{"certificate"}, {1}},
      {"control"},
      t::CertificateBinding{
          {"control"}, {}, "spiffe://kiarabot/control", t::Service::Control, now - 1h, now + 1h},
      t::CredentialState::Active,
      now - 1h,
      std::nullopt,
      {t::ResourceKind::Principal, {"control"}},
      now + 1h};
}
void peer_tests() {
  Factory factory;
  Certificates crypto(factory.script);
  t::IdentityAdminApplication app(factory, crypto);
  auto row = certificate(factory.script.now);
  factory.script.credentials.lookup =
      [&]() -> t::Outcome<std::optional<t::CredentialVersionRecord>> { return std::optional{row}; };
  t::Principal principal{
      {"control"}, t::PrincipalKind::PlatformService, std::nullopt, {1}, t::LifecycleState::Active};
  factory.script.principals.read = [&](const auto&) -> t::Outcome<std::optional<t::Principal>> {
    return std::optional{principal};
  };
  const t::PeerRegistrationRequest request{{}, factory.script.now + 10s};
  check(std::holds_alternative<t::VerifiedPeer>(app.authenticate_peer(request, {})),
        "registered platform peer");
  check(factory.script.active == 0 && factory.script.commits == 0, "all read UoWs released");
  crypto.fail = true;
  check(error(app.authenticate_peer(request, {}), t::ErrorCode::UpstreamUnavailable),
        "crypto dependency failure is explicit");
  crypto.fail = false;
  crypto.after_verify = [&] {
    row.state = t::CredentialState::Revoked;
    row.revoked_at = factory.script.now;
  };
  check(error(app.authenticate_peer(request, {}), t::ErrorCode::Unauthenticated),
        "revoke between read and crypto rejects");
  row = certificate(factory.script.now);
  crypto.after_verify = {};
  principal.state = t::LifecycleState::Disabled;
  check(error(app.registered_peer(request), t::ErrorCode::Unauthenticated), "disabled peer");
  principal.state = t::LifecycleState::Active;
  row.expires_at = factory.script.now;
  check(error(app.registered_peer(request), t::ErrorCode::Unauthenticated),
        "credential expiry exact boundary");
  factory.script.credentials.lookup =
      []() -> t::Outcome<std::optional<t::CredentialVersionRecord>> { return unavailable(); };
  check(error(app.registered_peer(request), t::ErrorCode::UpstreamUnavailable),
        "lookup failure not absence");
  factory.script.credentials.lookup =
      []() -> t::Outcome<std::optional<t::CredentialVersionRecord>> {
    return std::optional<t::CredentialVersionRecord>{};
  };
  check(error(app.registered_peer(request), t::ErrorCode::Unauthenticated),
        "CA alone without registration denies");
  factory.fail = true;
  check(error(app.registered_peer(request), t::ErrorCode::UpstreamUnavailable), "begin failure");
}
t::OfflineIdentityBootstrap bootstrap_request(t::TimePoint now) {
  return {{"request", "trace", "", now + 10s},
          {1},
          {"administrator"},
          {"password"},
          "admin",
          {"prederived-by-mature-adapter", {1}},
          now + 24h,
          {"control"},
          {"certificate"},
          std::get<t::CertificateBinding>(certificate(now).material),
          {{{"admin-assignment"},
            {"administrator"},
            {{t::FixedRole::Admin, {t::ResourceKind::Principal, {"administrator"}}}},
            {},
            {1}}},
          {{{"control-assignment"},
            {"control"},
            {},
            {{t::Permission::IdentitySessionRead, {t::ResourceKind::Principal, {"administrator"}}}},
            {1}}}};
}
void bootstrap_tests() {
  Factory factory;
  Certificates crypto(factory.script);
  t::IdentityAdminApplication app(factory, crypto);
  auto request = bootstrap_request(factory.script.now);
  int principals = 0;
  int credentials = 0;
  int assignments = 0;
  int watermarks = 0;
  int audits = 0;
  bool fail_audit = false;
  factory.script.principals.write = [&](const auto& row, auto expected) -> t::Status {
    check(!expected && factory.script.active == 1, "same UoW creates principal");
    check(row.binding == t::ResourceRef{t::ResourceKind::Principal, {row.id.value}},
          "bootstrap persists explicit self binding for both initial principals");
    check((row.id == request.administrator && row.kind == t::PrincipalKind::Human) ||
              (row.id == request.control && row.kind == t::PrincipalKind::PlatformService),
          "bootstrap binds each initial subject with its actual kind");
    ++principals;
    return std::monostate{};
  };
  factory.script.credentials.write = [&](const auto& row) -> t::Status {
    check(row.state == t::CredentialState::Active && row.expires_at.has_value(),
          "finite active credential");
    ++credentials;
    return std::monostate{};
  };
  factory.script.credentials.login_write = [&](const auto& row) -> t::Status {
    check(row.subject == request.administrator &&
              row.password_credential == request.password_credential,
          "login references exact admin credential");
    return std::monostate{};
  };
  factory.script.assignments.write = [&](const auto&) -> t::Status {
    ++assignments;
    return std::monostate{};
  };
  factory.script.authorizations.write = [&](const auto& row, auto expected) -> t::Status {
    check(!expected && row.version.value == 1 && row.invalidation_dirty,
          "initial authority watermark");
    ++watermarks;
    return std::monostate{};
  };
  factory.script.audit = [&](const auto& row) -> t::Status {
    ++audits;
    check(row.actor == request.administrator && row.method == "BootstrapAdministrator",
          "audit identity");
    if (fail_audit) {
      return unavailable();
    }
    return std::monostate{};
  };
  check(std::holds_alternative<t::IdentityBootstrapReply>(app.bootstrap_offline(request)),
        "bootstrap committed");
  check(principals == 2 && credentials == 2 && assignments == 2 && watermarks == 2 && audits == 1 &&
            factory.script.bootstrap.consumed == 1 && factory.script.commits == 1 &&
            factory.script.active == 0,
        "all bootstrap writes share one commit");
  factory.script.bootstrap.marker.state = t::BootstrapState::Consumed;
  check(error(app.bootstrap_offline(request), t::ErrorCode::Conflict),
        "restart cannot reopen marker");
  check(principals == 2 && factory.script.commits == 1, "consumed marker no writes");
  factory.script.bootstrap.marker.state = t::BootstrapState::Required;
  fail_audit = true;
  check(error(app.bootstrap_offline(request), t::ErrorCode::UpstreamUnavailable),
        "audit failure aborts bootstrap");
  check(factory.script.bootstrap.consumed == 1 && factory.script.commits == 1 &&
            factory.script.active == 0,
        "failure cannot consume marker or commit");
  fail_audit = false;
  factory.script.commit = t::CommitResult::Unknown;
  const auto unknown = app.bootstrap_offline(request);
  const auto* failure = std::get_if<t::Failure>(&unknown);
  check(failure && failure->acceptance == t::Acceptance::Unknown &&
            failure->recovery == t::Recovery::QuerySameIdentity,
        "unknown never success or automatic retry");
  const auto old_commits = factory.script.commits;
  request.context.deadline = factory.script.now;
  check(error(app.bootstrap_offline(request), t::ErrorCode::Timeout), "deadline exact cutoff");
  check(factory.script.commits == old_commits, "expired request cannot commit");
  request = bootstrap_request(factory.script.now);
  request.control_assignments.front().roles = request.administrator_assignments.front().roles;
  check(error(app.bootstrap_offline(request), t::ErrorCode::InvalidArgument),
        "Control cannot inherit human role");
}
void delegation_tests();
int main() {
  peer_tests();
  bootstrap_tests();
  delegation_tests();
}

// Deliberately inert byte owners for orchestration tests, not a production crypto implementation.
class TestSecret : public t::SecretBuffer {
public:
  explicit TestSecret(std::size_t count) : bytes_(count, std::byte{1}) {}
  std::span<const std::byte> bytes() const noexcept override {
    return bytes_;
  }

private:
  std::vector<std::byte> bytes_;
};
class Random : public t::RandomPort {
public:
  explicit Random(Script& script) : script_(script) {}
  t::Outcome<t::OwnedSecret> bytes(std::size_t count) override {
    check(script_.active == 0, "random outside UoW");
    return t::OwnedSecret{std::make_unique<TestSecret>(count)};
  }
  t::Outcome<t::OwnedSecret> opaque_token() override {
    std::abort();
  }

private:
  Script& script_;
};
class Clock : public t::SecurityClockPort {
public:
  t::TimePoint now;
  bool trusted = true;
  explicit Clock(t::TimePoint value) : now(value) {}
  t::Outcome<t::SecurityClockSample> sample() override {
    return t::SecurityClockSample{now, {}, 1s, trusted};
  }
};
class Delegations : public t::DelegationCryptoPort {
public:
  explicit Delegations(Script& script) : script_(script) {}
  std::function<void()> while_signing;
  bool fail = false;
  int calls = 0;
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims&, const t::CryptoKeyRef&) override {
    check(script_.active == 0, "sign outside UoW");
    ++calls;
    if (fail) {
      return unavailable();
    }
    if (while_signing) {
      while_signing();
    }
    return t::OwnedSecret{std::make_unique<TestSecret>(16)};
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView, t::Service, const t::VerifiedPeer&,
                                           const t::SecurityClockSample&) override {
    std::abort();
  }
  t::Outcome<t::VerifiedDelegation> verify_exchange_parent(t::SecretView, const t::VerifiedPeer&,
                                                           const t::SecurityClockSample&) override {
    std::abort();
  }

private:
  Script& script_;
};
class SessionApplication : public t::IdentitySecurityApplication {
public:
  t::ValidatedSession current;
  bool rejected = false;
  int calls = 0;
  explicit SessionApplication(t::ValidatedSession value) : current(std::move(value)) {}
  t::ServiceReadiness readiness() const override {
    return {false, {}};
  }
  t::Outcome<t::ValidatedSession> validate_session(const t::AccessSessionRequest&) override {
    ++calls;
    if (rejected) {
      return t::rejected(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
    }
    return current;
  }
  t::Outcome<t::IdentityTokenReply> login(const t::LoginRequest&) override {
    std::abort();
  }
  t::Outcome<t::IdentityTokenReply> refresh_session(const t::RefreshSessionRequest&) override {
    std::abort();
  }
  t::Outcome<t::CurrentPrincipal> get_principal(const t::AccessSessionRequest&) override {
    std::abort();
  }
  t::Outcome<t::RevokeSessionReply> revoke_session(const t::RevokeSessionRequest&) override {
    std::abort();
  }
  t::Outcome<t::SessionPageReply> list_sessions(const t::ListSessionsRequest&) override {
    std::abort();
  }
};
void delegation_tests() {
  Factory factory;
  const auto now = factory.script.now;
  const t::Principal principal{
      {"human"}, t::PrincipalKind::Human, std::nullopt, {1}, t::LifecycleState::Active};
  const t::SessionSecurity session{{"session"},
                                   principal.id,
                                   {{"password"}, {1}},
                                   {t::TokenPurpose::Access, {"token-key"}, {}},
                                   {t::TokenPurpose::Refresh, {"token-key"}, {}},
                                   now - 1s,
                                   now + 15min,
                                   now + 24h,
                                   1,
                                   {1},
                                   false};
  const t::SessionMetadata metadata{
      session.id, session.subject, session.created_at, session.refresh_expires_at, 1, {1}, false};
  SessionApplication sessions({principal, metadata, {}, {1}, now});
  Delegations crypto(factory.script);
  Random random(factory.script);
  Clock clock(now);
  t::IdentityDelegationApplication app(sessions, factory, crypto, random, clock,
                                       {"kiarabot-identity", {"signing-key"}});
  factory.script.sessions.read = [&](const auto&) -> t::Outcome<std::optional<t::SessionSecurity>> {
    return std::optional{session};
  };
  factory.script.principals.read = [&](const auto&) -> t::Outcome<std::optional<t::Principal>> {
    return std::optional{principal};
  };
  auto credential = t::CredentialVersionRecord{{{"password"}, {1}},
                                               principal.id,
                                               t::PasswordVerifier{"prederived", {1}},
                                               t::CredentialState::Active,
                                               now - 1s,
                                               std::nullopt,
                                               {t::ResourceKind::Principal, {"human"}},
                                               now + 24h};
  factory.script.credentials.lookup =
      [&]() -> t::Outcome<std::optional<t::CredentialVersionRecord>> {
    return std::optional{credential};
  };
  factory.script.authorizations.read =
      [&](const auto&) -> t::Outcome<std::optional<t::AuthorizationState>> {
    return std::optional{t::AuthorizationState{principal.id, {1}, false}};
  };
  factory.script.assignments.read = []() -> t::Outcome<std::vector<t::ExactAssignment>> {
    return std::vector<t::ExactAssignment>{};
  };
  const auto binding = std::get<t::CertificateBinding>(certificate(now).material);
  const t::VerifiedPeer peer{
      {t::PlatformPeer{t::Service::Control, {"control"}}, binding, {{"certificate"}, {1}}}, now};
  t::IssueIdentityDelegationRequest request{
      {{"request", "trace", "", now + 10s}, peer, {}},
      t::Service::Identity,
      {{t::Permission::IdentitySessionRead, {t::ResourceKind::Principal, {"human"}}}}};
  auto control_principal = t::Principal{{"control"},
                                        t::PrincipalKind::PlatformService,
                                        t::ResourceRef{t::ResourceKind::Principal, {"control"}},
                                        {1},
                                        t::LifecycleState::Active};
  auto control_credential = certificate(now);
  t::ResourceVersion control_version{1};
  std::vector<t::ExactAssignment> control_grants{
      {{"control-delegation"},
       control_principal.id,
       {},
       {{t::Permission::IdentityDelegationIssue,
         {t::ResourceKind::Principal, {control_principal.id.value}}}},
       {1}}};
  const auto expected_control_grants = control_grants;
  factory.script.principals.read = [&](const auto& id) -> t::Outcome<std::optional<t::Principal>> {
    return std::optional{id == control_principal.id ? control_principal : principal};
  };
  factory.script.credentials.lookup_id =
      [&](const auto& id) -> t::Outcome<std::optional<t::CredentialVersionRecord>> {
    return std::optional{id == control_credential.ref.id ? control_credential : credential};
  };
  factory.script.credentials.lookup_certificate =
      [&]() -> t::Outcome<std::optional<t::CredentialVersionRecord>> {
    return std::optional{control_credential};
  };
  factory.script.authorizations.read =
      [&](const auto& id) -> t::Outcome<std::optional<t::AuthorizationState>> {
    return std::optional{t::AuthorizationState{
        id, id == control_principal.id ? control_version : t::ResourceVersion{1}, false}};
  };
  factory.script.assignments.read_subject =
      [&](const auto& id) -> t::Outcome<std::vector<t::ExactAssignment>> {
    return id == control_principal.id ? t::Outcome<std::vector<t::ExactAssignment>>{control_grants}
                                      : factory.script.assignments.read();
  };
  auto result = app.issue_delegation(request);
  check(std::holds_alternative<t::IdentityDelegationReply>(result), "self list delegation signs");
  const auto& reply = std::get<t::IdentityDelegationReply>(result);
  check(reply.claims.business_hops == 1 && reply.claims.actor_chain.size() == 1 &&
            reply.claims.direct_actor.value == "control" &&
            reply.claims.source_session == session.id &&
            reply.claims.source_credential == session.credential &&
            reply.claims.expires_at == now + 56s && reply.claims.proof_id.value.size() == 36,
        "bounded proof carries source and first hop");
  check(sessions.calls == 2 && factory.script.active == 0 && factory.script.commits == 0,
        "source revalidated after signing no persistence");
  clock.now = now + std::chrono::nanoseconds{731};
  credential.expires_at = now + 34s + std::chrono::nanoseconds{987};
  auto fractional = app.issue_delegation(request);
  check(std::holds_alternative<t::IdentityDelegationReply>(fractional),
        "fractional real clock remains usable for integral JWT NumericDate");
  const auto& rounded = std::get<t::IdentityDelegationReply>(fractional);
  check(rounded.claims.issued_at == now && rounded.claims.not_before == now &&
            rounded.claims.expires_at == now + 34s &&
            rounded.claims.expires_at <= *credential.expires_at && rounded.checked_at == now,
        "NumericDate rounding shortens source expiry and preserves actual checked time");
  clock.now = now;
  credential.expires_at = now + 24h;
  auto bounded_request = request;
  bounded_request.permissions.clear();
  t::ExactAssignment bulk{{"bulk"}, principal.id, {}, {}, {1}};
  for (std::size_t index = 0; index <= t::kMaximumDelegationBindings; ++index) {
    auto suffix = std::to_string(index);
    suffix.insert(0, 12 - suffix.size(), '0');
    const t::PermissionBinding permission{
        t::Permission::ResourceRead,
        {t::ResourceKind::Account, {"00000000-0000-4000-8000-" + suffix}}};
    bounded_request.permissions.push_back(permission);
    bulk.explicit_permissions.push_back(permission);
  }
  const auto calls_before_bound = sessions.calls;
  const auto signs_before_bound = crypto.calls;
  check(error(app.issue_delegation(bounded_request), t::ErrorCode::InvalidArgument) &&
            sessions.calls == calls_before_bound && crypto.calls == signs_before_bound &&
            factory.script.active == 0,
        "129 delegation tuples reject before current-state reads or signing");
  bounded_request.permissions.pop_back();
  bulk.explicit_permissions.pop_back();
  factory.script.assignments.read = [bulk]() -> t::Outcome<std::vector<t::ExactAssignment>> {
    return std::vector{bulk};
  };
  auto at_bound = app.issue_delegation(bounded_request);
  check(std::holds_alternative<t::IdentityDelegationReply>(at_bound) &&
            std::get<t::IdentityDelegationReply>(at_bound).claims.permissions.size() ==
                t::kMaximumDelegationBindings,
        "128 explicitly authorized tuples remain usable at the cryptographic limit");
  factory.script.assignments.read = []() -> t::Outcome<std::vector<t::ExactAssignment>> {
    return std::vector<t::ExactAssignment>{};
  };
  const auto never_signs = [&](t::ErrorCode code, std::string_view reason) {
    const auto signed_before = crypto.calls;
    check(error(app.issue_delegation(request), code) && crypto.calls == signed_before &&
              factory.script.active == 0,
          reason);
  };
  control_grants.clear();
  never_signs(t::ErrorCode::PermissionDenied, "registered Control needs explicit delegation cap");
  control_grants = {{{"role-only"},
                     control_principal.id,
                     {{t::FixedRole::Admin, {t::ResourceKind::Principal, {"control"}}}},
                     {},
                     {1}}};
  never_signs(t::ErrorCode::PermissionDenied, "admin role cannot imply delegation cap");
  control_grants = expected_control_grants;
  control_grants.front().explicit_permissions.front().resource.id.value = "human";
  never_signs(t::ErrorCode::PermissionDenied, "wrong Principal cannot grant Control self cap");
  control_grants.front().explicit_permissions.front().resource = {t::ResourceKind::Catalog,
                                                                  {"control"}};
  never_signs(t::ErrorCode::PermissionDenied, "Catalog cannot substitute own Principal capability");
  control_grants = expected_control_grants;
  control_credential.state = t::CredentialState::Revoked;
  control_credential.revoked_at = now;
  never_signs(t::ErrorCode::Unauthenticated, "revoked current Control credential never signs");
  control_credential = certificate(now);
  auto changed_after_sign = [&](auto change, t::ErrorCode code, std::string_view reason) {
    const auto signed_before = crypto.calls;
    crypto.while_signing = change;
    check(error(app.issue_delegation(request), code) && crypto.calls == signed_before + 1 &&
              factory.script.active == 0,
          reason);
    crypto.while_signing = {};
    control_grants = expected_control_grants;
    control_version = {1};
    control_credential = certificate(now);
    control_principal.state = t::LifecycleState::Active;
  };
  changed_after_sign([&] { control_grants.clear(); }, t::ErrorCode::PermissionDenied,
                     "Control grant revoked while signing rejects generated proof");
  changed_after_sign([&] { ++control_version.value; }, t::ErrorCode::Conflict,
                     "Control grant version changed while signing rejects generated proof");
  changed_after_sign([&] { ++control_credential.ref.version.value; }, t::ErrorCode::Unauthenticated,
                     "Control credential rotated while signing rejects generated proof");
  changed_after_sign([&] { control_principal.state = t::LifecycleState::Disabled; },
                     t::ErrorCode::Unauthenticated,
                     "Control disabled while signing rejects generated proof");
  changed_after_sign([&] { control_credential.expires_at = now; }, t::ErrorCode::Unauthenticated,
                     "Control expires while signing rejects generated proof");
  changed_after_sign(
      [&] {
        std::get<t::CertificateBinding>(control_credential.material).registered_san = "other";
      },
      t::ErrorCode::Unauthenticated, "Control SAN binding changes while signing rejects proof");
  int time_reads = 0;
  factory.script.sample_time = [&]() -> t::Outcome<t::TimePoint> {
    return now + (++time_reads == 1 ? 0s : 2s);
  };
  never_signs(t::ErrorCode::Timeout, "pre-sign authority checkpoint exceeding one second rejects");
  factory.script.sample_time = {};
  time_reads = 0;
  factory.script.sample_time = [&]() -> t::Outcome<t::TimePoint> {
    return now + (++time_reads == 4 ? 2s : 0s);
  };
  const auto post_checkpoint_signs = crypto.calls;
  check(error(app.issue_delegation(request), t::ErrorCode::Timeout) &&
            crypto.calls == post_checkpoint_signs + 1 && factory.script.active == 0,
        "post-sign authority checkpoint exceeding one second rejects generated proof");
  factory.script.sample_time = {};
  time_reads = 0;
  factory.script.sample_time = [&]() -> t::Outcome<t::TimePoint> {
    return now - (++time_reads > 2 ? 1s : 0s);
  };
  const auto backwards_signs = crypto.calls;
  check(error(app.issue_delegation(request), t::ErrorCode::UpstreamUnavailable) &&
            crypto.calls == backwards_signs + 1 && factory.script.active == 0,
        "backwards authoritative post-sign time cannot return a proof");
  factory.script.sample_time = {};
  crypto.while_signing = [&] { sessions.rejected = true; };
  check(error(app.issue_delegation(request), t::ErrorCode::Unauthenticated),
        "source revoked during signing rejects proof");
  sessions.rejected = false;
  crypto.while_signing = {};
  crypto.fail = true;
  check(error(app.issue_delegation(request), t::ErrorCode::UpstreamUnavailable),
        "sign failure no proof success");
  crypto.fail = false;
  request.permissions.front().resource.id.value = "other";
  const auto calls = crypto.calls;
  check(error(app.issue_delegation(request), t::ErrorCode::PermissionDenied),
        "foreign session list requires exact grant");
  check(crypto.calls == calls, "denied request never signs");
  request.permissions.front().resource.id.value = "human";
  factory.script.assignments.read = []() -> t::Outcome<std::vector<t::ExactAssignment>> {
    return unavailable();
  };
  check(error(app.issue_delegation(request), t::ErrorCode::UpstreamUnavailable),
        "query failure not empty authorization");
  factory.script.assignments.read = []() -> t::Outcome<std::vector<t::ExactAssignment>> {
    return std::vector<t::ExactAssignment>{};
  };
  credential.state = t::CredentialState::Revoked;
  credential.revoked_at = now;
  check(error(app.issue_delegation(request), t::ErrorCode::Unauthenticated),
        "source credential revoked without watermark change denies");
  credential.state = t::CredentialState::Active;
  credential.revoked_at.reset();
  clock.trusted = false;
  check(error(app.issue_delegation(request), t::ErrorCode::UpstreamUnavailable),
        "untrusted clock cannot issue proof");
}
