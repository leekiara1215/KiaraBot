#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/application/target/identity_principal_application.hpp"
#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
#include "kiarabot/core/domain/target/identity_state_rules.hpp"

namespace t = kiarabot::core::target;
using std::chrono_literals::operator""h;
using std::chrono_literals::operator""s;

void check(bool value, std::string_view name) {
  if (!value) {
    std::cerr << name << '\n';
    std::exit(1);
  }
}
template <typename Value> bool failure_is(const t::Outcome<Value>& result, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&result);
  return failure && failure->code == code;
}
t::TimePoint at(std::chrono::hours offset) {
  return t::TimePoint{std::chrono::hours{1000} + offset};
}
// Insecure allocation is a TEST secret buffer only, never a production crypto adapter.
class FixtureSecret final : public t::SecretBuffer {
public:
  explicit FixtureSecret(std::string value) : bytes_(value.size()) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      bytes_[index] = static_cast<std::byte>(value[index]);
    }
  }
  std::span<const std::byte> bytes() const noexcept override {
    return bytes_;
  }

private:
  std::vector<std::byte> bytes_;
};
std::string text(t::SecretView value) {
  std::string result;
  result.reserve(value.size());
  for (const auto byte : value) {
    result.push_back(static_cast<char>(std::to_integer<unsigned>(byte)));
  }
  return result;
}
template <typename Id> struct IdLess {
  bool operator()(const Id& left, const Id& right) const {
    return left.value < right.value;
  }
};
template <typename Id, typename Value> using IdMap = std::map<Id, Value, IdLess<Id>>;
const t::SubjectId kActor{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
const t::CredentialId kActorCredential{"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"};
const t::ResourceRef kAdapterResource{t::ResourceKind::Adapter, {"adapter-one"}};
const t::ResourceRef kOtherResource{t::ResourceKind::Adapter, {"adapter-two"}};
t::Sha256Digest fingerprint(std::byte seed) {
  t::Sha256Digest result{};
  result.bytes[0] = seed;
  return result;
}
struct Store {
  t::TimePoint now = at(0h);
  IdMap<t::SubjectId, t::Principal> principals;
  IdMap<t::CredentialId, t::CredentialVersionRecord> credential_heads;
  std::vector<t::CommandReceipt> receipts;
  std::vector<t::EncryptedSecretReceipt> secret_receipts;
  IdMap<t::SubjectId, t::AuthorizationState> authorizations;
  std::vector<t::ExactAssignment> assignments;
  std::vector<t::AuditEntry> audits;
  std::vector<t::TimePoint> now_script;
  std::size_t now_index = 0;
  t::CommitResult commit_result = t::CommitResult::Committed;
  bool fail_audit = false;
  bool fail_receipt_insert = false;
  bool fail_secret_receipt_insert = false;
  int commits = 0;
  int rollbacks = 0;
};
t::VerifiedPeer actor_peer(const Store& store) {
  const auto& head = store.credential_heads.at(kActorCredential);
  const auto* certificate = std::get_if<t::CertificateBinding>(&head.material);
  if (!certificate) {
    std::abort();
  }
  return t::VerifiedPeer{
      t::RegisteredPeer{t::PlatformPeer{t::Service::Control, kActor}, *certificate, head.ref},
      store.now};
}
class Principals final : public t::Repository<t::SubjectId, t::Principal> {
public:
  explicit Principals(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::Principal>> find(const t::SubjectId& key) override {
    const auto found = store_.principals.find(key);
    if (found == store_.principals.end()) {
      return std::optional<t::Principal>{};
    }
    return std::optional<t::Principal>{found->second};
  }
  t::Status save(const t::Principal& value, std::optional<t::ResourceVersion> expected) override {
    const auto found = store_.principals.find(value.id);
    if (!expected && found != store_.principals.end()) {
      return t::rejected(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
    }
    if (expected && (found == store_.principals.end() || found->second.version != *expected)) {
      return t::rejected(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
    }
    // Identity private v1 uniqueness: one Principal per exact binding, independent of the
    // request idempotency key.
    if (value.binding) {
      for (const auto& [id, existing] : store_.principals) {
        if (id != value.id && existing.binding == value.binding) {
          return t::rejected(t::ErrorCode::Conflict, t::Recovery::QuerySameIdentity);
        }
      }
    }
    store_.principals[value.id] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};
class Credentials final : public t::IdentityCredentialRepository {
public:
  explicit Credentials(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    const auto found = store_.credential_heads.find(id);
    if (found == store_.credential_heads.end()) {
      return std::optional<t::CredentialVersionRecord>{};
    }
    return std::optional<t::CredentialVersionRecord>{found->second};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  version(const t::CredentialVersionRef& ref) override {
    return current(ref.id);
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest& leaf) override {
    for (const auto& [id, record] : store_.credential_heads) {
      (void)id;
      const auto* certificate = std::get_if<t::CertificateBinding>(&record.material);
      if (certificate && certificate->fingerprint == leaf) {
        return std::optional<t::CredentialVersionRecord>{record};
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_bootstrap(const t::TokenVerifier&) override {
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_platform(const t::PlatformPeer&) override {
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::LoginIdentity>> login(const std::string&) override {
    return std::optional<t::LoginIdentity>{};
  }
  t::Status save_login(const t::LoginIdentity&, std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }
  t::Status save(const t::CredentialVersionRecord& value,
                 std::optional<t::ResourceVersion> expected) override {
    const auto found = store_.credential_heads.find(value.ref.id);
    if (!expected && found != store_.credential_heads.end()) {
      return t::rejected(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
    }
    if (expected &&
        (found == store_.credential_heads.end() || found->second.ref.version != *expected)) {
      return t::rejected(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
    }
    store_.credential_heads[value.ref.id] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};
class BaseSessions final : public t::Repository<t::SessionId, t::LoginSession> {
public:
  t::Outcome<std::optional<t::LoginSession>> find(const t::SessionId&) override {
    return std::optional<t::LoginSession>{};
  }
  t::Status save(const t::LoginSession&, std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }
};
class Sessions final : public t::IdentitySessionSecurityRepository {
public:
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId&) override {
    return std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_access(const t::TokenVerifier&) override {
    return std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_refresh(const t::TokenVerifier&) override {
    return std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::RefreshHandle>>
  resolve_refresh(const t::TokenVerifier&, const t::RefreshRecoveryLookup&, t::TimePoint) override {
    return std::optional<t::RefreshHandle>{};
  }
  t::Status record_refresh_consumption(const t::ConsumedRefreshHandle&) override {
    return std::monostate{};
  }
  t::Outcome<std::vector<t::SessionMetadata>> list_metadata(const t::SubjectId&,
                                                            std::size_t) override {
    return std::vector<t::SessionMetadata>{};
  }
  t::Outcome<std::size_t> count_unexpired() override {
    return std::size_t{0};
  }
  t::Status save(const t::SessionSecurity&, std::optional<t::ResourceVersion>,
                 std::optional<std::uint64_t>) override {
    return std::monostate{};
  }
};
class Grants final : public t::Repository<t::AssignmentId, t::ResourceGrant> {
public:
  t::Outcome<std::optional<t::ResourceGrant>> find(const t::AssignmentId&) override {
    return std::optional<t::ResourceGrant>{};
  }
  t::Status save(const t::ResourceGrant&, std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }
};
class Authorizations final : public t::Repository<t::SubjectId, t::AuthorizationState> {
public:
  explicit Authorizations(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::AuthorizationState>> find(const t::SubjectId& key) override {
    const auto found = store_.authorizations.find(key);
    if (found == store_.authorizations.end()) {
      return std::optional<t::AuthorizationState>{};
    }
    return std::optional<t::AuthorizationState>{found->second};
  }
  t::Status save(const t::AuthorizationState&, std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }

private:
  Store& store_;
};
class Assignments final : public t::IdentityExactAssignmentRepository {
public:
  explicit Assignments(Store& store) : store_(store) {}
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& subject) override {
    std::vector<t::ExactAssignment> result;
    for (const auto& assignment : store_.assignments) {
      if (assignment.subject == subject) {
        result.push_back(assignment);
      }
    }
    return result;
  }
  t::Outcome<std::optional<t::AssignmentStateRecord>> current(const t::AssignmentId&) override {
    return std::optional<t::AssignmentStateRecord>{};
  }
  t::Status save_state(const t::AssignmentStateRecord&,
                       std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }
  t::Status save(const t::ExactAssignment&, std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }

private:
  Store& store_;
};
class Receipts final : public t::ReceiptRepository {
public:
  explicit Receipts(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey& key) override {
    for (const auto& receipt : store_.receipts) {
      if (receipt.key == key) {
        return std::optional<t::CommandReceipt>{receipt};
      }
    }
    return std::optional<t::CommandReceipt>{};
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt& receipt,
                                     t::ReceiptCapacity) override {
    if (store_.fail_receipt_insert) {
      return t::rejected(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    for (const auto& stored : store_.receipts) {
      if (stored.key == receipt.key) {
        if (stored.fingerprint != receipt.fingerprint) {
          return t::rejected(t::ErrorCode::Conflict);
        }
        return t::ReceiptWrite{stored, true};
      }
    }
    store_.receipts.push_back(receipt);
    return t::ReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class Journal final : public t::JournalRepository {
public:
  explicit Journal(Store& store) : store_(store) {}
  t::Status append_audit(const t::AuditEntry& entry) override {
    if (store_.fail_audit) {
      return t::rejected(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    store_.audits.push_back(entry);
    return std::monostate{};
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    return std::monostate{};
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    return std::vector<t::OutboxClaim>{};
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    return std::monostate{};
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    return std::optional<t::InboxRecord>{};
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    return t::rejected(t::ErrorCode::Internal);
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class Operations final : public t::Repository<t::OperationId, t::Operation> {
public:
  t::Outcome<std::optional<t::Operation>> find(const t::OperationId&) override {
    return std::optional<t::Operation>{};
  }
  t::Status save(const t::Operation&, std::optional<t::ResourceVersion>) override {
    return std::monostate{};
  }
};
class SecretReceipts final : public t::IdentitySecretReceiptRepository {
public:
  explicit SecretReceipts(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::EncryptedSecretReceipt>>
  find(const t::SecretReceiptScope& scope) override {
    for (const auto& receipt : store_.secret_receipts) {
      if (receipt.binding.scope == scope) {
        return std::optional<t::EncryptedSecretReceipt>{receipt};
      }
    }
    return std::optional<t::EncryptedSecretReceipt>{};
  }
  t::Outcome<t::SecretReceiptWrite> insert(const t::EncryptedSecretReceipt& receipt,
                                           t::SecretReceiptCapacity) override {
    if (store_.fail_secret_receipt_insert) {
      return t::rejected(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    for (const auto& stored : store_.secret_receipts) {
      if (stored.binding.scope == receipt.binding.scope) {
        return t::rejected(t::ErrorCode::Conflict);
      }
    }
    store_.secret_receipts.push_back(receipt);
    return t::SecretReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class Bootstrap final : public t::IdentityBootstrapRepository {
public:
  t::Outcome<t::AdministratorBootstrap> state() override {
    return t::AdministratorBootstrap{t::BootstrapState::Consumed, {1}, kActor, at(0h)};
  }
  t::Status consume(const t::AdministratorBootstrap&, t::ResourceVersion) override {
    return std::monostate{};
  }
};
// Scripted single-transaction UoW: all repositories write a private snapshot; only an explicit
// Committed result publishes it to the durable fixture store. Rollback, unknown commit and any
// dependency failure therefore leave the durable fixture unchanged.
class Work final : public t::IdentitySecurityUnitOfWork {
public:
  Work(Store& store)
      : store_(store), local_(store), principals_(local_), credentials_(local_), base_sessions_(),
        grants_(), authorizations_(local_), assignments_(local_), receipts_(local_),
        secret_receipts_(local_), journal_(local_), operations_(), bootstrap_() {}
  t::Outcome<t::TimePoint> current_time() override {
    if (!store_.now_script.empty() && store_.now_index < store_.now_script.size()) {
      return store_.now_script[store_.now_index++];
    }
    return store_.now;
  }
  t::ReceiptRepository& receipts() override {
    return receipts_;
  }
  t::JournalRepository& journal() override {
    return journal_;
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    return operations_;
  }
  t::CommitResult commit() override {
    ++store_.commits;
    if (store_.commit_result != t::CommitResult::Committed) {
      return store_.commit_result;
    }
    store_.principals = local_.principals;
    store_.credential_heads = local_.credential_heads;
    store_.receipts = local_.receipts;
    store_.secret_receipts = local_.secret_receipts;
    store_.authorizations = local_.authorizations;
    store_.assignments = local_.assignments;
    store_.audits = local_.audits;
    return t::CommitResult::Committed;
  }
  void rollback() noexcept override {
    ++store_.rollbacks;
  }
  t::Repository<t::SubjectId, t::Principal>& principals() override {
    return principals_;
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    return base_sessions_;
  }
  t::Repository<t::AssignmentId, t::ResourceGrant>& grants() override {
    return grants_;
  }
  t::Repository<t::SubjectId, t::AuthorizationState>& authorizations() override {
    return authorizations_;
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
    return secret_receipts_;
  }
  t::IdentityBootstrapRepository& bootstrap() override {
    return bootstrap_;
  }

private:
  Store& store_;
  Store local_;
  Principals principals_;
  Credentials credentials_;
  BaseSessions base_sessions_;
  Sessions sessions_;
  Grants grants_;
  Authorizations authorizations_;
  Assignments assignments_;
  Receipts receipts_;
  SecretReceipts secret_receipts_;
  Journal journal_;
  Operations operations_;
  Bootstrap bootstrap_;
};
class Factory final : public t::IdentitySecurityUnitOfWorkFactory {
public:
  explicit Factory(Store& store) : store_(store) {}
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    return std::unique_ptr<t::IdentitySecurityUnitOfWork>{std::make_unique<Work>(store_)};
  }

private:
  Store& store_;
};
class Random final : public t::RandomPort {
public:
  t::Outcome<t::OwnedSecret> bytes(std::size_t count) override {
    std::string value(count, 'a');
    for (std::size_t index = 0; index < count; ++index) {
      value[index] = static_cast<char>('a' + ((sequence_ + index) % 26));
    }
    ++sequence_;
    return t::OwnedSecret{std::make_unique<FixtureSecret>(std::move(value))};
  }
  t::Outcome<t::OwnedSecret> opaque_token() override {
    std::string value(64, '0');
    for (std::size_t index = 0; index < value.size(); ++index) {
      value[index] = static_cast<char>('0' + ((sequence_ + index) % 10));
    }
    ++sequence_;
    return t::OwnedSecret{std::make_unique<FixtureSecret>(std::move(value))};
  }

private:
  std::size_t sequence_ = 0;
};
class Tokens final : public t::TokenCryptoPort {
public:
  t::Outcome<t::TokenVerifier> make_verifier(t::SecretView token, t::TokenPurpose purpose,
                                             const t::CryptoKeyRef& key) override {
    t::TokenVerifier verifier{purpose, key, {}};
    verifier.tag.bytes[0] = token.empty() ? std::byte{0} : token[0];
    return verifier;
  }
  t::Outcome<bool> verify(t::SecretView, const t::TokenVerifier&) override {
    return true;
  }
  t::Outcome<t::RequestCommitment> commit_request(const t::SecretReceiptScope& scope,
                                                  const t::SecretRequestParameters& parameters,
                                                  t::SecretView secret,
                                                  const t::CryptoKeyRef& key) override {
    t::RequestCommitment commitment{key, {}};
    std::size_t index = 0;
    for (const auto byte : secret) {
      const auto value = std::to_integer<unsigned>(byte);
      commitment.tag.bytes[index % 32] = static_cast<std::byte>(
          (std::to_integer<unsigned>(commitment.tag.bytes[index % 32]) + value) & 0xffU);
      ++index;
    }
    commitment.tag.bytes[0] = std::byte{static_cast<unsigned char>(
        std::to_integer<unsigned>(commitment.tag.bytes[0]) + scope.index() + 1U)};
    if (const auto* delivery = std::get_if<t::CredentialDeliveryParameters>(&parameters)) {
      commitment.tag.bytes[1] =
          std::byte{static_cast<unsigned char>(delivery->expected_version.value & 0xffU)};
    }
    return commitment;
  }
};
class Passwords final : public t::PasswordCryptoPort {
public:
  t::Outcome<t::PasswordVerifier> derive(t::SecretView password) override {
    return t::PasswordVerifier{"phc$" + text(password), {1}};
  }
  t::Outcome<bool> verify(t::SecretView, const t::PasswordVerifier&) override {
    return true;
  }
};
class ReceiptsCrypto final : public t::SecretReceiptCryptoPort {
public:
  struct Sealed {
    t::CredentialVersionRef ref;
    std::string secret;
    t::TimePoint expires;
  };
  t::Outcome<t::SealedSecret> seal(const t::SecretReplayPayload& plaintext,
                                   const t::SecretReceiptBinding& binding,
                                   const t::CryptoKeyRef& key) override {
    const auto* delivery = std::get_if<t::EphemeralCredentialDelivery>(&plaintext);
    if (!delivery) {
      return t::rejected(t::ErrorCode::Internal);
    }
    (void)binding;
    const auto index = static_cast<std::uint32_t>(sealed_.size());
    sealed_.push_back(
        Sealed{delivery->credential, text(delivery->secret->bytes()), delivery->expires_at});
    return t::SealedSecret{key,
                           1,
                           {std::byte{static_cast<unsigned char>(index)}},
                           std::vector<std::byte>{std::byte{1}}};
  }
  t::Outcome<t::SecretReplayPayload> open(const t::SealedSecret& sealed,
                                          const t::SecretReceiptBinding&) override {
    if (sealed.nonce.empty()) {
      return t::rejected(t::ErrorCode::Internal);
    }
    const auto index = std::to_integer<std::size_t>(sealed.nonce[0]);
    if (index >= sealed_.size()) {
      return t::rejected(t::ErrorCode::Internal);
    }
    const auto& stored = sealed_[index];
    return t::SecretReplayPayload{t::EphemeralCredentialDelivery{
        stored.ref, t::OwnedSecret{std::make_unique<FixtureSecret>(stored.secret)},
        stored.expires}};
  }

private:
  std::vector<Sealed> sealed_;
};
class Fingerprints final : public t::IdentityPrincipalFingerprintPort {
public:
  t::Outcome<t::Sha256Digest> revoke_credential(const t::CredentialId& target,
                                                const t::ResourceVersion& expected) override {
    t::Sha256Digest result{};
    result.bytes[0] = std::byte{0x5a};
    std::size_t index = 1;
    for (const auto character : target.value) {
      if (index >= 24) {
        break;
      }
      result.bytes[index++] = std::byte{static_cast<unsigned char>(character)};
    }
    for (std::size_t shift = 0; shift < 8; ++shift) {
      result.bytes[24 + shift] =
          std::byte{static_cast<unsigned char>((expected.value >> (56 - shift * 8)) & 0xffU)};
    }
    return result;
  }
  t::Outcome<t::Sha256Digest> register_principal(t::PrincipalKind kind,
                                                 const t::ResourceRef& binding,
                                                 const std::vector<t::Service>& services) override {
    t::Sha256Digest result{};
    result.bytes[0] = std::byte{static_cast<unsigned char>(kind)};
    result.bytes[1] = std::byte{static_cast<unsigned char>(binding.kind)};
    std::size_t index = 2;
    for (const auto character : binding.id.value) {
      if (index >= 24) {
        break;
      }
      result.bytes[index] = std::byte{static_cast<unsigned char>(character)};
      ++index;
    }
    auto sorted = services;
    std::sort(sorted.begin(), sorted.end());
    result.bytes[24] = std::byte{static_cast<unsigned char>(sorted.size())};
    for (std::size_t position = 0; position < sorted.size() && position < 4; ++position) {
      result.bytes[25 + position] = std::byte{static_cast<unsigned char>(sorted[position])};
    }
    return result;
  }
};
class Fixture {
public:
  Fixture() : factory_(store_) {
    seed_actor();
    seed_grant(t::Permission::IdentityPrincipalManage, kAdapterResource);
    seed_grant(t::Permission::IdentityPrincipalRead, kAdapterResource);
    seed_grant(t::Permission::IdentityPrincipalManage, kOtherResource);
    seed_grant(t::Permission::IdentityPrincipalRead, kOtherResource);
    auto made = t::make_identity_principal_application(
        {factory_, tokens_, passwords_, receipts_, random_, fingerprints_},
        {.token_key = {"token"},
         .request_commitment_key = {"request"},
         .receipt_encryption_key = {"receipt"},
         .bootstrap_credential_lifetime = std::chrono::hours{24},
         .password_credential_lifetime = std::chrono::hours{365 * 24},
         .secret_receipts = {100},
         .maximum_allowed_services = 16});
    application_ = std::move(std::get<std::unique_ptr<t::IdentityPrincipalApplication>>(made));
    if (!application_) {
      std::abort();
    }
  }
  t::IdentityPrincipalApplication& application() {
    return *application_;
  }
  Store& store() {
    return store_;
  }
  t::VerifiedPeer peer() const {
    return actor_peer(store_);
  }
  t::IdentityRequestContext context(std::string key = "idem-1") const {
    return t::IdentityRequestContext{"request-1", "trace-1", std::move(key), store_.now + 5s};
  }
  t::RegisterPrincipalCommand
  registration(t::ResourceRef binding = kAdapterResource,
               std::vector<t::Service> services = {t::Service::Runtime, t::Service::Scheduler},
               t::PrincipalKind kind = t::PrincipalKind::Adapter) {
    return t::RegisterPrincipalCommand{context(), peer(), kind, binding, std::move(services)};
  }

private:
  void seed_actor() {
    const t::ResourceRef self{t::ResourceKind::Principal, {kActor.value}};
    store_.principals[kActor] = t::Principal{
        kActor, t::PrincipalKind::PlatformService, self, {1}, t::LifecycleState::Active};
    const auto certificate = t::CertificateBinding{kActor,
                                                   fingerprint(std::byte{7}),
                                                   "spiffe://kiarabot/control",
                                                   t::Service::Control,
                                                   store_.now - 1h,
                                                   store_.now + 1h};
    store_.credential_heads[kActorCredential] = t::CredentialVersionRecord{
        {kActorCredential, {1}}, kActor,       certificate, t::CredentialState::Active,
        store_.now - 1h,         std::nullopt, self,        store_.now + 1h};
    store_.authorizations[kActor] = t::AuthorizationState{kActor, {1}, false};
  }
  void seed_grant(t::Permission permission, const t::ResourceRef& resource) {
    store_.assignments.push_back(
        t::ExactAssignment{t::AssignmentId{"assignment-" + std::to_string(grants_++)},
                           kActor,
                           {},
                           {{permission, resource}},
                           {1}});
  }
  Store store_{};
  std::size_t grants_ = 0;
  Factory factory_;
  Random random_;
  Tokens tokens_;
  Passwords passwords_;
  ReceiptsCrypto receipts_;
  Fingerprints fingerprints_;
  std::unique_ptr<t::IdentityPrincipalApplication> application_;
};
int main() {
  {
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    check(!std::holds_alternative<t::Failure>(registered), "registration succeeds");
    const auto& snapshot = std::get<t::RegisteredPrincipalSnapshot>(registered);
    check(snapshot.principal.kind == t::PrincipalKind::Adapter &&
              snapshot.principal.state == t::LifecycleState::Disabled &&
              snapshot.principal.version.value == 1 &&
              snapshot.principal.binding == kAdapterResource,
          "registration stores disabled adapter principal with exact binding");
    check(snapshot.initial_credential.state == t::CredentialStatusState::Pending &&
              !snapshot.initial_credential.expires_at &&
              snapshot.initial_credential.ref.version.value == 1,
          "registration returns pending credential without fabricated expiry");
    check(fixture.store().principals.size() == 2 && fixture.store().credential_heads.size() == 2 &&
              fixture.store().audits.size() == 1 && fixture.store().receipts.size() == 1,
          "registration writes principal/credential/audit/receipt once");
    const auto& stored = fixture.store().credential_heads.at(snapshot.initial_credential.ref.id);
    const auto* material = std::get_if<t::PendingCredentialMaterial>(&stored.material);
    check(material && material->kind == t::CredentialKind::WorkloadBootstrap &&
              material->audiences ==
                  std::vector<t::Service>{t::Service::Runtime, t::Service::Scheduler},
          "pending credential keeps allowed services as audiences");
  }
  {
    Fixture fixture;
    auto first = fixture.application().register_principal(fixture.registration());
    auto replay = fixture.application().register_principal(fixture.registration());
    check(!std::holds_alternative<t::Failure>(replay), "registration replay succeeds");
    const auto& original = std::get<t::RegisteredPrincipalSnapshot>(first);
    const auto& repeated = std::get<t::RegisteredPrincipalSnapshot>(replay);
    check(original.principal.id == repeated.principal.id &&
              original.initial_credential.ref == repeated.initial_credential.ref,
          "registration replay returns the original snapshot");
    check(fixture.store().principals.size() == 2 && fixture.store().receipts.size() == 1 &&
              fixture.store().audits.size() == 1,
          "registration replay adds no principal/receipt/audit");
    auto changed = fixture.registration(kAdapterResource, {t::Service::Runtime});
    check(failure_is(fixture.application().register_principal(changed), t::ErrorCode::Conflict),
          "registration replay with changed content conflicts");
  }
  {
    Fixture fixture;
    auto human =
        fixture.registration(kAdapterResource, {t::Service::Runtime}, t::PrincipalKind::Human);
    check(
        failure_is(fixture.application().register_principal(human), t::ErrorCode::InvalidArgument),
        "human registration is rejected");
    auto no_services = fixture.registration(kAdapterResource, {});
    check(failure_is(fixture.application().register_principal(no_services),
                     t::ErrorCode::InvalidArgument),
          "empty allowed services rejected");
    auto wrong_binding =
        fixture.registration(kOtherResource, {t::Service::Runtime}, t::PrincipalKind::Plugin);
    check(failure_is(fixture.application().register_principal(wrong_binding),
                     t::ErrorCode::InvalidArgument),
          "plugin binding must be an instance resource");
    auto platform = fixture.registration(kAdapterResource, {t::Service::Runtime},
                                         t::PrincipalKind::PlatformService);
    check(failure_is(fixture.application().register_principal(platform),
                     t::ErrorCode::InvalidArgument),
          "platform service principal requires a principal self binding");
  }
  {
    Fixture fixture;
    fixture.store().assignments.clear();
    check(failure_is(fixture.application().register_principal(fixture.registration()),
                     t::ErrorCode::PermissionDenied),
          "registration requires an exact management grant");
  }
  {
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto& snapshot = std::get<t::RegisteredPrincipalSnapshot>(registered);
    const auto id = snapshot.initial_credential.ref.id;
    auto delivery = fixture.application().rotate_credential(
        {fixture.context("rotate-1"), fixture.peer(), id, {1}});
    check(!std::holds_alternative<t::Failure>(delivery), "credential rotation succeeds");
    const auto& issued = std::get<t::CredentialDeliveryReply>(delivery);
    check(issued.credential.version.value == 2 && issued.secret && !issued.secret->bytes().empty(),
          "rotation issues version two one-time secret");
    check(issued.delivery_receipt_id.rfind("delivery-", 0) == 0 &&
              issued.expires_at > fixture.store().now,
          "rotation returns correlation receipt id and finite expiry");
    const auto& record = fixture.store().credential_heads.at(id);
    const auto* bootstrap = std::get_if<t::WorkloadBootstrapVerifier>(&record.material);
    check(record.state == t::CredentialState::Active && bootstrap &&
              bootstrap->audiences ==
                  std::vector<t::Service>{t::Service::Runtime, t::Service::Scheduler},
          "rotation stores active workload bootstrap verifier");
    const auto& activated = fixture.store().principals.at(snapshot.principal.id);
    check(activated.state == t::LifecycleState::Active && activated.version.value == 2 &&
              fixture.store().secret_receipts.size() == 1 && fixture.store().audits.size() == 2,
          "first rotation activates the disabled principal and stores receipt/audit");
    auto replay = fixture.application().rotate_credential(
        {fixture.context("rotate-1"), fixture.peer(), id, {1}});
    check(!std::holds_alternative<t::Failure>(replay), "credential delivery replay succeeds");
    check(text(std::get<t::CredentialDeliveryReply>(replay).secret->bytes()) ==
              text(issued.secret->bytes()),
          "delivery replay returns original secret");
    check(fixture.store().secret_receipts.size() == 1,
          "delivery replay does not store a second receipt");
    check(failure_is(fixture.application().rotate_credential(
                         {fixture.context("rotate-2"), fixture.peer(), id, {1}}),
                     t::ErrorCode::Conflict),
          "rotation with stale expected version conflicts");
  }
  {
    Fixture fixture;
    const t::SubjectId human{"ffffffff-ffff-4fff-8fff-ffffffffffff"};
    const t::ResourceRef human_resource{t::ResourceKind::Principal, {human.value}};
    fixture.store().principals[human] = t::Principal{
        human, t::PrincipalKind::Human, human_resource, {1}, t::LifecycleState::Active};
    fixture.store().authorizations[human] = t::AuthorizationState{human, {1}, false};
    fixture.store().assignments.push_back(
        t::ExactAssignment{t::AssignmentId{"assignment-human"},
                           kActor,
                           {},
                           {{t::Permission::IdentityPrincipalManage, human_resource}},
                           {1}});
    const t::CredentialId password_id{"cccccccc-cccc-4ccc-8ccc-cccccccccccc"};
    fixture.store().credential_heads[password_id] =
        t::CredentialVersionRecord{{password_id, {1}},
                                   human,
                                   t::PasswordVerifier{"bootstrap-phc", {1}},
                                   t::CredentialState::Active,
                                   fixture.store().now - 1h,
                                   std::nullopt,
                                   human_resource,
                                   fixture.store().now + 24h};
    auto delivery = fixture.application().rotate_credential(
        {fixture.context("rotate-human-password"), fixture.peer(), password_id, {1}});
    check(!std::holds_alternative<t::Failure>(delivery),
          "human password credential rotation succeeds");
    const auto& issued = std::get<t::CredentialDeliveryReply>(delivery);
    const auto& rotated = fixture.store().credential_heads.at(password_id);
    const auto* verifier = std::get_if<t::PasswordVerifier>(&rotated.material);
    check(rotated.state == t::CredentialState::Active && rotated.ref.version.value == 2 &&
              verifier && verifier->encoded.rfind("phc$", 0) == 0 && issued.secret &&
              !issued.secret->bytes().empty() && issued.expires_at > fixture.store().now,
          "password rotation re-derives an active verifier and returns one-time password");
    auto replay = fixture.application().rotate_credential(
        {fixture.context("rotate-human-password"), fixture.peer(), password_id, {1}});
    check(!std::holds_alternative<t::Failure>(replay) &&
              text(std::get<t::CredentialDeliveryReply>(replay).secret->bytes()) ==
                  text(issued.secret->bytes()),
          "human password delivery replay returns the original password");
    auto revoked = fixture.application().revoke_credential(
        {fixture.context("revoke-human-password"), fixture.peer(), password_id, {2}});
    check(!std::holds_alternative<t::Failure>(revoked),
          "human password credential revoke succeeds");
    const auto& revoked_row = fixture.store().credential_heads.at(password_id);
    const auto gate = t::require_current_credential(
        fixture.store().principals.at(human), revoked_row, revoked_row.ref, fixture.store().now);
    check(std::holds_alternative<t::Failure>(gate) &&
              std::get<t::Failure>(gate).code == t::ErrorCode::Unauthenticated,
          "revoked human credential fails Login/Validate current-credential gate");
  }
  {
    Fixture fixture;
    const t::CredentialId certificate_id{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"};
    const t::SubjectId owner{"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"};
    fixture.store().principals[owner] = t::Principal{
        owner, t::PrincipalKind::Adapter, kAdapterResource, {1}, t::LifecycleState::Active};
    fixture.store().authorizations[owner] = t::AuthorizationState{owner, {1}, false};
    fixture.store().credential_heads[certificate_id] = t::CredentialVersionRecord{
        {certificate_id, {1}},
        owner,
        t::CertificateBinding{owner, fingerprint(std::byte{9}), "san", std::nullopt,
                              fixture.store().now - 1h, fixture.store().now + 1h},
        t::CredentialState::Active,
        fixture.store().now - 1h,
        std::nullopt,
        kAdapterResource,
        fixture.store().now + 1h};
    check(failure_is(
              fixture.application().rotate_credential(
                  {fixture.context("rotate-certificate"), fixture.peer(), certificate_id, {1}}),
              t::ErrorCode::CapabilityNotSupported),
          "certificate binding rotation is a documented kind-level CapabilityNotSupported");
  }
  {
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto id = std::get<t::RegisteredPrincipalSnapshot>(registered).initial_credential.ref.id;
    auto revoked = fixture.application().revoke_credential(
        {fixture.context("revoke-1"), fixture.peer(), id, {1}});
    check(!std::holds_alternative<t::Failure>(revoked), "credential revoke succeeds");
    const auto& reply = std::get<t::RevokeCredentialReply>(revoked);
    check(reply.version.value == 2 &&
              fixture.store().credential_heads.at(id).state == t::CredentialState::Revoked &&
              fixture.store().audits.size() == 2,
          "revoke advances version and records revoked state with audit");
    auto replay = fixture.application().revoke_credential(
        {fixture.context("revoke-1"), fixture.peer(), id, {1}});
    check(!std::holds_alternative<t::Failure>(replay) &&
              std::get<t::RevokeCredentialReply>(replay).version.value == 2 &&
              fixture.store().audits.size() == 2,
          "revoke replay returns original version without a second audit");
    check(failure_is(fixture.application().revoke_credential(
                         {fixture.context("revoke-2"), fixture.peer(), id, {2}}),
                     t::ErrorCode::Conflict),
          "revoking an already revoked credential conflicts");
  }
  {
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto& snapshot = std::get<t::RegisteredPrincipalSnapshot>(registered);
    auto status = fixture.application().get_principal_status(
        {fixture.context(), fixture.peer(), snapshot.principal.id});
    check(!std::holds_alternative<t::Failure>(status), "principal status read succeeds");
    check(std::get<t::PrincipalStatusReply>(status).principal.id == snapshot.principal.id,
          "principal status returns the stored principal");
    auto credential = fixture.application().get_credential_status(
        {fixture.context(), fixture.peer(), snapshot.initial_credential.ref.id});
    check(!std::holds_alternative<t::Failure>(credential), "credential status read succeeds");
    const auto& value = std::get<t::CredentialStatusSnapshot>(credential);
    check(value.state == t::CredentialStatusState::Pending && !value.expires_at &&
              value.ref == snapshot.initial_credential.ref,
          "pending credential status carries no fabricated expiry");
    check(failure_is(fixture.application().get_principal_status(
                         {fixture.context(), fixture.peer(),
                          t::SubjectId{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"}}),
                     t::ErrorCode::NotFound),
          "unknown principal status is NotFound");
    fixture.store().assignments.clear();
    check(failure_is(fixture.application().get_credential_status(
                         {fixture.context(), fixture.peer(), snapshot.initial_credential.ref.id}),
                     t::ErrorCode::NotFound),
          "credential status without an exact read grant hides existence as NotFound");
  }
  {
    // Scoped authorization hides existence: denied and missing both return NOT_FOUND.
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto snapshot = std::get<t::RegisteredPrincipalSnapshot>(registered);
    fixture.store().assignments.clear();
    check(failure_is(fixture.application().rotate_credential({fixture.context("hidden-rotate"),
                                                              fixture.peer(),
                                                              snapshot.initial_credential.ref.id,
                                                              {1}}),
                     t::ErrorCode::NotFound),
          "unauthorized rotation hides existence as NotFound");
    check(failure_is(fixture.application().revoke_credential({fixture.context("hidden-revoke"),
                                                              fixture.peer(),
                                                              snapshot.initial_credential.ref.id,
                                                              {1}}),
                     t::ErrorCode::NotFound),
          "unauthorized revoke hides existence as NotFound");
    check(failure_is(fixture.application().get_credential_status(
                         {fixture.context(), fixture.peer(), snapshot.initial_credential.ref.id}),
                     t::ErrorCode::NotFound),
          "unauthorized credential status hides existence as NotFound");
    check(failure_is(fixture.application().get_principal_status(
                         {fixture.context(), fixture.peer(), snapshot.principal.id}),
                     t::ErrorCode::NotFound),
          "unauthorized principal status hides existence as NotFound");
    check(failure_is(fixture.application().rotate_credential(
                         {fixture.context("missing-rotate"),
                          fixture.peer(),
                          t::CredentialId{"dddddddd-dddd-4ddd-8ddd-dddddddddddd"},
                          {1}}),
                     t::ErrorCode::NotFound),
          "missing credential rotation is NotFound");
  }
  {
    // Same receipt key with a different expected version must conflict, never replay.
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto id = std::get<t::RegisteredPrincipalSnapshot>(registered).initial_credential.ref.id;
    check(!std::holds_alternative<t::Failure>(fixture.application().revoke_credential(
              {fixture.context("revoke-keyed"), fixture.peer(), id, {1}})),
          "keyed revoke succeeds");
    check(failure_is(fixture.application().revoke_credential(
                         {fixture.context("revoke-keyed"), fixture.peer(), id, {2}}),
                     t::ErrorCode::Conflict),
          "same idempotency key with a different expected version conflicts");
    check(!std::holds_alternative<t::Failure>(fixture.application().revoke_credential(
              {fixture.context("revoke-keyed"), fixture.peer(), id, {1}})),
          "identical revoke replay still returns the original version");
  }
  {
    // v1 uniqueness: one Principal per exact binding, regardless of the idempotency key.
    Fixture fixture;
    check(!std::holds_alternative<t::Failure>(
              fixture.application().register_principal(fixture.registration())),
          "first binding registration succeeds");
    auto duplicate = fixture.registration();
    duplicate.context.idempotency_key = "idem-2";
    check(failure_is(fixture.application().register_principal(duplicate), t::ErrorCode::Conflict),
          "second registration for the same binding conflicts");
    check(fixture.store().principals.size() == 2, "duplicate binding adds no second principal");
  }
  {
    // requires_online_authorization=true also enforces the one-second checkpoint before commit.
    Fixture fixture;
    fixture.store().now_script = {fixture.store().now, fixture.store().now + 2s};
    check(failure_is(fixture.application().register_principal(fixture.registration()),
                     t::ErrorCode::Timeout),
          "stalled authorization window rejects registration");
    check(fixture.store().principals.size() == 1 && fixture.store().commits == 0,
          "stalled authorization commits no principal");
  }
  {
    // Unknown commit never returns a secret; rolled back and failed dependencies stay failures.
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto snapshot = std::get<t::RegisteredPrincipalSnapshot>(registered);
    fixture.store().commit_result = t::CommitResult::Unknown;
    const auto unknown =
        fixture.application().rotate_credential({fixture.context("commit-unknown"),
                                                 fixture.peer(),
                                                 snapshot.initial_credential.ref.id,
                                                 {1}});
    check(failure_is(unknown, t::ErrorCode::UpstreamUnavailable) &&
              std::get<t::Failure>(unknown).acceptance == t::Acceptance::Unknown,
          "unknown commit returns UpstreamUnavailable/Unknown and no secret");
    fixture.store().commit_result = t::CommitResult::RolledBack;
    const auto rolled = fixture.application().rotate_credential({fixture.context("commit-rolled"),
                                                                 fixture.peer(),
                                                                 snapshot.initial_credential.ref.id,
                                                                 {1}});
    check(failure_is(rolled, t::ErrorCode::UpstreamUnavailable) &&
              std::get<t::Failure>(rolled).acceptance == t::Acceptance::NotAccepted,
          "rolled back commit returns UpstreamUnavailable/NotAccepted");
    fixture.store().commit_result = t::CommitResult::Committed;
    const auto before_commits = fixture.store().commits;
    fixture.store().fail_audit = true;
    const auto audit_failed = fixture.application().rotate_credential(
        {fixture.context("audit-fail"), fixture.peer(), snapshot.initial_credential.ref.id, {1}});
    check(failure_is(audit_failed, t::ErrorCode::UpstreamUnavailable) &&
              fixture.store().commits == before_commits &&
              fixture.store().credential_heads.at(snapshot.initial_credential.ref.id).state ==
                  t::CredentialState::Pending,
          "audit failure keeps the durable snapshot uncommitted");
    fixture.store().fail_audit = false;
    fixture.store().fail_secret_receipt_insert = true;
    const auto receipt_failed = fixture.application().rotate_credential(
        {fixture.context("receipt-fail"), fixture.peer(), snapshot.initial_credential.ref.id, {1}});
    check(failure_is(receipt_failed, t::ErrorCode::UpstreamUnavailable) &&
              fixture.store().commits == before_commits &&
              fixture.store().credential_heads.at(snapshot.initial_credential.ref.id).state ==
                  t::CredentialState::Pending,
          "receipt capacity/dependency failure keeps the durable snapshot uncommitted");
    fixture.store().fail_secret_receipt_insert = false;
  }
  {
    // Read methods never commit; they release the read-only UoW through rollback.
    Fixture fixture;
    auto registered = fixture.application().register_principal(fixture.registration());
    const auto snapshot = std::get<t::RegisteredPrincipalSnapshot>(registered);
    const auto commits = fixture.store().commits;
    const auto rollbacks = fixture.store().rollbacks;
    check(!std::holds_alternative<t::Failure>(fixture.application().get_credential_status(
              {fixture.context(), fixture.peer(), snapshot.initial_credential.ref.id})),
          "read-only credential status succeeds");
    check(!std::holds_alternative<t::Failure>(fixture.application().get_principal_status(
              {fixture.context(), fixture.peer(), snapshot.principal.id})),
          "read-only principal status succeeds");
    check(fixture.store().commits == commits && fixture.store().rollbacks == rollbacks + 2,
          "read methods never commit and release both read-only UoWs");
  }
  return 0;
}
