// clang-format off
#include "kiarabot/core/application/target/identity_exchange_application.hpp"

#include <cassert>
#include <functional>
#include <stdexcept>
#include <utility>
// clang-format on

namespace {
namespace t = kiarabot::core::target;
t::Failure failure(t::ErrorCode code = t::ErrorCode::Internal) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
t::SecretView view(std::string_view input) {
  return std::as_bytes(std::span{input.data(), input.size()});
}
class Secret final : public t::SecretBuffer {
public:
  explicit Secret(t::SecretView input) : value_(input.begin(), input.end()) {}
  std::span<const std::byte> bytes() const noexcept override {
    return value_;
  }

private:
  std::vector<std::byte> value_;
};
struct State {
  t::TimePoint now{std::chrono::seconds{1000}};
  t::SubjectId control{"11111111-1111-4111-8111-111111111111"},
      runtime{"22222222-2222-4222-8222-222222222222"},
      human{"33333333-3333-4333-8333-333333333333"};
  t::ResourceRef model{t::ResourceKind::Model, {"88888888-8888-4888-8888-888888888888"}},
      task{t::ResourceKind::Task, {"99999999-9999-4999-8999-999999999999"}};
  std::vector<t::Principal> principals;
  std::vector<t::AuthorizationState> authorizations;
  std::vector<t::CredentialVersionRecord> credentials;
  std::vector<t::ExactAssignment> assignments;
  t::SessionSecurity session;
  std::vector<t::LoginSession> metadata;
  t::DelegationClaims parent;
  int active = 0, begins = 0, commits = 0, writes = 0, unexpected = 0;
  bool fail_begin = false, fail_time = false, fail_read = false;
  std::function<void()> before_time, before_begin;
  State() {
    const auto created = now - std::chrono::hours{1}, expires = now + std::chrono::hours{24};
    for (const auto& subject : {control, runtime}) {
      const auto service = subject == control ? t::Service::Control : t::Service::Runtime;
      const t::ResourceRef binding{t::ResourceKind::Principal, {subject.value}};
      principals.push_back({subject, t::PrincipalKind::PlatformService, binding, {1}});
      authorizations.push_back({subject, {1}, false});
      t::CertificateBinding certificate{
          subject, {},      subject == control ? "spiffe://control" : "spiffe://runtime",
          service, created, expires};
      certificate.fingerprint.bytes[0] = subject == control ? std::byte{1} : std::byte{2};
      const t::CredentialId id{subject == control ? "44444444-4444-4444-8444-444444444444"
                                                  : "55555555-5555-4555-8555-555555555555"};
      credentials.push_back({{id, {1}},
                             subject,
                             certificate,
                             t::CredentialState::Active,
                             created,
                             {},
                             binding,
                             expires});
    }
    principals.push_back({human, t::PrincipalKind::Human, {}, {1}});
    authorizations.push_back({human, {7}, false});
    const t::CredentialVersionRef source{{"66666666-6666-4666-8666-666666666666"}, {1}};
    credentials.push_back({source,
                           human,
                           t::PasswordVerifier{"verifier", {1}},
                           t::CredentialState::Active,
                           created,
                           {},
                           {t::ResourceKind::Principal, {human.value}},
                           expires});
    session = {{"77777777-7777-4777-8777-777777777777"},
               human,
               source,
               {t::TokenPurpose::Access, {"token"}, {}},
               {t::TokenPurpose::Refresh, {"token"}, {}},
               created,
               now + std::chrono::minutes{15},
               now + std::chrono::hours{24},
               1,
               {1},
               false,
               t::SessionControlPeer{control, credentials[0].ref.id}};
    metadata = {{session.id, human, {1}, 1, session.refresh_expires_at, false}};
    assignments = {
        {{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"},
         human,
         {},
         {{t::Permission::ModelChat, model}},
         {1}},
        {{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"},
         runtime,
         {},
         {{t::Permission::IdentityDelegationIssue, {t::ResourceKind::Principal, {runtime.value}}}},
         {1}},
        {{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"},
         control,
         {},
         {{t::Permission::IdentityDelegationIssue, {t::ResourceKind::Principal, {control.value}}}},
         {1}}};
    assignments.push_back({{"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"},
                           human,
                           {},
                           {{t::Permission::TaskSchedule, task}},
                           {1}});
    parent = {"identity",
              t::Service::Runtime,
              human,
              control,
              {control},
              {{t::Permission::ModelChat, model}, {t::Permission::TaskSchedule, task}},
              {"cccccccc-cccc-4ccc-8ccc-cccccccccccc"},
              std::get<t::CertificateBinding>(credentials[0].material).fingerprint,
              {7},
              now - std::chrono::seconds{10},
              now - std::chrono::seconds{10},
              now + std::chrono::seconds{46},
              1,
              source,
              session.id};
  }
};
template <class Key, class Value> class ReadMany final : public t::Repository<Key, Value> {
public:
  ReadMany(State& state, std::vector<Value>& values) : state_(state), values_(values) {}
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& value : values_) {
      const bool match = [&] {
        if constexpr (requires { value.id; }) {
          return value.id == key;
        } else {
          return value.subject == key;
        }
      }();
      if (match) {
        return std::optional{value};
      }
    }
    return std::optional<Value>{};
  }
  t::Status save(const Value&, std::optional<t::ResourceVersion>) override {
    ++state_.writes;
    return failure();
  }

private:
  State& state_;
  std::vector<Value>& values_;
};
class Credentials final : public t::IdentityCredentialRepository {
public:
  explicit Credentials(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_bootstrap(const t::TokenVerifier&) override {
    throw std::logic_error("unexpected bootstrap verifier lookup");
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_platform(const t::PlatformPeer&) override {
    throw std::logic_error("unexpected platform actor lookup");
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
  by_certificate(const t::Sha256Digest& tag) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& row : state_.credentials) {
      const auto* cert = std::get_if<t::CertificateBinding>(&row.material);
      if (cert && cert->fingerprint == tag) {
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
  t::Outcome<std::optional<t::AssignmentStateRecord>> current(const t::AssignmentId&) override {
    throw std::logic_error("unexpected assignment state lookup");
  }
  t::Status save_state(const t::AssignmentStateRecord&,
                       std::optional<t::ResourceVersion>) override {
    throw std::logic_error("unexpected assignment state write");
  }
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
class SourceSessions final : public t::IdentitySessionSecurityRepository {
public:
  explicit SourceSessions(State& state) : state_(state) {}
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId& id) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return id == state_.session.id ? std::optional{state_.session}
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
      : state_(state), principal_(state, state.principals),
        authorization_(state, state.authorizations), metadata_(state, state.metadata),
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
  ReadMany<t::SubjectId, t::Principal> principal_;
  ReadMany<t::SubjectId, t::AuthorizationState> authorization_;
  ReadMany<t::SessionId, t::LoginSession> metadata_;
  Credentials credentials_;
  Assignments assignments_;
  Unused unused_;
  SourceSessions sessions_;
};
class Transactions final : public t::IdentitySecurityUnitOfWorkFactory {
public:
  explicit Transactions(State& state) : state_(state) {}
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    ++state_.begins;
    if (state_.before_begin) {
      auto hook = std::move(state_.before_begin);
      state_.before_begin = {};
      hook();
    }
    if (state_.fail_begin) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return std::unique_ptr<t::IdentitySecurityUnitOfWork>{std::make_unique<Work>(state_)};
  }

private:
  State& state_;
};
// Fake proof bytes below exercise orchestration only; real JWT adapters are tested separately.
class Clock final : public t::SecurityClockPort {
public:
  explicit Clock(State& state) : state_(state) {}
  t::Outcome<t::SecurityClockSample> sample() override {
    assert(state_.active == 0);
    if (unavailable) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return t::SecurityClockSample{state_.now, {}, skew, trusted};
  }
  State& state_;
  bool trusted = true, unavailable = false;
  std::chrono::milliseconds skew{0};
};
class Random final : public t::RandomPort {
public:
  explicit Random(State& state) : state_(state) {}
  t::Outcome<t::OwnedSecret> bytes(std::size_t count) override {
    assert(state_.active == 0);
    if (unavailable) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<std::byte> data(wrong_size ? 0 : count, std::byte{1});
    if (after_bytes) {
      auto hook = std::move(after_bytes);
      after_bytes = {};
      hook();
    }
    return std::unique_ptr<t::SecretBuffer>{std::make_unique<Secret>(data)};
  }
  t::Outcome<t::OwnedSecret> opaque_token() override {
    ++state_.unexpected;
    return failure();
  }
  State& state_;
  bool unavailable = false, wrong_size = false;
  std::function<void()> after_bytes;
};
class Crypto final : public t::DelegationCryptoPort {
public:
  explicit Crypto(State& state) : state_(state) {}
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims& claims,
                                  const t::CryptoKeyRef& key) override {
    assert(state_.active == 0);
    ++signs;
    assert(key.value == "signing");
    signed_claims = claims;
    if (fail_sign) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (after_sign) {
      auto hook = std::move(after_sign);
      after_sign = {};
      hook();
    }
    return std::unique_ptr<t::SecretBuffer>{std::make_unique<Secret>(view("signed-proof"))};
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView, t::Service, const t::VerifiedPeer&,
                                           const t::SecurityClockSample&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::AuthorityVerifiedDelegation>
  verify_for_authorization(t::SecretView, const t::VerifiedPeer&,
                           const t::SecurityClockSample&) override {
    throw std::logic_error("unexpected online authority verification");
  }
  t::Outcome<t::VerifiedDelegation> verify_exchange_parent(t::SecretView proof,
                                                           const t::VerifiedPeer& recipient,
                                                           const t::SecurityClockSample&) override {
    assert(state_.active == 0);
    ++verifies;
    assert(std::ranges::equal(proof, view("parent-proof")));
    assert(std::get<t::PlatformPeer>(recipient.binding.peer).service == t::Service::Runtime);
    if (fail_verify) {
      return failure(t::ErrorCode::Unauthenticated);
    }
    if (after_verify) {
      auto hook = std::move(after_verify);
      after_verify = {};
      hook();
    }
    return t::VerifiedDelegation{state_.parent, {"parent-signing"}};
  }
  State& state_;
  int signs = 0, verifies = 0;
  bool fail_sign = false, fail_verify = false;
  std::optional<t::DelegationClaims> signed_claims;
  std::function<void()> after_verify, after_sign;
};
struct Fixture {
  State state;
  Transactions transactions{state};
  Crypto crypto{state};
  Random random{state};
  Clock clock{state};
  t::IdentityExchangeApplication app{
      transactions, crypto, random, clock, {"identity", {"signing"}}};
  ~Fixture() {
    assert(state.active == 0 && state.commits == 0 && state.writes == 0 && state.unexpected == 0);
  }
  t::ExchangeIdentityDelegationRequest request() const {
    const auto& certificate = std::get<t::CertificateBinding>(state.credentials[1].material);
    return {{"request", "trace", {}, state.now + std::chrono::seconds{30}},
            {{t::PlatformPeer{t::Service::Runtime, state.runtime}, certificate,
              state.credentials[1].ref},
             state.now},
            view("parent-proof"),
            t::Service::Model,
            {{t::Permission::ModelChat, state.model}}};
  }
};
void expect_failure(const t::Outcome<t::IdentityDelegationReply>& result) {
  assert(std::holds_alternative<t::Failure>(result));
}
void valid_second_hop_preserves_origin_and_binds_runtime() {
  Fixture f;
  auto request = f.request();
  auto reply = f.app.exchange_delegation(request);
  assert(std::holds_alternative<t::IdentityDelegationReply>(reply));
  const auto& result = std::get<t::IdentityDelegationReply>(reply);
  const auto& claims = result.claims;
  assert(result.proof && !result.proof->bytes().empty());
  assert(claims.business_hops == 2);
  assert(claims.subject == f.state.human &&
         claims.source_credential == f.state.parent.source_credential &&
         claims.source_session == f.state.parent.source_session);
  assert(claims.actor_chain == std::vector<t::SubjectId>({f.state.control, f.state.runtime}));
  assert(claims.direct_actor == f.state.runtime && claims.audience == t::Service::Model &&
         claims.permissions == request.permissions);
  assert(claims.client_certificate ==
         std::get<t::CertificateBinding>(f.state.credentials[1].material).fingerprint);
  assert(claims.client_certificate != f.state.parent.client_certificate);
  assert(claims.expires_at <= f.state.parent.expires_at &&
         claims.expires_at - claims.issued_at <= std::chrono::seconds{56});
  assert(result.checked_at == f.state.now && f.crypto.verifies == 1 && f.crypto.signs == 1);
  request.audience = t::Service::Scheduler;
  request.permissions = {{t::Permission::TaskSchedule, f.state.task}};
  assert(std::holds_alternative<t::IdentityDelegationReply>(f.app.exchange_delegation(request)));
}
void all_three_current_authorities_are_independent() {
  for (unsigned checkpoint = 0; checkpoint < 2; ++checkpoint) {
    for (unsigned authority = 0; authority < 3; ++authority) {
      Fixture f;
      const auto revoke = [&] {
        f.state.assignments.erase(f.state.assignments.begin() + authority);
      };
      if (checkpoint == 0) {
        revoke();
      } else {
        f.crypto.after_sign = revoke;
      }
      expect_failure(f.app.exchange_delegation(f.request()));
      assert(f.crypto.signs == (checkpoint == 0 ? 0 : 1));
    }
  }
  for (unsigned authority = 1; authority < 3; ++authority) {
    Fixture f;
    f.state.assignments[authority].explicit_permissions[0].resource = f.state.model;
    expect_failure(f.app.exchange_delegation(f.request()));
    assert(f.crypto.signs == 0);
    f.state.assignments[authority].explicit_permissions[0].resource = {t::ResourceKind::Principal,
                                                                       {f.state.human.value}};
    expect_failure(f.app.exchange_delegation(f.request()));
    assert(f.crypto.signs == 0);
    f.state.assignments[authority].explicit_permissions.clear();
    const auto& owner = authority == 1 ? f.state.runtime : f.state.control;
    f.state.assignments[authority].roles = {
        {t::FixedRole::Admin, {t::ResourceKind::Principal, {owner.value}}}};
    expect_failure(f.app.exchange_delegation(f.request()));
    assert(f.crypto.signs == 0);
  }
}
void previous_control_and_origin_revocation_rechecked() {
  for (unsigned checkpoint = 0; checkpoint < 2; ++checkpoint) {
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
      Fixture f;
      const auto change = [&] {
        switch (mutation) {
        case 0:
          f.state.credentials[0].state = t::CredentialState::Revoked;
          f.state.credentials[0].revoked_at = f.state.now;
          break;
        case 1:
          ++f.state.credentials[0].ref.version.value;
          std::get<t::CertificateBinding>(f.state.credentials[0].material).fingerprint.bytes[0] =
              std::byte{9};
          break;
        case 2:
          f.state.principals[0].state = t::LifecycleState::Disabled;
          break;
        case 3:
          std::get<t::CertificateBinding>(f.state.credentials[0].material).platform_service =
              t::Service::Runtime;
          break;
        case 4:
          f.state.credentials[2].state = t::CredentialState::Revoked;
          f.state.credentials[2].revoked_at = f.state.now;
          break;
        case 5:
          ++f.state.credentials[2].ref.version.value;
          break;
        case 6:
          f.state.session.revoked = true;
          f.state.metadata[0].revoked = true;
          break;
        case 7:
          ++f.state.authorizations[2].version.value;
          break;
        case 8:
          f.state.credentials[1].state = t::CredentialState::Revoked;
          f.state.credentials[1].revoked_at = f.state.now;
          break;
        case 9:
          f.state.principals[1].state = t::LifecycleState::Disabled;
          break;
        case 10:
          std::get<t::CertificateBinding>(f.state.credentials[0].material).registered_san.clear();
          break;
        case 11:
          f.state.credentials[0].expires_at = f.state.now;
          std::get<t::CertificateBinding>(f.state.credentials[0].material).not_after = f.state.now;
          break;
        default:
          throw std::logic_error("unexpected mutation");
        }
      };
      if (checkpoint == 0) {
        change();
      } else {
        f.crypto.after_sign = change;
      }
      expect_failure(f.app.exchange_delegation(f.request()));
    }
  }
}
void parent_shape_and_narrowing_are_closed() {
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    Fixture f;
    auto request = f.request();
    switch (scenario) {
    case 0:
      request.audience = t::Service::Runtime;
      break;
    case 1:
      request.audience = t::Service::Host;
      break;
    case 2:
      request.permissions = {{t::Permission::ModelChat,
                              {t::ResourceKind::Model, {"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"}}}};
      break;
    case 3:
      f.state.parent.business_hops = 2;
      f.state.parent.actor_chain.push_back(f.state.runtime);
      break;
    case 4:
      f.state.parent.audience = t::Service::Model;
      break;
    case 5:
      f.state.parent.source_session.reset();
      break;
    case 6:
      f.state.parent.client_certificate =
          std::get<t::CertificateBinding>(f.state.credentials[1].material).fingerprint;
      break;
    case 7:
      request.permissions.clear();
      break;
    default:
      throw std::logic_error("unexpected scenario");
    }
    expect_failure(f.app.exchange_delegation(request));
    assert(f.crypto.signs == 0);
  }
}
void dependency_clock_and_random_failures() {
  for (unsigned scenario = 0; scenario < 9; ++scenario) {
    Fixture f;
    switch (scenario) {
    case 0:
      f.state.fail_begin = true;
      break;
    case 1:
      f.state.fail_time = true;
      break;
    case 2:
      f.state.fail_read = true;
      break;
    case 3:
      f.crypto.fail_verify = true;
      break;
    case 4:
      f.crypto.fail_sign = true;
      break;
    case 5:
      f.clock.unavailable = true;
      break;
    case 6:
      f.clock.trusted = false;
      break;
    case 7:
      f.clock.skew = std::chrono::milliseconds{2001};
      break;
    case 8:
      f.random.wrong_size = true;
      break;
    default:
      throw std::logic_error("unexpected scenario");
    }
    expect_failure(f.app.exchange_delegation(f.request()));
  }
  Fixture random;
  random.random.unavailable = true;
  expect_failure(random.app.exchange_delegation(random.request()));
}
void final_time_floor_and_authorization_budget() {
  Fixture floor;
  floor.state.now += std::chrono::milliseconds{900};
  auto request = floor.request();
  auto result = floor.app.exchange_delegation(request);
  assert(std::holds_alternative<t::IdentityDelegationReply>(result));
  const auto& claims = std::get<t::IdentityDelegationReply>(result).claims;
  assert(claims.expires_at <= floor.state.parent.expires_at &&
         claims.expires_at <= request.context.deadline);
  assert(claims.expires_at.time_since_epoch().count() % 1000000000 == 0);
  Fixture stale;
  unsigned samples = 0;
  std::function<void()> final_delay;
  final_delay = [&] {
    if (++samples == 1) {
      stale.state.before_time = final_delay;
    } else {
      stale.state.now += std::chrono::seconds{2};
    }
  };
  stale.crypto.after_sign = [&] { stale.state.before_time = final_delay; };
  expect_failure(stale.app.exchange_delegation(stale.request()));
  Fixture gap;
  const auto gap_request = gap.request();
  gap.random.after_bytes = [&] { gap.state.now = gap_request.context.deadline; };
  const auto exhausted = gap.app.exchange_delegation(gap_request);
  assert(std::get<t::Failure>(exhausted).code == t::ErrorCode::Timeout && gap.crypto.signs == 0);
  Fixture expired;
  expired.state.parent.expires_at = expired.state.now;
  expect_failure(expired.app.exchange_delegation(expired.request()));
  Fixture deadline;
  auto elapsed = deadline.request();
  elapsed.context.deadline = deadline.state.now;
  expect_failure(deadline.app.exchange_delegation(elapsed));
}
void workload_source_keeps_credential_origin_without_human_session() {
  Fixture f;
  const t::ResourceRef instance{t::ResourceKind::Instance,
                                {"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"}};
  f.state.principals[2].kind = t::PrincipalKind::Plugin;
  f.state.principals[2].binding = instance;
  f.state.credentials[2].binding = instance;
  auto& credential = f.state.credentials[2];
  assert(credential.expires_at.has_value());
  const auto expiry = *credential.expires_at;
  credential.material =
      t::WorkloadBootstrapVerifier{{t::TokenPurpose::WorkloadBootstrap, {"bootstrap"}, {}},
                                   instance,
                                   {t::Service::Runtime, t::Service::Model, t::Service::Scheduler},
                                   expiry};
  f.state.parent.source_session.reset();
  const auto result = f.app.exchange_delegation(f.request());
  assert(std::holds_alternative<t::IdentityDelegationReply>(result));
  const auto& child = std::get<t::IdentityDelegationReply>(result).claims;
  assert(child.subject == f.state.parent.subject && !child.source_session);
  assert(child.source_credential == f.state.parent.source_credential);
  f.crypto.after_sign = [&] { ++f.state.credentials[2].ref.version.value; };
  expect_failure(f.app.exchange_delegation(f.request()));
}
void signed_lifetime_is_capped_by_every_authoritative_bound() {
  for (unsigned boundary = 0; boundary < 6; ++boundary) {
    Fixture f;
    auto limit = f.state.now + std::chrono::milliseconds{3900};
    if (boundary == 0) {
      limit = f.state.now + std::chrono::seconds{3};
      f.state.parent.expires_at = limit;
    }
    if (boundary == 1 || boundary == 2) {
      const auto index = boundary == 1 ? 1U : 0U;
      f.state.credentials[index].expires_at = limit;
      std::get<t::CertificateBinding>(f.state.credentials[index].material).not_after = limit;
    }
    if (boundary == 3) {
      f.state.credentials[2].expires_at = limit;
    }
    if (boundary == 4) {
      f.state.session.access_expires_at = limit;
    }
    auto request = f.request();
    if (boundary == 5) {
      request.context.deadline = limit;
    }
    const auto result = f.app.exchange_delegation(request);
    assert(std::holds_alternative<t::IdentityDelegationReply>(result));
    const auto& child = std::get<t::IdentityDelegationReply>(result).claims;
    assert(child.expires_at <= limit && child.expires_at <= f.state.parent.expires_at);
    assert(child.expires_at.time_since_epoch().count() % 1000000000 == 0);
    assert(child.expires_at - child.issued_at <= std::chrono::seconds{56});
  }
}

void backwards_database_time_across_authorization_uows_is_unavailable() {
  for (const auto rollback : {std::chrono::milliseconds{250}, std::chrono::milliseconds{1000}}) {
    Fixture f;
    const auto not_before = f.state.now;
    f.state.now += rollback + std::chrono::milliseconds{250};
    f.state.parent.issued_at = not_before;
    f.state.parent.not_before = not_before;
    const auto initial_time = f.state.now;
    f.crypto.after_sign = [&] { f.state.now -= rollback; };
    const auto result = f.app.exchange_delegation(f.request());
    assert(f.crypto.signs == 1 && f.crypto.verifies == 1);
    assert(f.crypto.signed_claims.has_value());
    assert(f.state.now < initial_time && f.state.now >= not_before);
    assert(f.state.now < f.state.parent.expires_at);
    assert(std::holds_alternative<t::Failure>(result));
    assert(std::get<t::Failure>(result).code == t::ErrorCode::UpstreamUnavailable);
    assert(f.state.writes == 0 && f.state.commits == 0);
  }
}

} // namespace
int main() try {
  valid_second_hop_preserves_origin_and_binds_runtime();
  all_three_current_authorities_are_independent();
  previous_control_and_origin_revocation_rechecked();
  parent_shape_and_narrowing_are_closed();
  dependency_clock_and_random_failures();
  final_time_floor_and_authorization_budget();
  backwards_database_time_across_authorization_uows_is_unavailable();
  workload_source_keeps_credential_origin_without_human_session();
  signed_lifetime_is_capped_by_every_authoritative_bound();
} catch (...) {
  return 1;
}
