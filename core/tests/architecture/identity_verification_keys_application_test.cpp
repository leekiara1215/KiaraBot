// clang-format off
#include "kiarabot/core/application/target/identity_verification_keys_application.hpp"

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
struct State {
  t::TimePoint now{std::chrono::seconds{1000}};
  t::SubjectId subject{"11111111-1111-4111-8111-111111111111"};
  t::ResourceRef catalog{t::ResourceKind::Catalog, {"22222222-2222-4222-8222-222222222222"}};
  t::Principal principal{subject,
                         t::PrincipalKind::PlatformService,
                         t::ResourceRef{t::ResourceKind::Principal, {subject.value}},
                         {1}};
  t::AuthorizationState authorization{subject, {5}, false};
  t::CredentialVersionRecord credential;
  std::vector<t::ExactAssignment> assignments;
  int active = 0, begins = 0, commits = 0, writes = 0, unexpected = 0;
  bool fail_begin = false, fail_time = false, fail_read = false;
  std::function<void()> before_begin;
  std::function<void()> before_time;
  State() {
    t::CertificateBinding certificate{subject,
                                      {},
                                      "spiffe://control",
                                      t::Service::Control,
                                      now - std::chrono::hours{1},
                                      now + std::chrono::hours{24}};
    certificate.fingerprint.bytes[0] = std::byte{1};
    credential = {{{"33333333-3333-4333-8333-333333333333"}, {1}},
                  subject,
                  certificate,
                  t::CredentialState::Active,
                  now - std::chrono::hours{1},
                  {},
                  {t::ResourceKind::Principal, {subject.value}},
                  now + std::chrono::hours{24}};
    assignments = {{{"44444444-4444-4444-8444-444444444444"},
                    subject,
                    {},
                    {{t::Permission::IdentityKeysRead, catalog}},
                    {1}}};
  }
};
template <class Key, class Value> class ReadOne final : public t::Repository<Key, Value> {
public:
  ReadOne(State& state, Value* value) : state_(state), value_(value) {}
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (value_) {
      const bool match = [&] {
        if constexpr (requires { value_->id; }) {
          return value_->id == key;
        } else {
          return value_->subject == key;
        }
      }();
      if (match) {
        return std::optional<Value>{*value_};
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
  Value* value_;
};
class Credentials final : public t::IdentityCredentialRepository {
public:
  explicit Credentials(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_bootstrap(const t::TokenVerifier&) override {
    throw std::logic_error("unexpected bootstrap verifier lookup");
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    if (state_.fail_read) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return id == state_.credential.ref.id ? std::optional{state_.credential}
                                          : std::optional<t::CredentialVersionRecord>{};
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
    const auto* certificate = std::get_if<t::CertificateBinding>(&state_.credential.material);
    return certificate && certificate->fingerprint == fingerprint
               ? std::optional{state_.credential}
               : std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_platform(const t::PlatformPeer&) override {
    throw std::logic_error("unexpected platform credential lookup");
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
    return subject == state_.subject ? state_.assignments : std::vector<t::ExactAssignment>{};
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
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId&) override {
    return bad();
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
      : state_(state), principal_(state, &state.principal),
        authorization_(state, &state.authorization), metadata_(state, nullptr), credentials_(state),
        assignments_(state), unused_(state), sessions_(state) {
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
// Public-only fixture: no claim that these deterministic raw32 bytes validate Ed25519 crypto.
class Material final : public t::VerificationKeyMaterialPort {
public:
  explicit Material(State& state) : state_(state) {
    keys = {{{{"active"}, {}, {2}, state.now + std::chrono::hours{1}}}, {2}, state.catalog};
    keys.keys[0].ed25519_public_key[0] = std::byte{9};
  }
  t::Outcome<t::VerificationKeySet> snapshot() override {
    assert(state_.active == 0);
    ++calls;
    if (unavailable) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    if (after_snapshot) {
      auto hook = std::move(after_snapshot);
      after_snapshot = {};
      hook();
    }
    return keys;
  }
  State& state_;
  t::VerificationKeySet keys;
  int calls = 0;
  bool unavailable = false;
  std::function<void()> after_snapshot;
};
struct Fixture {
  State state;
  Transactions transactions{state};
  Material material{state};
  std::unique_ptr<t::IdentityVerificationKeysApplication> app;
  Fixture() {
    auto result = t::make_identity_verification_keys_application(transactions, material);
    app = std::move(std::get<std::unique_ptr<t::IdentityVerificationKeysApplication>>(result));
  }
  ~Fixture() {
    assert(state.active == 0 && state.commits == 0 && state.writes == 0 && state.unexpected == 0);
  }
  t::GetVerificationKeysRequest request(std::optional<t::ResourceVersion> known = {}) const {
    const auto& cert = std::get<t::CertificateBinding>(state.credential.material);
    assert(cert.platform_service.has_value());
    return {{"request", "trace", {}, state.now + std::chrono::seconds{30}},
            {{t::PlatformPeer{*cert.platform_service, state.subject}, cert, state.credential.ref},
             state.now},
            known};
  }
};
void expect(const t::Outcome<t::GetVerificationKeysReply>& result, t::ErrorCode code) {
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).code == code);
}
void complete_keys_for_all_supported_known_versions() {
  Fixture f;
  for (auto known : {std::optional<t::ResourceVersion>{}, std::optional<t::ResourceVersion>{{1}},
                     std::optional<t::ResourceVersion>{{2}}}) {
    const auto reply = f.app->get_verification_keys(f.request(known));
    assert(std::holds_alternative<t::GetVerificationKeysReply>(reply));
    const auto& value = std::get<t::GetVerificationKeysReply>(reply);
    assert(value.keys.version.value == 2 && value.keys.keys.size() == 1);
    assert(value.keys.keys[0].ed25519_public_key == f.material.keys.keys[0].ed25519_public_key);
    assert(value.checked_at == f.state.now);
  }
  expect(f.app->get_verification_keys(f.request(t::ResourceVersion{3})),
         t::ErrorCode::InvalidArgument);
  expect(f.app->get_verification_keys(f.request(t::ResourceVersion{0})),
         t::ErrorCode::InvalidArgument);
  auto request = f.request();
  std::get<t::PlatformPeer>(request.caller.binding.peer).service = t::Service::Runtime;
  std::get<t::CertificateBinding>(f.state.credential.material).platform_service =
      t::Service::Runtime;
  request.caller.binding.certificate.platform_service = t::Service::Runtime;
  assert(
      std::holds_alternative<t::GetVerificationKeysReply>(f.app->get_verification_keys(request)));
}
void exact_grants_and_permission_precedence() {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    Fixture f;
    if (scenario == 0) {
      f.state.assignments.clear();
    }
    if (scenario == 1) {
      f.state.assignments[0].explicit_permissions[0].resource.id.value =
          "55555555-5555-4555-8555-555555555555";
    }
    if (scenario == 2) {
      f.state.assignments[0].explicit_permissions.clear();
      f.state.assignments[0].roles = {{t::FixedRole::Admin, f.state.catalog}};
    }
    expect(f.app->get_verification_keys(f.request(t::ResourceVersion{999})),
           t::ErrorCode::PermissionDenied);
  }
}
void peer_and_current_credential_denials() {
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    Fixture f;
    auto request = f.request();
    switch (scenario) {
    case 0:
      request.caller.binding.peer =
          t::PluginPeer{{"55555555-5555-4555-8555-555555555555"}, f.state.subject};
      break;
    case 1:
      f.state.principal.kind = t::PrincipalKind::Human;
      break;
    case 2:
      ++f.state.credential.ref.version.value;
      break;
    case 3:
      f.state.credential.state = t::CredentialState::Revoked;
      f.state.credential.revoked_at = f.state.now;
      break;
    case 4:
      request.caller.binding.certificate.fingerprint.bytes[0] = std::byte{2};
      break;
    case 5:
      request.caller.binding.certificate.registered_san = "spiffe://wrong";
      break;
    case 6:
      std::get<t::PlatformPeer>(request.caller.binding.peer).service = t::Service::Runtime;
      break;
    case 7:
      f.state.principal.state = t::LifecycleState::Disabled;
      break;
    default:
      throw std::logic_error("unexpected scenario");
    }
    expect(f.app->get_verification_keys(request), t::ErrorCode::Unauthenticated);
  }
}
void material_and_dependency_failures_never_fake_empty() {
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    Fixture f;
    switch (scenario) {
    case 0:
      f.material.keys.keys.clear();
      break;
    case 1:
      f.material.keys.version = {0};
      break;
    case 2:
      f.material.keys.keys[0].version = {0};
      break;
    case 3:
      f.material.keys.keys[0].valid_until = t::TimePoint::max();
      break;
    case 4:
      f.material.keys.keyset_resource.kind = t::ResourceKind::Principal;
      break;
    case 5:
      f.material.keys.keyset_resource.id.value = "keys";
      break;
    case 6:
      f.material.keys.keys.push_back(f.material.keys.keys[0]);
      break;
    case 7:
      f.material.keys.keys[0].reference.value.clear();
      break;
    default:
      throw std::logic_error("unexpected scenario");
    }
    expect(f.app->get_verification_keys(f.request()), t::ErrorCode::Internal);
  }
  Fixture unavailable;
  unavailable.material.unavailable = true;
  expect(unavailable.app->get_verification_keys(unavailable.request()),
         t::ErrorCode::UpstreamUnavailable);
  Fixture begin;
  begin.state.fail_begin = true;
  expect(begin.app->get_verification_keys(begin.request()), t::ErrorCode::UpstreamUnavailable);
  assert(begin.material.calls == 0);
  Fixture time;
  time.state.fail_time = true;
  expect(time.app->get_verification_keys(time.request()), t::ErrorCode::UpstreamUnavailable);
  Fixture read;
  read.state.fail_read = true;
  expect(read.app->get_verification_keys(read.request()), t::ErrorCode::UpstreamUnavailable);
  Fixture expired;
  expired.material.keys.keys[0].valid_until = expired.state.now;
  expect(expired.app->get_verification_keys(expired.request()), t::ErrorCode::UpstreamUnavailable);
}
void final_authority_time_and_expiry_rechecks() {
  for (unsigned scenario = 0; scenario < 5; ++scenario) {
    Fixture f;
    auto request = f.request();
    unsigned final_time_calls = 0;
    std::function<void()> advance_in_final;
    advance_in_final = [&] {
      if (++final_time_calls == 1) {
        f.state.before_time = advance_in_final;
      } else {
        f.state.now += std::chrono::seconds{2};
      }
    };
    f.material.after_snapshot = [&] {
      switch (scenario) {
      case 0:
        ++f.state.credential.ref.version.value;
        break;
      case 1:
        ++f.state.authorization.version.value;
        break;
      case 2:
        f.state.before_time = advance_in_final;
        break;
      case 3:
        f.state.now = request.context.deadline;
        break;
      case 4:
        f.material.keys.keys[0].valid_until = f.state.now;
        break;
      default:
        throw std::logic_error("unexpected scenario");
      }
    };
    const auto result = f.app->get_verification_keys(request);
    constexpr t::ErrorCode codes[] = {t::ErrorCode::Unauthenticated, t::ErrorCode::Conflict,
                                      t::ErrorCode::Timeout, t::ErrorCode::Timeout,
                                      t::ErrorCode::UpstreamUnavailable};
    expect(result, codes[scenario]);
  }
  for (unsigned dependency = 0; dependency < 3; ++dependency) {
    Fixture f;
    f.material.after_snapshot = [&] {
      if (dependency == 0) {
        f.state.fail_begin = true;
      } else if (dependency == 1) {
        f.state.fail_time = true;
      } else {
        f.state.fail_read = true;
      }
    };
    expect(f.app->get_verification_keys(f.request()), t::ErrorCode::UpstreamUnavailable);
  }
  Fixture revoked_grant;
  revoked_grant.material.after_snapshot = [&] { revoked_grant.state.assignments.clear(); };
  expect(revoked_grant.app->get_verification_keys(revoked_grant.request(t::ResourceVersion{999})),
         t::ErrorCode::PermissionDenied);
  Fixture expiring;
  const auto expiring_request = expiring.request();
  expiring.material.keys.keys[0].valid_until = expiring.state.now + std::chrono::milliseconds{500};
  unsigned samples = 0;
  std::function<void()> advance_to_expiry;
  advance_to_expiry = [&] {
    if (++samples == 1) {
      expiring.state.before_time = advance_to_expiry;
    } else {
      expiring.state.now += std::chrono::milliseconds{500};
    }
  };
  expiring.material.after_snapshot = [&] { expiring.state.before_time = advance_to_expiry; };
  expect(expiring.app->get_verification_keys(expiring_request), t::ErrorCode::UpstreamUnavailable);
  Fixture fresh;
  const auto request = fresh.request();
  fresh.material.after_snapshot = [&] { fresh.state.now += std::chrono::milliseconds{500}; };
  const auto result = fresh.app->get_verification_keys(request);
  assert(std::holds_alternative<t::GetVerificationKeysReply>(result));
  assert(std::get<t::GetVerificationKeysReply>(result).checked_at == fresh.state.now);
}
} // namespace
int main() try {
  complete_keys_for_all_supported_known_versions();
  exact_grants_and_permission_precedence();
  peer_and_current_credential_denials();
  material_and_dependency_failures_never_fake_empty();
  final_authority_time_and_expiry_rechecks();
} catch (...) {
  return 1;
}
