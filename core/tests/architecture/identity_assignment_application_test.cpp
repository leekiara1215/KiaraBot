// clang-format off
#include "kiarabot/core/application/target/identity_assignment_application.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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
const std::string kUser = "20000000-0000-4000-8000-000000000001";
struct State {
  t::TimePoint now{std::chrono::seconds{1000}};
  std::vector<t::Principal> principals;
  std::vector<t::AuthorizationState> authorizations;
  std::vector<t::CredentialVersionRecord> credentials;
  std::vector<t::AssignmentStateRecord> assignments;
  std::vector<t::CommandReceipt> receipts;
  std::vector<t::AuditEntry> audits;
  std::optional<t::InvalidationStreamState> stream;
  std::vector<t::InvalidationRecord> invalidations;
  std::uint64_t random_counter = 0;
  int begins = 0, commits = 0, unexpected = 0;
  bool fail_fingerprint = false;
  void add_principal(const std::string& name, t::PrincipalKind kind) {
    const t::ResourceRef binding{kind == t::PrincipalKind::Human ? t::ResourceKind::Principal
                                                                 : t::ResourceKind::Principal,
                                 {name}};
    principals.push_back({t::SubjectId{name}, kind, binding, {1}, t::LifecycleState::Active});
    authorizations.push_back({t::SubjectId{name}, {5}, false});
  }
  void add_control_credential(const std::string& name, unsigned fingerprint_byte) {
    add_principal(name, t::PrincipalKind::PlatformService);
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
                           {t::ResourceKind::Principal, {name}},
                           now + std::chrono::hours{24}});
  }
  void grant(const std::string& id, const std::string& subject, t::Permission permission,
             const t::ResourceRef& resource, std::uint64_t version = 1) {
    t::AssignmentStateRecord record;
    record.assignment.id = t::AssignmentId{id};
    record.assignment.subject = t::SubjectId{subject};
    record.assignment.version = t::ResourceVersion{version};
    record.assignment.explicit_permissions.push_back({permission, resource});
    record.state = t::AssignmentState::Active;
    assignments.push_back(record);
  }
  void remove_grant(const std::string& id) {
    for (auto iterator = assignments.begin(); iterator != assignments.end(); ++iterator) {
      if (iterator->assignment.id.value == id) {
        assignments.erase(iterator);
        return;
      }
    }
  }
  const t::AssignmentStateRecord* find_state(const std::string& id) const {
    for (const auto& record : assignments) {
      if (record.assignment.id.value == id) {
        return &record;
      }
    }
    return nullptr;
  }
  t::AuthorizationState* find_authorization(const std::string& subject) {
    for (auto& authorization : authorizations) {
      if (authorization.subject.value == subject) {
        return &authorization;
      }
    }
    return nullptr;
  }
  State() {
    add_control_credential("control", 1);
    add_principal(kUser, t::PrincipalKind::Human);
    grant("control-manage-principal", "control", t::Permission::IdentityAssignmentManage,
          {t::ResourceKind::Principal, {kUser}});
    grant("control-read-principal", "control", t::Permission::IdentityAssignmentRead,
          {t::ResourceKind::Principal, {kUser}});
    grant("control-manage-model", "control", t::Permission::IdentityAssignmentManage,
          {t::ResourceKind::Model, {"model"}});
    grant("control-read-model", "control", t::Permission::IdentityAssignmentRead,
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
class Fingerprints final : public t::IdentityAssignmentFingerprintPort {
public:
  explicit Fingerprints(State& state) : state_(state) {}
  t::Outcome<t::Sha256Digest> set_assignment(const t::ExactAssignmentSpec& spec,
                                             std::optional<t::AssignmentId> target,
                                             std::optional<t::ResourceVersion> expected) override {
    if (state_.fail_fingerprint) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    t::Sha256Digest digest{};
    digest.bytes[0] = static_cast<std::byte>(1);
    digest.bytes[1] = static_cast<std::byte>(target ? 1 : 0);
    digest.bytes[2] = static_cast<std::byte>(expected ? 1 : 0);
    digest.bytes[3] = static_cast<std::byte>(spec.subject.value.size());
    if (const auto* role = std::get_if<t::RoleAssignmentSpec>(&spec.basis)) {
      digest.bytes[4] = static_cast<std::byte>(static_cast<int>(role->role));
      digest.bytes[5] = static_cast<std::byte>(role->scope.size());
    } else {
      const auto& explicit_spec = std::get<t::ExplicitAssignmentSpec>(spec.basis);
      digest.bytes[4] = static_cast<std::byte>(100 + explicit_spec.permissions.size());
      digest.bytes[5] = static_cast<std::byte>(explicit_spec.scope.size());
    }
    return digest;
  }
  t::Outcome<t::Sha256Digest> revoke_assignment(const t::AssignmentId& target,
                                                t::ResourceVersion expected) override {
    if (state_.fail_fingerprint) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    t::Sha256Digest digest{};
    digest.bytes[0] = static_cast<std::byte>(2);
    digest.bytes[1] = static_cast<std::byte>(target.value.size());
    digest.bytes[2] = static_cast<std::byte>(expected.value);
    return digest;
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
  t::Status save(const t::AuthorizationState& value,
                 std::optional<t::ResourceVersion> expected) override {
    auto* stored = state_.find_authorization(value.subject.value);
    if (!stored || !expected || stored->version != *expected) {
      return failure(t::ErrorCode::Conflict);
    }
    *stored = value;
    return std::monostate{};
  }

private:
  State& state_;
};
class AssignmentStore final : public t::IdentityExactAssignmentRepository {
public:
  explicit AssignmentStore(State& state) : state_(state) {}
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& subject) override {
    std::vector<t::ExactAssignment> result;
    for (const auto& record : state_.assignments) {
      if (record.assignment.subject == subject && record.state == t::AssignmentState::Active) {
        result.push_back(record.assignment);
      }
    }
    return result;
  }
  t::Outcome<std::optional<t::AssignmentStateRecord>> current(const t::AssignmentId& id) override {
    for (const auto& record : state_.assignments) {
      if (record.assignment.id == id) {
        return std::optional<t::AssignmentStateRecord>{record};
      }
    }
    return std::optional<t::AssignmentStateRecord>{};
  }
  t::Status save(const t::ExactAssignment& assignment,
                 std::optional<t::ResourceVersion> expected) override {
    return save_state({assignment, t::AssignmentState::Active, std::nullopt}, expected);
  }
  t::Status save_state(const t::AssignmentStateRecord& record,
                       std::optional<t::ResourceVersion> expected) override {
    if (std::holds_alternative<t::Failure>(t::validate_assignment_state(record))) {
      return failure();
    }
    for (auto& stored : state_.assignments) {
      if (stored.assignment.id != record.assignment.id) {
        continue;
      }
      if (!expected || stored.assignment.version != *expected ||
          stored.assignment.subject != record.assignment.subject ||
          stored.state == t::AssignmentState::Revoked) {
        return failure(t::ErrorCode::Conflict);
      }
      stored = record;
      return std::monostate{};
    }
    if (expected || record.assignment.version.value != 1 ||
        record.state != t::AssignmentState::Active) {
      return failure(t::ErrorCode::Conflict);
    }
    state_.assignments.push_back(record);
    return std::monostate{};
  }

private:
  State& state_;
};
class Receipts final : public t::ReceiptRepository {
public:
  explicit Receipts(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey& key) override {
    for (const auto& receipt : state_.receipts) {
      if (receipt.key == key) {
        return std::optional<t::CommandReceipt>{receipt};
      }
    }
    return std::optional<t::CommandReceipt>{};
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt& receipt,
                                     t::ReceiptCapacity) override {
    for (const auto& stored : state_.receipts) {
      if (!(stored.key == receipt.key)) {
        continue;
      }
      if (stored.fingerprint == receipt.fingerprint) {
        return t::ReceiptWrite{stored, true};
      }
      return failure(t::ErrorCode::Conflict);
    }
    state_.receipts.push_back(receipt);
    return t::ReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    ++state_.unexpected;
    return failure();
  }

private:
  State& state_;
};
class Journal final : public t::JournalRepository {
public:
  explicit Journal(State& state) : state_(state) {}
  t::Status append_audit(const t::AuditEntry& entry) override {
    state_.audits.push_back(entry);
    return std::monostate{};
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
  version(const t::CredentialVersionRef& ref) override {
    for (const auto& record : state_.credentials) {
      if (record.ref == ref) {
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
class Unused final : public t::IdentitySecretReceiptRepository,
                     public t::IdentityBootstrapRepository,
                     public t::Repository<t::OperationId, t::Operation>,
                     public t::Repository<t::AssignmentId, t::ResourceGrant> {
public:
  explicit Unused(State& state) : state_(state) {}
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

private:
  State& state_;
};
class UnusedSessions final : public t::IdentitySessionSecurityRepository {
public:
  explicit UnusedSessions(State& state) : state_(state) {}
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

private:
  State& state_;
};
class Metadata final : public t::Repository<t::SessionId, t::LoginSession> {
public:
  explicit Metadata(State& state) : state_(state) {}
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
class FakeInvalidations final : public t::IdentityInvalidationRepository {
public:
  explicit FakeInvalidations(State& state) : state_(state) {}
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
    if (state_.invalidations.empty()) {
      return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>{state_.invalidations.front().sequence};
  }
  t::Outcome<std::vector<t::InvalidationRecord>>
  read_after(const std::string& epoch, std::uint64_t sequence, std::size_t limit) override {
    std::vector<t::InvalidationRecord> result;
    for (const auto& record : state_.invalidations) {
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
    state_.invalidations.push_back(record);
    return record;
  }
  t::Outcome<std::size_t> prune(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  State& state_;
};
class Work final : public t::IdentitySecurityUnitOfWork {
public:
  explicit Work(State& state)
      : state_(state), principals_(state), authorizations_(state), assignments_(state),
        receipts_(state), journal_(state), credentials_(state), unused_(state),
        unused_sessions_(state), metadata_(state), invalidations_(state) {
    ++state_.begins;
  }
  ~Work() override {
    rollback();
  }
  t::Outcome<t::TimePoint> current_time() override {
    return state_.now;
  }
  t::ReceiptRepository& receipts() override {
    return receipts_;
  }
  t::JournalRepository& journal() override {
    return journal_;
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    return unused_;
  }
  t::Repository<t::SubjectId, t::Principal>& principals() override {
    return principals_;
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    return metadata_;
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
    return unused_sessions_;
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
  t::IdentityInvalidationRepository* invalidations() noexcept override {
    return &invalidations_;
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
  AssignmentStore assignments_;
  Receipts receipts_;
  Journal journal_;
  Credentials credentials_;
  Unused unused_;
  UnusedSessions unused_sessions_;
  Metadata metadata_;
  FakeInvalidations invalidations_;
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
  Fingerprints fingerprints{state};
  Transactions transactions{state};
  std::unique_ptr<t::IdentityAssignmentApplication> app;
  Fixture() {
    auto created = t::make_identity_assignment_application(
        transactions, fingerprints, random,
        {.maximum_snapshots = 16, .maximum_snapshot_records = 128});
    app = std::move(std::get<std::unique_ptr<t::IdentityAssignmentApplication>>(created));
  }
  t::VerifiedPeer control_peer() const {
    const auto& record = state.credentials.front();
    const auto& certificate = std::get<t::CertificateBinding>(record.material);
    return {{t::PlatformPeer{t::Service::Control, record.subject}, certificate, record.ref},
            state.now};
  }
  t::IdentityRequestContext context(const std::string& idempotency_key) const {
    return {"11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222",
            idempotency_key, state.now + std::chrono::seconds{1}};
  }
  t::SetAssignmentRequest set_request(const std::string& idempotency_key,
                                      const t::ExactAssignmentSpec& spec,
                                      std::optional<t::AssignmentId> id = std::nullopt,
                                      std::optional<t::ResourceVersion> expected = std::nullopt) {
    return {context(idempotency_key), control_peer(), spec, id, expected};
  }
  t::ListAssignmentsRequest list_request(const std::string& subject, std::uint32_t page_size,
                                         std::string page_token = {}, std::string snapshot_id = {},
                                         std::vector<t::ResourceRef> filter = {}) {
    return {context("list-key"),
            control_peer(),
            t::SubjectId{subject},
            t::AssignmentScopeFilter{std::move(filter)},
            {page_size, std::move(page_token), std::move(snapshot_id)}};
  }
  t::RevokeAssignmentRequest revoke_request(const std::string& idempotency_key,
                                            const std::string& id, std::uint64_t expected) {
    return {context(idempotency_key), control_peer(), t::AssignmentId{id},
            t::ResourceVersion{expected}};
  }
};
t::ExactAssignmentSpec role_spec(const std::string& subject, t::FixedRole role,
                                 std::vector<t::ResourceRef> scope) {
  t::ExactAssignmentSpec spec{t::SubjectId{subject}, t::RoleAssignmentSpec{role, std::move(scope)}};
  return spec;
}
template <typename Value> Value expect_ok(const t::Outcome<Value>& result) {
  assert(std::holds_alternative<Value>(result));
  return std::get<Value>(result);
}
template <typename Value> void expect_failure(const t::Outcome<Value>& result, t::ErrorCode code) {
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).code == code);
}
void create_and_replay() {
  Fixture fixture;
  const auto spec = role_spec(kUser, t::FixedRole::Viewer, {{t::ResourceKind::Principal, {kUser}}});
  const auto created = fixture.app->set_assignment(fixture.set_request("set-key-1", spec));
  const auto& reply = expect_ok(created);
  assert(reply.assignment.version.value == 1);
  assert(reply.assignment.id.value.size() == 36);
  assert(fixture.state.assignments.size() == 5);
  assert(fixture.state.find_authorization(kUser)->version.value == 6);
  assert(fixture.state.audits.size() == 1);
  assert(fixture.state.receipts.size() == 1);
  // Same idempotency key and same content returns the ORIGINAL success snapshot; no second
  // assignment version, audit row, receipt or authorization bump.
  const auto replayed = fixture.app->set_assignment(fixture.set_request("set-key-1", spec));
  const auto& replay = expect_ok(replayed);
  assert(replay.assignment.id == reply.assignment.id);
  assert(replay.assignment.version.value == 1);
  assert(fixture.state.find_authorization(kUser)->version.value == 6);
  assert(fixture.state.audits.size() == 1);
  // A changed request with the same idempotency key is Conflict, never a second charge.
  const auto changed =
      role_spec(kUser, t::FixedRole::Operator, {{t::ResourceKind::Model, {"model"}}});
  expect_failure(fixture.app->set_assignment(fixture.set_request("set-key-1", changed)),
                 t::ErrorCode::Conflict);
}
void update_revoke_and_tombstone() {
  Fixture fixture;
  const auto spec = role_spec(kUser, t::FixedRole::Viewer, {{t::ResourceKind::Principal, {kUser}}});
  const auto created =
      expect_ok(fixture.app->set_assignment(fixture.set_request("set-key-1", spec)));
  const auto id = created.assignment.id;
  const auto updated_spec =
      role_spec(kUser, t::FixedRole::Operator, {{t::ResourceKind::Principal, {kUser}}});
  const auto updated = expect_ok(fixture.app->set_assignment(
      fixture.set_request("set-key-2", updated_spec, id, t::ResourceVersion{1})));
  assert(updated.assignment.version.value == 2);
  assert(updated.assignment.roles.front().role == t::FixedRole::Operator);
  // Stale expected version is Conflict/ReadCurrent and changes nothing.
  expect_failure(fixture.app->set_assignment(
                     fixture.set_request("set-key-3", updated_spec, id, t::ResourceVersion{1})),
                 t::ErrorCode::Conflict);
  const auto revoked = expect_ok(
      fixture.app->revoke_assignment(fixture.revoke_request("revoke-key-1", id.value, 2)));
  assert(revoked.version.value == 3);
  const auto* tombstone = fixture.state.find_state(id.value);
  assert(tombstone && tombstone->state == t::AssignmentState::Revoked);
  assert(tombstone->assignment.version.value == 3);
  assert(tombstone->assignment.subject == t::SubjectId{kUser});
  // Revoked IDs never revive, not even with a correct-looking update or a fresh key.
  expect_failure(fixture.app->set_assignment(
                     fixture.set_request("set-key-4", spec, id, t::ResourceVersion{3})),
                 t::ErrorCode::Conflict);
  expect_failure(
      fixture.app->revoke_assignment(fixture.revoke_request("revoke-key-2", id.value, 3)),
      t::ErrorCode::Conflict);
  // Same-key revoke replay returns the original success version after current authentication.
  const auto replay = expect_ok(
      fixture.app->revoke_assignment(fixture.revoke_request("revoke-key-1", id.value, 2)));
  assert(replay.version.value == 3);
  assert(fixture.state.find_authorization(kUser)->version.value == 8);
}
void authority_and_input_rejections() {
  const auto spec = role_spec(kUser, t::FixedRole::Viewer, {{t::ResourceKind::Principal, {kUser}}});
  {
    Fixture fixture;
    // Remove the management grant: write authority is denied before any receipt/audit write.
    fixture.state.remove_grant("control-manage-principal");
    expect_failure(fixture.app->set_assignment(fixture.set_request("set-key-1", spec)),
                   t::ErrorCode::PermissionDenied);
  }
  {
    // Missing target principal is NotFound, not a fabricated empty or Internal result.
    Fixture fixture;
    const auto missing =
        role_spec("99999999-9999-4999-8999-999999999999", t::FixedRole::Viewer,
                  {{t::ResourceKind::Principal, {"99999999-9999-4999-8999-999999999999"}}});
    expect_failure(fixture.app->set_assignment(fixture.set_request("set-key-2", missing)),
                   t::ErrorCode::NotFound);
  }
  {
    Fixture fixture;
    // Malformed shape is rejected with the stable argument error.
    auto request = fixture.set_request("set-key-3", spec);
    request.assignment_id = t::AssignmentId{"not-a-uuid"};
    request.expected_version = t::ResourceVersion{1};
    expect_failure(fixture.app->set_assignment(request), t::ErrorCode::InvalidArgument);
    request = fixture.set_request("set-key-4", spec);
    request.expected_version = t::ResourceVersion{1};
    expect_failure(fixture.app->set_assignment(request), t::ErrorCode::InvalidArgument);
    expect_failure(fixture.app->revoke_assignment(fixture.revoke_request(
                       "revoke-key-1", "33333333-3333-4333-8333-333333333333", 0)),
                   t::ErrorCode::InvalidArgument);
  }
  {
    // Fingerprint/dependency failures are never converted into an all-zero success digest.
    Fixture fixture;
    fixture.state.fail_fingerprint = true;
    expect_failure(fixture.app->set_assignment(fixture.set_request("set-key-5", spec)),
                   t::ErrorCode::UpstreamUnavailable);
  }
}
void list_pages_and_current_authority() {
  Fixture fixture;
  const auto base = role_spec(kUser, t::FixedRole::Viewer, {{t::ResourceKind::Principal, {kUser}}});
  expect_ok(fixture.app->set_assignment(fixture.set_request("set-key-1", base)));
  expect_ok(fixture.app->set_assignment(
      fixture.set_request("set-key-2", role_spec(kUser, t::FixedRole::Operator,
                                                 {{t::ResourceKind::Model, {"model"}}}))));
  expect_ok(fixture.app->set_assignment(fixture.set_request(
      "set-key-3", role_spec(kUser, t::FixedRole::Viewer, {{t::ResourceKind::Model, {"model"}}}))));
  // The actor's own grants are included by the fixture state; page over only the target user's
  // assignments by filtering to resources outside the actor's own grant set.
  const auto first = expect_ok(fixture.app->list_assignments(
      fixture.list_request(kUser, 0, {}, {}, {{t::ResourceKind::Principal, {kUser}}})));
  assert(first.page_size == 20);
  assert(first.assignments.size() == 1);
  assert(first.next_page_token.empty());
  const auto all = expect_ok(fixture.app->list_assignments(fixture.list_request(kUser, 2)));
  assert(all.assignments.size() == 2);
  assert(!all.next_page_token.empty());
  const auto second = expect_ok(fixture.app->list_assignments(
      fixture.list_request(kUser, 0, all.next_page_token, all.snapshot_id)));
  assert(second.page_size == 2);
  assert(!second.assignments.empty());
  // A changed authorization watermark between pages is Conflict/ReadCurrent, not a stale page.
  fixture.state.find_authorization(kUser)->version = t::ResourceVersion{42};
  expect_failure(fixture.app->list_assignments(
                     fixture.list_request(kUser, 2, all.next_page_token, all.snapshot_id)),
                 t::ErrorCode::Conflict);
  // An actor without assignment.read is denied before any snapshot is read.
  Fixture unprivileged;
  unprivileged.state.remove_grant("control-read-principal");
  expect_failure(unprivileged.app->list_assignments(unprivileged.list_request(kUser, 10)),
                 t::ErrorCode::PermissionDenied);
}
} // namespace

int main() {
  create_and_replay();
  update_revoke_and_tombstone();
  authority_and_input_rejections();
  list_pages_and_current_authority();
  return 0;
}
