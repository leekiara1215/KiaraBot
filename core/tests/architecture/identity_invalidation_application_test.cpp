// clang-format off
#include "kiarabot/core/application/target/identity_invalidation_application.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace {
namespace t = kiarabot::core::target;
t::Failure failure(t::ErrorCode code = t::ErrorCode::Internal) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
class Bytes final : public t::SecretBuffer {
public:
  Bytes(std::size_t count, unsigned char seed) {
    count_ = count;
    for (std::size_t index = 0; index < count && index < data_.size(); ++index) {
      data_[index] = static_cast<std::byte>((seed + index) & 0xffU);
    }
  }
  std::span<const std::byte> bytes() const noexcept override {
    return {data_.data(), count_};
  }

private:
  std::array<std::byte, 64> data_{};
  std::size_t count_ = 0;
};
struct State {
  t::TimePoint now{std::chrono::seconds{1000}};
  std::vector<t::Principal> principals;
  std::vector<t::AuthorizationState> authorizations;
  std::vector<t::CredentialVersionRecord> credentials;
  std::vector<t::AssignmentStateRecord> assignments;
  std::optional<t::InvalidationStreamState> stream;
  std::vector<t::InvalidationRecord> log;
  std::uint64_t random_counter = 0;
  int begins = 0, commits = 0, unexpected = 0;
  bool no_store = false;
  void add_control(const std::string& name, unsigned fingerprint_byte) {
    const t::ResourceRef binding{t::ResourceKind::Principal, {name}};
    principals.push_back({t::SubjectId{name},
                          t::PrincipalKind::PlatformService,
                          binding,
                          {1},
                          t::LifecycleState::Active});
    authorizations.push_back({t::SubjectId{name}, {5}, false});
    t::CertificateBinding certificate{t::SubjectId{name},
                                      {},
                                      std::string{"spiffe://"} + name,
                                      t::Service::Control,
                                      now - std::chrono::hours{1},
                                      now + std::chrono::hours{24}};
    certificate.fingerprint.bytes[0] = static_cast<std::byte>(fingerprint_byte);
    credentials.push_back({{{name + "-credential"}, {1}},
                           t::SubjectId{name},
                           certificate,
                           t::CredentialState::Active,
                           now - std::chrono::hours{1},
                           {},
                           binding,
                           now + std::chrono::hours{24}});
  }
  void grant(const std::string& id, const std::string& subject, t::Permission permission,
             const t::ResourceRef& resource) {
    t::AssignmentStateRecord record;
    record.assignment.id = t::AssignmentId{id};
    record.assignment.subject = t::SubjectId{subject};
    record.assignment.version = t::ResourceVersion{1};
    record.assignment.explicit_permissions.push_back({permission, resource});
    record.state = t::AssignmentState::Active;
    assignments.push_back(record);
  }
  State() {
    add_control("control", 1);
    grant("invalidation-read-model", "control", t::Permission::IdentityInvalidationRead,
          {t::ResourceKind::Model, {"model"}});
  }
};
class Random final : public t::RandomPort {
public:
  explicit Random(State& state) : state_(state) {}
  t::Outcome<t::OwnedSecret> bytes(std::size_t count) override {
    return std::make_unique<Bytes>(count, static_cast<unsigned char>(++state_.random_counter));
  }
  t::Outcome<t::OwnedSecret> opaque_token() override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class Principals final : public t::Repository<t::SubjectId, t::Principal> {
public:
  explicit Principals(State& state) : state_(state) {}
  t::Outcome<std::optional<t::Principal>> find(const t::SubjectId& key) override {
    for (const auto& principal : state_.principals) {
      if (principal.id == key) {
        return std::optional<t::Principal>{principal};
      }
    }
    return std::optional<t::Principal>{};
  }
  t::Status save(const t::Principal&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class Authorizations final : public t::Repository<t::SubjectId, t::AuthorizationState> {
public:
  explicit Authorizations(State& state) : state_(state) {}
  t::Outcome<std::optional<t::AuthorizationState>> find(const t::SubjectId& key) override {
    for (const auto& authorization : state_.authorizations) {
      if (authorization.subject == key) {
        return std::optional<t::AuthorizationState>{authorization};
      }
    }
    return std::optional<t::AuthorizationState>{};
  }
  t::Status save(const t::AuthorizationState&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
std::vector<t::ResourceRef> scope_of(const t::ExactAssignment& assignment) {
  std::vector<t::ResourceRef> scope;
  for (const auto& binding : assignment.roles) {
    if (std::find(scope.begin(), scope.end(), binding.resource) == scope.end()) {
      scope.push_back(binding.resource);
    }
  }
  for (const auto& binding : assignment.explicit_permissions) {
    if (std::find(scope.begin(), scope.end(), binding.resource) == scope.end()) {
      scope.push_back(binding.resource);
    }
  }
  return scope;
}
class Assignments final : public t::IdentityExactAssignmentRepository {
public:
  explicit Assignments(State& state) : state_(state) {}
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& subject) override {
    std::vector<t::ExactAssignment> result;
    for (const auto& record : state_.assignments) {
      if (record.assignment.subject == subject && record.state == t::AssignmentState::Active) {
        result.push_back(record.assignment);
      }
    }
    return result;
  }
  t::Outcome<std::vector<t::ExactAssignment>>
  for_scope(std::span<const t::ResourceRef> scope) override {
    std::vector<t::ExactAssignment> result;
    for (const auto& record : state_.assignments) {
      if (record.state != t::AssignmentState::Active) {
        continue;
      }
      for (const auto& resource : scope_of(record.assignment)) {
        if (std::find(scope.begin(), scope.end(), resource) != scope.end()) {
          result.push_back(record.assignment);
          break;
        }
      }
    }
    return result;
  }
  t::Outcome<std::optional<t::AssignmentStateRecord>> current(const t::AssignmentId&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save(const t::ExactAssignment&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save_state(const t::AssignmentStateRecord&,
                       std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class Invalidations final : public t::IdentityInvalidationRepository {
public:
  explicit Invalidations(State& state) : state_(state) {}
  t::Outcome<std::optional<t::InvalidationStreamState>> state() override {
    return state_.stream;
  }
  t::Status initialize(const t::InvalidationStreamState& stream) override {
    if (!state_.stream) {
      state_.stream = stream;
    }
    return std::monostate{};
  }
  t::Outcome<std::optional<std::uint64_t>> oldest_sequence(const std::string&) override {
    if (state_.log.empty()) {
      return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>{state_.log.front().sequence};
  }
  t::Outcome<std::vector<t::InvalidationRecord>>
  read_after(const std::string& epoch, std::uint64_t sequence, std::size_t limit) override {
    std::vector<t::InvalidationRecord> result;
    for (const auto& record : state_.log) {
      if (record.epoch == epoch && record.sequence > sequence && result.size() < limit) {
        result.push_back(record);
      }
    }
    return result;
  }
  t::Outcome<t::InvalidationRecord> append(const t::InvalidationEvent& event,
                                           t::TimePoint now) override {
    if (!state_.stream) {
      return failure(t::ErrorCode::Conflict);
    }
    const auto sequence = ++state_.stream->next_sequence;
    const t::InvalidationRecord record{state_.stream->epoch, sequence, event, now};
    state_.log.push_back(record);
    return record;
  }
  t::Outcome<std::size_t> prune(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  State& state_;
};
class Credentials final : public t::IdentityCredentialRepository {
public:
  explicit Credentials(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    for (const auto& record : state_.credentials) {
      if (record.ref.id == id) {
        return std::optional<t::CredentialVersionRecord>{record};
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest& fingerprint) override {
    for (const auto& record : state_.credentials) {
      const auto* certificate = std::get_if<t::CertificateBinding>(&record.material);
      if (certificate && certificate->fingerprint == fingerprint) {
        return std::optional<t::CredentialVersionRecord>{record};
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
  by_bootstrap(const t::TokenVerifier&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_platform(const t::PlatformPeer&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::LoginIdentity>> login(const std::string&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save_login(const t::LoginIdentity&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save(const t::CredentialVersionRecord&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class UnusedRepositories final : public t::ReceiptRepository,
                                 public t::JournalRepository,
                                 public t::Repository<t::OperationId, t::Operation>,
                                 public t::Repository<t::AssignmentId, t::ResourceGrant>,
                                 public t::Repository<t::SessionId, t::LoginSession> {
public:
  explicit UnusedRepositories(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt&, t::ReceiptCapacity) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status append_audit(const t::AuditEntry&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::Operation>> find(const t::OperationId&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save(const t::Operation&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::ResourceGrant>> find(const t::AssignmentId&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save(const t::ResourceGrant&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::LoginSession>> find(const t::SessionId&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save(const t::LoginSession&, std::optional<t::ResourceVersion>) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class UnusedUnavailable final : public t::IdentitySessionSecurityRepository,
                                public t::IdentitySecretReceiptRepository,
                                public t::IdentityBootstrapRepository {
public:
  explicit UnusedUnavailable(State& state) : state_(state) {}
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_access(const t::TokenVerifier&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_refresh(const t::TokenVerifier&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::RefreshHandle>>
  resolve_refresh(const t::TokenVerifier&, const t::RefreshRecoveryLookup&, t::TimePoint) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status record_refresh_consumption(const t::ConsumedRefreshHandle&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::size_t> count_unexpired() override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::vector<t::SessionMetadata>> list_metadata(const t::SubjectId&,
                                                            std::size_t) override {
    ++state_.unexpected;
    return failure();
  }
  t::Status save(const t::SessionSecurity&, std::optional<t::ResourceVersion>,
                 std::optional<std::uint64_t>) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::optional<t::EncryptedSecretReceipt>> find(const t::SecretReceiptScope&) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::SecretReceiptWrite> insert(const t::EncryptedSecretReceipt&,
                                           t::SecretReceiptCapacity) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    ++state_.unexpected;
    return failure();
  }
  t::Outcome<t::AdministratorBootstrap> state() override {
    ++state_.unexpected;
    return failure();
  }
  t::Status consume(const t::AdministratorBootstrap&, t::ResourceVersion) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class Work final : public t::IdentitySecurityUnitOfWork {
public:
  explicit Work(State& state)
      : state_(state), principals_(state), authorizations_(state), assignments_(state),
        invalidations_(state), credentials_(state), unused_(state), unavailable_(state) {
    ++state_.begins;
  }
  ~Work() override {
    rollback();
  }
  t::Outcome<t::TimePoint> current_time() override {
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
    return principals_;
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    return unused_;
  }
  t::Repository<t::AssignmentId, t::ResourceGrant>& grants() override {
    return unused_;
  }
  t::Repository<t::SubjectId, t::AuthorizationState>& authorizations() override {
    return authorizations_;
  }
  t::IdentityCredentialRepository& credentials() override {
    return credentials_;
  }
  t::IdentitySessionSecurityRepository& session_security() override {
    return unavailable_;
  }
  t::IdentityExactAssignmentRepository& exact_assignments() override {
    return assignments_;
  }
  t::IdentitySecretReceiptRepository& secret_receipts() override {
    return unavailable_;
  }
  t::IdentityBootstrapRepository& bootstrap() override {
    return unavailable_;
  }
  t::IdentityInvalidationRepository* invalidations() noexcept override {
    return state_.no_store ? nullptr : &invalidations_;
  }
  t::CommitResult commit() override {
    ++state_.commits;
    rollback();
    return t::CommitResult::Committed;
  }
  void rollback() noexcept override {
    if (active_) {
      active_ = false;
    }
  }

private:
  State& state_;
  bool active_ = true;
  Principals principals_;
  Authorizations authorizations_;
  Assignments assignments_;
  Invalidations invalidations_;
  Credentials credentials_;
  UnusedRepositories unused_;
  UnusedUnavailable unavailable_;
};
class Transactions final : public t::IdentitySecurityUnitOfWorkFactory {
public:
  explicit Transactions(State& state) : state_(state) {}
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    return std::unique_ptr<t::IdentitySecurityUnitOfWork>{std::make_unique<Work>(state_)};
  }

private:
  State& state_;
};
struct Fixture {
  State state;
  Random random{state};
  Transactions transactions{state};
  std::unique_ptr<t::IdentityInvalidationApplication> app;
  Fixture() {
    auto created = t::make_identity_invalidation_application(transactions, random, {.batch = 32});
    app = std::move(std::get<std::unique_ptr<t::IdentityInvalidationApplication>>(created));
  }
  t::VerifiedPeer control_peer() const {
    const auto& record = state.credentials.front();
    const auto& certificate = std::get<t::CertificateBinding>(record.material);
    return {{t::PlatformPeer{t::Service::Control, record.subject}, certificate, record.ref},
            state.now};
  }
  t::IdentityRequestContext context() const {
    return {"11111111-1111-4111-8111-111111111111",
            "22222222-2222-4222-8222-222222222222",
            {},
            state.now + std::chrono::seconds{1}};
  }
  std::vector<t::ResourceRef> scope() const {
    return {{t::ResourceKind::Model, {"model"}}};
  }
  t::InvalidationWatchRequest watch(t::InvalidationCursor cursor) const {
    return {context(), control_peer(), scope(), std::move(cursor)};
  }
  t::AuthorizationSnapshotRequest snapshot() const {
    return {context(), control_peer(), scope(), {}};
  }
};
t::InvalidationEvent event(const std::string& principal, std::uint64_t version) {
  return {t::SubjectId{principal},
          t::ResourceVersion{version},
          {{t::ResourceKind::Model, {"model"}}},
          false};
}
template <typename Value> Value expect_ok(const t::Outcome<Value>& result) {
  assert(std::holds_alternative<Value>(result));
  return std::get<Value>(result);
}
template <typename Value> void expect_failure(const t::Outcome<Value>& result, t::ErrorCode code) {
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).code == code);
}
void snapshot_and_watch() {
  Fixture fixture;
  const auto snapshot = expect_ok(fixture.app->snapshot(fixture.snapshot()));
  assert(snapshot.assignments.size() == 1);
  assert(snapshot.cursor.source_service == t::kInvalidationSourceService);
  assert(!snapshot.cursor.epoch.empty());
  assert(snapshot.cursor.sequence == 0);
  assert(snapshot.authorization_version.value == 5);
  assert(snapshot.checked_at > t::TimePoint{});
  // Fresh subscription without a cursor always needs a snapshot first.
  const auto initial = expect_ok(fixture.app->open_watch(
      fixture.watch({std::string{t::kInvalidationSourceService}, {}, fixture.scope(), 0})));
  assert(initial.resync_required);
  assert(initial.watermark.epoch == snapshot.cursor.epoch);
  assert(initial.watermark.sequence == 0);
  // Append two committed invalidations through the shared transaction helper.
  auto work =
      std::get<std::unique_ptr<t::IdentitySecurityUnitOfWork>>(fixture.transactions.begin());
  expect_ok(
      t::append_identity_invalidation(*work, fixture.random, event("user", 6), fixture.state.now));
  expect_ok(t::append_identity_invalidation(*work, fixture.random, event("user", 7),
                                            fixture.state.now + std::chrono::seconds{1}));
  assert(work->commit() == t::CommitResult::Committed);
  // A cursor at the snapshot watermark receives both records without resync.
  const auto resumed = expect_ok(fixture.app->open_watch(fixture.watch(
      {std::string{t::kInvalidationSourceService}, snapshot.cursor.epoch, fixture.scope(), 0})));
  assert(!resumed.resync_required);
  assert(resumed.backlog.size() == 2);
  assert(resumed.backlog.front().sequence == 1);
  assert(resumed.backlog.back().event.authorization_version.value == 7);
  const auto polled = expect_ok(fixture.app->poll(
      {fixture.context(), fixture.control_peer(), fixture.scope(), snapshot.cursor.epoch, 1, 32}));
  assert(!polled.resync_required);
  assert(polled.records.size() == 1 && polled.records.front().sequence == 2);
}
void resync_and_rejections() {
  Fixture fixture;
  expect_ok(fixture.app->snapshot(fixture.snapshot()));
  const auto epoch = fixture.state.stream->epoch;
  // A retained-record gap (oldest retained newer than cursor+1) forces a full snapshot.
  fixture.state.log.push_back({epoch, 10, event("user", 9), fixture.state.now});
  fixture.state.stream->next_sequence = 10;
  const auto gap = expect_ok(fixture.app->open_watch(
      fixture.watch({std::string{t::kInvalidationSourceService}, epoch, fixture.scope(), 3})));
  assert(gap.resync_required);
  // A different/unknown epoch is a resync signal, not silently empty continuity.
  const auto epoch_mismatch = expect_ok(fixture.app->open_watch(fixture.watch(
      {std::string{t::kInvalidationSourceService}, "other-epoch", fixture.scope(), 10})));
  assert(epoch_mismatch.resync_required);
  // A future sequence is caller error, never resync or truncation.
  expect_failure(fixture.app->open_watch(fixture.watch(
                     {std::string{t::kInvalidationSourceService}, epoch, fixture.scope(), 99})),
                 t::ErrorCode::InvalidArgument);
  // Missing subscription authority is rechecked against current exact grants, not cached.
  Fixture denied;
  denied.state.assignments.clear();
  expect_failure(denied.app->snapshot(denied.snapshot()), t::ErrorCode::PermissionDenied);
  expect_failure(denied.app->open_watch(denied.watch({})), t::ErrorCode::PermissionDenied);
  // Invalid cursor shapes are stable argument errors.
  expect_failure(fixture.app->open_watch(
                     fixture.watch({std::string{"other-service"}, epoch, fixture.scope(), 1})),
                 t::ErrorCode::InvalidArgument);
}
void missing_store_fails_closed() {
  Fixture fixture;
  fixture.state.no_store = true;
  expect_failure(fixture.app->snapshot(fixture.snapshot()), t::ErrorCode::UpstreamUnavailable);
  expect_failure(fixture.app->open_watch(fixture.watch({})), t::ErrorCode::UpstreamUnavailable);
}
} // namespace

int main() {
  snapshot_and_watch();
  resync_and_rejections();
  missing_store_fails_closed();
  return 0;
}
