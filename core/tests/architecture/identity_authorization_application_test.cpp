// clang-format off
#include "kiarabot/core/application/target/identity_authorization_application.hpp"

#include <cassert>
#include <functional>
#include <stdexcept>
#include <utility>
#include <type_traits>
// clang-format on

namespace {
namespace t = kiarabot::core::target;
t::Failure failure(t::ErrorCode code = t::ErrorCode::Internal) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
struct State {
  t::TimePoint now{std::chrono::seconds{1000}};
  t::SubjectId subject{"model"};
  std::vector<t::Principal> principals;
  std::vector<t::AuthorizationState> authorizations;
  std::vector<t::CredentialVersionRecord> credentials;
  std::vector<t::ExactAssignment> assignments;
  std::optional<t::SessionSecurity> session;
  int active = 0, begins = 0, commits = 0, writes = 0, unexpected = 0;
  bool fail_begin = false, fail_time = false, fail_read = false, ambiguous_platform = false;
  std::function<void()> before_begin;
  std::function<void()> before_time;
  void add_platform(const char* name, t::Service service, unsigned fingerprint) {
    const t::SubjectId id{name};
    const t::ResourceRef binding{t::ResourceKind::Principal, {name}};
    principals.push_back({id, t::PrincipalKind::PlatformService, binding, {1}});
    authorizations.push_back({id, {5}, false});
    t::CertificateBinding certificate{id,
                                      {},
                                      std::string{"spiffe://"} + name,
                                      service,
                                      now - std::chrono::hours{1},
                                      now + std::chrono::hours{24}};
    certificate.fingerprint.bytes[0] = static_cast<std::byte>(fingerprint);
    credentials.push_back({{{std::string{name} + "-credential"}, {1}},
                           id,
                           certificate,
                           t::CredentialState::Active,
                           now - std::chrono::hours{1},
                           {},
                           binding,
                           now + std::chrono::hours{24}});
  }
  State() {
    add_platform("model", t::Service::Model, 1);
    add_platform("control", t::Service::Control, 2);
    add_platform("runtime", t::Service::Runtime, 3);
    principals.push_back({{"user"}, t::PrincipalKind::Human, {}, {1}});
    authorizations.push_back({{"user"}, {5}, false});
    credentials.push_back({{{"password"}, {1}},
                           {"user"},
                           t::PasswordVerifier{"test-phc", {1}},
                           t::CredentialState::Active,
                           now - std::chrono::hours{1},
                           {},
                           {t::ResourceKind::Principal, {"user"}},
                           now + std::chrono::hours{24}});
    session = t::SessionSecurity{{"session"},
                                 {"user"},
                                 {{"password"}, {1}},
                                 {t::TokenPurpose::Access, {"key"}, {}},
                                 {t::TokenPurpose::Refresh, {"key"}, {}},
                                 now - std::chrono::seconds{10},
                                 now + std::chrono::minutes{5},
                                 now + std::chrono::hours{1},
                                 1,
                                 {1},
                                 false,
                                 t::SessionControlPeer{{"control"}, {"control-credential"}}};
    assignments = {
        {{"consumer-cap"},
         {"model"},
         {},
         {{t::Permission::IdentityAuthorizationCheck, {t::ResourceKind::Principal, {"model"}}}},
         {1}},
        {{"user-grant"},
         {"user"},
         {},
         {{t::Permission::ModelChat, {t::ResourceKind::Model, {"model-resource"}}}},
         {1}}};
    for (const auto* name : {"control", "runtime"}) {
      assignments.insert(assignments.end() - 1, {{std::string{name} + "-issue-cap"},
                                                 {name},
                                                 {},
                                                 {{t::Permission::IdentityDelegationIssue,
                                                   {t::ResourceKind::Principal, {name}}}},
                                                 {1}});
    }
  }
};
template <class Key, class Value> class ReadOne final : public t::Repository<Key, Value> {
public:
  explicit ReadOne(State& state) : state_(state) {}
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if constexpr (std::is_same_v<Value, t::Principal>) {
      for (const auto& value : state_.principals) {
        if (value.id == key) {
          return std::optional<Value>{value};
        }
      }
    } else if constexpr (std::is_same_v<Value, t::AuthorizationState>) {
      for (const auto& value : state_.authorizations) {
        if (value.subject == key) {
          return std::optional<Value>{value};
        }
      }
    } else {
      ++state_.unexpected;
      return failure();
    }
    return std::optional<Value>{};
  }
  t::Status save(const Value&, std::optional<t::ResourceVersion>) override {
    ++state_.writes;
    return failure();
  }

private:
  State& state_;
};
class Credentials final : public t::IdentityCredentialRepository {
public:
  explicit Credentials(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_bootstrap(const t::TokenVerifier&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& row : state_.credentials) {
      if (row.ref.id == id) {
        return std::optional{row};
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  version(const t::CredentialVersionRef&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest& fingerprint) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& row : state_.credentials) {
      const auto* certificate = std::get_if<t::CertificateBinding>(&row.material);
      if (certificate && certificate->fingerprint == fingerprint) {
        return std::optional{row};
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_platform(const t::PlatformPeer& peer) override {
    if (state_.ambiguous_platform) {
      return failure();
    }
    for (const auto& row : state_.credentials) {
      const auto* certificate = std::get_if<t::CertificateBinding>(&row.material);
      if (certificate && row.subject == peer.subject &&
          certificate->platform_service == peer.service) {
        return std::optional{row};
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }

  t::Outcome<std::optional<t::LoginIdentity>> login(const std::string&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save_login(const t::LoginIdentity&, std::optional<t::ResourceVersion>) override {
    ++state_.writes;
    return failure();
  }
  t::Status save(const t::CredentialVersionRecord&, std::optional<t::ResourceVersion>) override {
    ++state_.writes;
    return failure();
  }

private:
  State& state_;
};
class Assignments final : public t::IdentityExactAssignmentRepository {
public:
  explicit Assignments(State& state) : state_(state) {}
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& subject) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::ExactAssignment> result;
    for (const auto& row : state_.assignments) {
      if (row.subject == subject) {
        result.push_back(row);
      }
    }
    return result;
  }
  t::Status save(const t::ExactAssignment&, std::optional<t::ResourceVersion>) override {
    ++state_.writes;
    return failure();
  }

private:
  State& state_;
};
// These ports are deliberately traps: this read-only capability must never use them.
class Unused final : public t::ReceiptRepository,
                     public t::JournalRepository,
                     public t::IdentityBootstrapRepository,
                     public t::IdentitySecretReceiptRepository,
                     public t::Repository<t::OperationId, t::Operation>,
                     public t::Repository<t::AssignmentId, t::ResourceGrant> {
public:
  explicit Unused(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey&) override {
    return bad();
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt&, t::ReceiptCapacity) override {
    return bad();
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return bad();
  }
  t::Status append_audit(const t::AuditEntry&) override {
    return bad();
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    return bad();
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    return bad();
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    return bad();
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    return bad();
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    return bad();
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    return bad();
  }
  t::Outcome<t::AdministratorBootstrap> state() override {
    return bad();
  }
  t::Status consume(const t::AdministratorBootstrap&, t::ResourceVersion) override {
    return bad();
  }
  t::Outcome<std::optional<t::EncryptedSecretReceipt>> find(const t::SecretReceiptScope&) override {
    return bad();
  }
  t::Outcome<t::SecretReceiptWrite> insert(const t::EncryptedSecretReceipt&,
                                           t::SecretReceiptCapacity) override {
    return bad();
  }
  t::Outcome<std::optional<t::Operation>> find(const t::OperationId&) override {
    return bad();
  }
  t::Status save(const t::Operation&, std::optional<t::ResourceVersion>) override {
    return bad();
  }
  t::Outcome<std::optional<t::ResourceGrant>> find(const t::AssignmentId&) override {
    return bad();
  }
  t::Status save(const t::ResourceGrant&, std::optional<t::ResourceVersion>) override {
    return bad();
  }

private:
  t::Failure bad() {
    ++state_.unexpected;
    return failure();
  }
  State& state_;
};
class UnusedSessions final : public t::IdentitySessionSecurityRepository {
public:
  explicit UnusedSessions(State& state) : state_(state) {}
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId& id) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return state_.session && state_.session->id == id ? state_.session
                                                      : std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_access(const t::TokenVerifier&) override {
    return bad();
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_refresh(const t::TokenVerifier&) override {
    return bad();
  }
  t::Outcome<std::optional<t::RefreshHandle>>
  resolve_refresh(const t::TokenVerifier&, const t::RefreshRecoveryLookup&, t::TimePoint) override {
    return bad();
  }
  t::Status record_refresh_consumption(const t::ConsumedRefreshHandle&) override {
    return bad();
  }
  t::Outcome<std::size_t> count_unexpired() override {
    return bad();
  }
  t::Outcome<std::vector<t::SessionMetadata>> list_metadata(const t::SubjectId&,
                                                            std::size_t) override {
    return bad();
  }
  t::Status save(const t::SessionSecurity&, std::optional<t::ResourceVersion>,
                 std::optional<std::uint64_t>) override {
    return bad();
  }

private:
  t::Failure bad() {
    ++state_.unexpected;
    return failure();
  }
  State& state_;
};
class Work final : public t::IdentitySecurityUnitOfWork {
public:
  explicit Work(State& state)
      : state_(state), principal_(state), authorization_(state), metadata_(state),
        credentials_(state), assignments_(state), unused_(state), sessions_(state) {
    ++state_.active;
  }
  ~Work() override {
    rollback();
  }
  t::Outcome<t::TimePoint> current_time() override {
    if (state_.before_time) {
      auto hook = std::move(state_.before_time);
      state_.before_time = {};
      hook();
    }
    if (state_.fail_time) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return state_.now;
  }
  t::ReceiptRepository& receipts() override {
    return unused_;
  }
  t::JournalRepository& journal() override {
    return unused_;
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    return unused_;
  }
  t::Repository<t::SubjectId, t::Principal>& principals() override {
    return principal_;
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    return metadata_;
  }
  t::Repository<t::AssignmentId, t::ResourceGrant>& grants() override {
    return unused_;
  }
  t::Repository<t::SubjectId, t::AuthorizationState>& authorizations() override {
    return authorization_;
  }
  t::IdentityCredentialRepository& credentials() override {
    return credentials_;
  }
  t::IdentitySessionSecurityRepository& session_security() override {
    return sessions_;
  }
  t::IdentityExactAssignmentRepository& exact_assignments() override {
    return assignments_;
  }
  t::IdentitySecretReceiptRepository& secret_receipts() override {
    return unused_;
  }
  t::IdentityBootstrapRepository& bootstrap() override {
    return unused_;
  }
  t::CommitResult commit() override {
    ++state_.commits;
    rollback();
    return t::CommitResult::RolledBack;
  }
  void rollback() noexcept override {
    if (active_) {
      active_ = false;
      --state_.active;
    }
  }

private:
  State& state_;
  bool active_ = true;
  ReadOne<t::SubjectId, t::Principal> principal_;
  ReadOne<t::SubjectId, t::AuthorizationState> authorization_;
  ReadOne<t::SessionId, t::LoginSession> metadata_;
  Credentials credentials_;
  Assignments assignments_;
  Unused unused_;
  UnusedSessions sessions_;
};
class Transactions final : public t::IdentitySecurityUnitOfWorkFactory {
public:
  explicit Transactions(State& state) : state_(state) {}
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    ++state_.begins;
    if (state_.before_begin) {
      state_.before_begin();
    }
    if (state_.fail_begin) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return std::unique_ptr<t::IdentitySecurityUnitOfWork>{std::make_unique<Work>(state_)};
  }

private:
  State& state_;
};
class Crypto final : public t::DelegationCryptoPort {
public:
  explicit Crypto(State& state) : state_(state) {
    claims = {"identity",
              t::Service::Model,
              {"user"},
              {"runtime"},
              {{"control"}, {"runtime"}},
              {{t::Permission::ModelChat, {t::ResourceKind::Model, {"model-resource"}}}},
              {"proof"},
              std::get<t::CertificateBinding>(state.credentials[2].material).fingerprint,
              {5},
              state.now,
              state.now,
              state.now + std::chrono::seconds{56},
              2,
              {{"password"}, {1}},
              t::SessionId{"session"}};
  }
  t::Outcome<t::AuthorityVerifiedDelegation>
  verify_for_authorization(t::SecretView, const t::VerifiedPeer&,
                           const t::SecurityClockSample&) override {
    assert(state_.active == 0);
    ++calls;
    if (hook) {
      hook();
    }
    if (fault) {
      return failure(*fault);
    }
    return t::AuthorityVerifiedDelegation{claims};
  }
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims&, const t::CryptoKeyRef&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView, t::Service, const t::VerifiedPeer&,
                                           const t::SecurityClockSample&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::VerifiedDelegation> verify_exchange_parent(t::SecretView, const t::VerifiedPeer&,
                                                           const t::SecurityClockSample&) override {
    ++state_.unexpected;
    return failure();
  }
  State& state_;
  t::DelegationClaims claims;
  std::optional<t::ErrorCode> fault;
  std::function<void()> hook;
  int calls = 0;
};
class Clock final : public t::SecurityClockPort {
public:
  explicit Clock(State& state) : state_(state) {}
  t::Outcome<t::SecurityClockSample> sample() override {
    assert(state_.active == 0);
    ++calls;
    if (fault || (final_fault && calls == 2)) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    const auto instant = calls == 2 ? monotonic : std::chrono::steady_clock::time_point{};
    return t::SecurityClockSample{calls == 2 ? final_utc.value_or(state_.now) : state_.now, instant,
                                  std::chrono::milliseconds{0}, trusted};
  }
  State& state_;
  bool fault = false, trusted = true, final_fault = false;
  int calls = 0;
  std::optional<t::TimePoint> final_utc;
  std::chrono::steady_clock::time_point monotonic{};
};
class Methods final : public t::AuthorizationMethodPolicyPort {
public:
  explicit Methods(State& state) : state_(state) {}
  t::Outcome<std::optional<t::AuthorizationMethodPolicy>>
  resolve(std::string_view method) override {
    assert(state_.active == 0);
    if (fault) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return method == "/kiarabot.v1.ModelService/Chat" ? policy : std::nullopt;
  }
  State& state_;
  bool fault = false;
  std::optional<t::AuthorizationMethodPolicy> policy = t::AuthorizationMethodPolicy{
      t::Service::Model, t::Permission::ModelChat, {t::ResourceKind::Model}};
};
struct Fixture {
  State state;
  Transactions transactions{state};
  Crypto crypto{state};
  Clock clock{state};
  Methods methods{state};
  std::unique_ptr<t::IdentityAuthorizationApplication> app;
  Fixture() {
    auto result = t::make_identity_authorization_application(transactions, crypto, clock, methods);
    app = std::move(std::get<std::unique_ptr<t::IdentityAuthorizationApplication>>(result));
  }
  ~Fixture() {
    assert(state.active == 0 && state.commits == 0 && state.writes == 0 && state.unexpected == 0);
  }
  t::CheckAuthorizationRequest request() const {
    const auto& row = state.credentials[0];
    const auto& certificate = std::get<t::CertificateBinding>(row.material);
    static constexpr std::byte kProof[] = {std::byte{1}};
    return {{"request", "trace", {}, state.now + std::chrono::seconds{1}},
            {{t::PlatformPeer{*certificate.platform_service, row.subject}, certificate, row.ref},
             state.now},
            kProof,
            "/kiarabot.v1.ModelService/Chat",
            {{t::ResourceKind::Model, {"model-resource"}}},
            {}};
  }
};
void expect(const t::Outcome<t::CheckAuthorizationReply>& result, t::ErrorCode code) {
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).code == code);
}
void allowed_and_known_versions() {
  Fixture f;
  for (auto version : {std::optional<t::ResourceVersion>{}, std::optional<t::ResourceVersion>{{1}},
                       std::optional<t::ResourceVersion>{{5}}}) {
    auto request = f.request();
    request.known_authorization_version = version;
    const auto reply = std::get<t::CheckAuthorizationReply>(f.app->check_authorization(request));
    assert(reply.allowed && !reply.denial && reply.authorization_version.value == 5 &&
           reply.checked_at == f.state.now);
  }
  auto request = f.request();
  request.known_authorization_version = t::ResourceVersion{6};
  expect(f.app->check_authorization(request), t::ErrorCode::InvalidArgument);
}
void business_denial_is_a_decision() {
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    Fixture f;
    if (mutation == 0) {
      f.crypto.claims.permissions[0].resource.id.value = "other";
    }
    if (mutation == 1) {
      f.state.assignments.pop_back();
    }
    if (mutation == 2) {
      f.state.assignments.back().explicit_permissions[0].resource.id.value = "other";
    }
    const auto reply =
        std::get<t::CheckAuthorizationReply>(f.app->check_authorization(f.request()));
    assert(!reply.allowed && reply.denial && reply.denial->code == t::ErrorCode::PermissionDenied);
    assert(reply.authorization_version.value == 5 && reply.checked_at == f.state.now);
  }
}
void consumer_capability_and_method_boundary() {
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    Fixture f;
    auto request = f.request();
    if (mutation == 0) {
      f.state.assignments.erase(f.state.assignments.begin());
    }
    if (mutation == 1) {
      f.state.assignments[0].explicit_permissions[0].resource.id.value = "user";
    }
    if (mutation == 2) {
      f.state.assignments[0].explicit_permissions.clear();
      f.state.assignments[0].roles = {
          {t::FixedRole::Admin, {t::ResourceKind::Principal, {"model"}}}};
    }
    if (mutation == 3) {
      f.crypto.claims.audience = t::Service::Host;
    }
    if (mutation == 4) {
      f.methods.policy->owner = t::Service::Host;
    }
    if (mutation == 5) {
      request.recipient.binding.peer = t::AdapterPeer{{"adapter"}, {"model"}};
    }
    if (mutation == 6) {
      f.state.credentials[0].state = t::CredentialState::Revoked;
      f.state.credentials[0].revoked_at = f.state.now;
    }
    expect(f.app->check_authorization(request),
           mutation < 3 ? t::ErrorCode::PermissionDenied : t::ErrorCode::Unauthenticated);
    if (mutation < 3 || mutation == 4 || mutation == 5 || mutation == 6) {
      assert(f.crypto.calls == 0);
    }
  }
}
void invalid_inputs() {
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    Fixture f;
    auto request = f.request();
    if (mutation == 0) {
      request.full_method = "unknown";
    }
    if (mutation == 1) {
      request.scope.clear();
    }
    if (mutation == 2) {
      request.scope[0].id.value = "*";
    }
    if (mutation == 3) {
      request.scope.push_back(request.scope[0]);
    }
    if (mutation == 4) {
      request.scope[0].kind = t::ResourceKind::Account;
    }
    if (mutation == 5) {
      request.known_authorization_version = t::ResourceVersion{0};
    }
    if (mutation == 6) {
      request.subject_proof = {};
    }
    if (mutation == 7) {
      request.context.request_id.clear();
    }
    expect(f.app->check_authorization(request), t::ErrorCode::InvalidArgument);
    assert(f.crypto.calls == 0);
  }
}
void every_signed_actor_requires_current_explicit_delegator_capability() {
  for (unsigned hops : {1U, 2U}) {
    for (unsigned actor = 1; actor <= hops; ++actor) {
      for (unsigned mutation = 0; mutation < 5; ++mutation) {
        Fixture f;
        if (hops == 1) {
          f.crypto.claims.business_hops = 1;
          f.crypto.claims.actor_chain = {{"control"}};
          f.crypto.claims.direct_actor = {"control"};
          f.crypto.claims.client_certificate =
              std::get<t::CertificateBinding>(f.state.credentials[1].material).fingerprint;
        }
        const auto revoke = [&] {
          auto& assignment = f.state.assignments[actor];
          if (mutation == 1) {
            assignment.explicit_permissions[0].resource.id.value = "another-principal";
          } else if (mutation == 2) {
            assignment.explicit_permissions.clear();
            assignment.roles = {
                {t::FixedRole::Admin, {t::ResourceKind::Principal, {assignment.subject.value}}}};
          } else {
            assignment.explicit_permissions[0].permission = t::Permission::IdentityKeysRead;
          }
          ++f.state.authorizations[actor].version.value;
        };
        if (mutation == 3) {
          f.crypto.hook = revoke; // Signature finishes after the actor loses its capability.
        } else if (mutation == 4) {
          f.state.before_begin = [&] {
            if (f.state.begins == 3) {
              revoke();
            }
          };
        } else {
          revoke();
        }
        expect(f.app->check_authorization(f.request()), t::ErrorCode::Unauthenticated);
      }
    }
  }
}
void source_and_actor_rejections() {
  for (unsigned mutation = 0; mutation < 13; ++mutation) {
    Fixture f;
    if (mutation == 0) {
      f.state.principals[3].state = t::LifecycleState::Disabled;
    }
    if (mutation == 1) {
      f.state.credentials[3].ref.version.value = 2;
    }
    if (mutation == 2) {
      f.state.session->revoked = true;
    }
    if (mutation == 3) {
      f.state.session->control_peer.reset();
    }
    if (mutation == 4) {
      f.state.session->control_peer->subject = {"different-control"};
    }
    if (mutation == 5) {
      f.state.credentials[1].expires_at = f.state.now;
    }
    if (mutation == 6) {
      f.state.principals[1].state = t::LifecycleState::Disabled;
    }
    if (mutation == 7) {
      f.state.credentials[2].state = t::CredentialState::Revoked;
      f.state.credentials[2].revoked_at = f.state.now;
    }
    if (mutation == 8) {
      f.crypto.claims.client_certificate.bytes[0] = std::byte{99};
    }
    if (mutation == 9) {
      f.state.authorizations[3].version.value = 6;
    }
    if (mutation == 10) {
      std::get<t::CertificateBinding>(f.state.credentials[2].material).platform_service =
          t::Service::Control;
    }
    if (mutation == 11) {
      f.state.session->access_expires_at = f.state.now;
    }
    if (mutation == 12) {
      f.crypto.claims.direct_actor = {"control"};
    }
    expect(f.app->check_authorization(f.request()), t::ErrorCode::Unauthenticated);
  }
}
void failures_and_time_window() {
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    Fixture f;
    auto request = f.request();
    if (mutation == 0) {
      f.state.fail_begin = true;
    }
    if (mutation == 1) {
      f.state.fail_time = true;
    }
    if (mutation == 2) {
      f.state.fail_read = true;
    }
    if (mutation == 3) {
      f.clock.fault = true;
    }
    if (mutation == 4) {
      f.clock.trusted = false;
    }
    if (mutation == 5) {
      f.methods.fault = true;
    }
    if (mutation == 6) {
      f.crypto.fault = t::ErrorCode::UpstreamUnavailable;
    }
    if (mutation == 7) {
      f.crypto.hook = [&] { f.state.now += std::chrono::seconds{2}; };
    }
    if (mutation == 8) {
      f.crypto.hook = [&] { f.state.now -= std::chrono::seconds{1}; };
    }
    expect(f.app->check_authorization(request),
           mutation == 7 ? t::ErrorCode::Timeout : t::ErrorCode::UpstreamUnavailable);
  }
  Fixture f;
  f.crypto.fault = t::ErrorCode::Unauthenticated;
  expect(f.app->check_authorization(f.request()), t::ErrorCode::Unauthenticated);
}
void final_reread_rejects_current_authority_changes() {
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    Fixture f;
    f.state.before_begin = [&] {
      if (f.state.begins != 3) {
        return;
      }
      if (mutation == 0) {
        f.state.principals[1].version.value = 2;
      }
      if (mutation == 1) {
        f.state.session->rotation = 2;
      }
      if (mutation == 2) {
        f.state.authorizations[3].version.value = 6;
      }
      if (mutation == 3) {
        f.state.now -= std::chrono::milliseconds{1};
      }
    };
    expect(f.app->check_authorization(f.request()),
           mutation == 3 ? t::ErrorCode::UpstreamUnavailable : t::ErrorCode::Unauthenticated);
  }
}
void crypto_races_and_monotonic_budget() {
  for (unsigned mutation = 0; mutation < 5; ++mutation) {
    Fixture f;
    if (mutation == 0) {
      f.crypto.hook = [&] { f.state.principals[0].version.value = 2; };
    }
    if (mutation == 1) {
      f.clock.monotonic += std::chrono::milliseconds{1001};
    }
    if (mutation == 2) {
      f.clock.monotonic -= std::chrono::milliseconds{1};
    }
    if (mutation == 3) {
      f.clock.final_fault = true;
    }
    if (mutation == 4) {
      f.state.before_begin = [&] {
        if (f.state.begins == 3) {
          f.state.now += std::chrono::milliseconds{1001};
        }
      };
    }
    auto request = f.request();
    request.context.deadline += std::chrono::seconds{10};
    expect(f.app->check_authorization(request),
           mutation == 0 ? t::ErrorCode::Unauthenticated
                         : (mutation == 1 || mutation == 4 ? t::ErrorCode::Timeout
                                                           : t::ErrorCode::UpstreamUnavailable));
  }
}
void final_clock_crossing_expiry_never_returns_allow() {
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    Fixture f;
    const auto expiry = f.state.now + std::chrono::seconds{1};
    if (mutation == 0) {
      f.crypto.claims.expires_at = expiry;
    }
    if (mutation == 1) {
      f.state.credentials[3].expires_at = expiry;
    }
    if (mutation == 2) {
      f.state.credentials[0].expires_at = expiry;
      std::get<t::CertificateBinding>(f.state.credentials[0].material).not_after = expiry;
    }
    auto request = f.request();
    request.context.deadline = mutation == 3 ? expiry : expiry + std::chrono::seconds{10};
    f.clock.final_utc = expiry + std::chrono::nanoseconds{1};
    f.clock.monotonic += std::chrono::milliseconds{500};
    expect(f.app->check_authorization(request), t::ErrorCode::Timeout);
    assert(f.state.begins == 3 && f.clock.calls == 2);
    assert(f.state.now < expiry); // Every DB authority sample remains valid and unchanged.
  }
}
void permitted_consumers_and_rejected_control() {
  for (const auto service :
       {t::Service::Runtime, t::Service::Host, t::Service::Scheduler, t::Service::Model}) {
    Fixture f;
    std::get<t::CertificateBinding>(f.state.credentials[0].material).platform_service = service;
    f.crypto.claims.audience = service;
    f.methods.policy->owner = service;
    assert(std::get<t::CheckAuthorizationReply>(f.app->check_authorization(f.request())).allowed);
  }
  for (const auto service : {t::Service::Control, t::Service::Identity}) {
    Fixture f;
    std::get<t::CertificateBinding>(f.state.credentials[0].material).platform_service = service;
    expect(f.app->check_authorization(f.request()), t::ErrorCode::Unauthenticated);
    assert(f.crypto.calls == 0);
  }
}
void workload_and_single_hop() {
  Fixture workload;
  workload.crypto.claims.source_session.reset();
  workload.state.session.reset();
  workload.state.principals[3].kind = t::PrincipalKind::Adapter;
  workload.state.principals[3].binding = t::ResourceRef{t::ResourceKind::Principal, {"user"}};
  auto certificate = std::get<t::CertificateBinding>(workload.state.credentials[1].material);
  certificate.subject = {"user"};
  certificate.platform_service.reset();
  certificate.fingerprint.bytes[0] = std::byte{9};
  workload.state.credentials[3].material = certificate;
  assert(std::get<t::CheckAuthorizationReply>(workload.app->check_authorization(workload.request()))
             .allowed);
  workload.state.ambiguous_platform = true;
  expect(workload.app->check_authorization(workload.request()), t::ErrorCode::Internal);
  Fixture rotated;
  rotated.state.credentials[1].ref.version.value = 2;
  std::get<t::CertificateBinding>(rotated.state.credentials[1].material).fingerprint.bytes[0] =
      std::byte{8};
  assert(std::get<t::CheckAuthorizationReply>(rotated.app->check_authorization(rotated.request()))
             .allowed);
  Fixture first;
  first.crypto.claims.business_hops = 1;
  first.crypto.claims.actor_chain = {{"control"}};
  first.crypto.claims.direct_actor = {"control"};
  first.crypto.claims.client_certificate =
      std::get<t::CertificateBinding>(first.state.credentials[1].material).fingerprint;
  assert(std::get<t::CheckAuthorizationReply>(first.app->check_authorization(first.request()))
             .allowed);
}
} // namespace
int main() {
  allowed_and_known_versions();
  business_denial_is_a_decision();
  consumer_capability_and_method_boundary();
  invalid_inputs();
  source_and_actor_rejections();
  every_signed_actor_requires_current_explicit_delegator_capability();
  failures_and_time_window();
  final_reread_rejects_current_authority_changes();
  workload_and_single_hop();
  crypto_races_and_monotonic_budget();
  permitted_consumers_and_rejected_control();
  final_clock_crossing_expiry_never_returns_allow();
}
