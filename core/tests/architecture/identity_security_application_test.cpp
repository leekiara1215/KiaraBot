// clang-format off
#include "kiarabot/core/application/target/identity_security_application.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <string_view>
#include <utility>

#include "kiarabot/core/domain/target/identity_credential_rules.hpp"
// clang-format on

namespace {
namespace t = kiarabot::core::target;
constexpr std::string_view kUser = "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kControl = "22222222-2222-4222-8222-222222222222";
constexpr std::string_view kPasswordCredential = "33333333-3333-4333-8333-333333333333";
constexpr std::string_view kPeerCredential = "44444444-4444-4444-8444-444444444444";
t::Failure fail(t::ErrorCode code = t::ErrorCode::Internal) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
t::SecretView view(std::string_view value) {
  return std::as_bytes(std::span{value.data(), value.size()});
}
// Test-only secret ownership and exact lookup substitutes exercise orchestration, not crypto.
// Production sodium/JWT/AEAD adapters have separate real-library acceptance tests.
class Secret final : public t::SecretBuffer {
public:
  explicit Secret(t::SecretView input) : value_(input.begin(), input.end()) {}
  std::span<const std::byte> bytes() const noexcept override {
    return value_;
  }

private:
  std::vector<std::byte> value_;
};
t::OwnedSecret copy(t::SecretView input) {
  return std::make_unique<Secret>(input);
}
bool same(const t::TokenVerifier& a, const t::TokenVerifier& b) {
  return a.key == b.key && a.purpose == b.purpose && a.tag == b.tag;
}
struct State {
  std::vector<t::Principal> principals;
  std::vector<t::LoginSession> metadata;
  std::vector<t::AuthorizationState> authorizations;
  std::vector<t::CredentialVersionRecord> credentials;
  std::vector<t::LoginIdentity> logins;
  std::vector<t::SessionSecurity> sessions;
  std::vector<t::ExactAssignment> assignments;
  std::vector<t::EncryptedSecretReceipt> receipts;
  std::vector<t::ConsumedRefreshHandle> consumed;
  std::vector<t::AuditEntry> audits;
  std::vector<t::CommandReceipt> ordinary;
};
struct Store {
  State state;
  t::TimePoint now{std::chrono::seconds{1000}};
  std::chrono::steady_clock::time_point monotonic{};
  int active = 0;
  int commits = 0;
  std::uint64_t revision = 0;
  bool fail_begin = false;
  bool fail_audit = false;
  bool fail_list = false;
  bool fail_count = false;
  t::CommitResult next_commit = t::CommitResult::Committed;
  std::function<void()> before_begin;
  std::function<void()> after_audit;
};
template <class Key, class Value> class Repo final : public t::Repository<Key, Value> {
public:
  explicit Repo(std::vector<Value>& values) : values_(values) {}
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    auto it = std::find_if(values_.begin(), values_.end(), [&](const auto& value) {
      if constexpr (requires { value.id; }) {
        return value.id == key;
      } else {
        return value.subject == key;
      }
    });
    return it == values_.end() ? std::optional<Value>{} : std::optional<Value>{*it};
  }
  t::Status save(const Value& value, std::optional<t::ResourceVersion> expected) override {
    auto key = [&] {
      if constexpr (requires { value.id; }) {
        return value.id;
      } else {
        return value.subject;
      }
    }();
    for (auto& existing : values_) {
      const bool match = [&] {
        if constexpr (requires { value.id; }) {
          return existing.id == key;
        } else {
          return existing.subject == key;
        }
      }();
      if (match) {
        if (!expected || existing.version != *expected ||
            value.version.value != expected->value + 1) {
          return fail(t::ErrorCode::Conflict);
        }
        existing = value;
        return std::monostate{};
      }
    }
    if (expected) {
      return fail(t::ErrorCode::Conflict);
    }
    values_.push_back(value);
    return std::monostate{};
  }

private:
  std::vector<Value>& values_;
};
class Credentials final : public t::IdentityCredentialRepository {
public:
  explicit Credentials(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    std::optional<t::CredentialVersionRecord> result;
    for (const auto& value : state_.credentials) {
      if (value.ref.id == id && (!result || result->ref.version.value < value.ref.version.value)) {
        result = value;
      }
    }
    return result;
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  version(const t::CredentialVersionRef& ref) override {
    for (const auto& value : state_.credentials) {
      if (value.ref == ref) {
        return std::optional{value};
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest& tag) override {
    for (const auto& value : state_.credentials) {
      const auto* cert = std::get_if<t::CertificateBinding>(&value.material);
      if (cert && cert->fingerprint == tag) {
        return current(value.ref.id);
      }
    }
    return std::optional<t::CredentialVersionRecord>{};
  }
  t::Outcome<std::optional<t::LoginIdentity>> login(const std::string& name) override {
    for (const auto& value : state_.logins) {
      if (value.normalized_login == name) {
        return std::optional{value};
      }
    }
    return std::optional<t::LoginIdentity>{};
  }
  t::Status save_login(const t::LoginIdentity&, std::optional<t::ResourceVersion>) override {
    return fail();
  }
  t::Status save(const t::CredentialVersionRecord&, std::optional<t::ResourceVersion>) override {
    return fail();
  }

private:
  State& state_;
};
class Sessions final : public t::IdentitySessionSecurityRepository {
public:
  Sessions(State& state, Store& store) : state_(state), store_(store) {}
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId& id) override {
    for (const auto& value : state_.sessions) {
      if (value.id == id) {
        return std::optional{value};
      }
    }
    return std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::SessionSecurity>>
  by_access(const t::TokenVerifier& verifier) override {
    for (const auto& value : state_.sessions) {
      if (same(value.access, verifier)) {
        return std::optional{value};
      }
    }
    return std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::SessionSecurity>>
  by_refresh(const t::TokenVerifier& verifier) override {
    for (const auto& value : state_.sessions) {
      if (same(value.refresh, verifier)) {
        return std::optional{value};
      }
    }
    return std::optional<t::SessionSecurity>{};
  }
  t::Outcome<std::optional<t::RefreshHandle>>
  resolve_refresh(const t::TokenVerifier& input, const t::RefreshRecoveryLookup& lookup,
                  t::TimePoint now) override {
    for (const auto& value : state_.sessions) {
      if (same(value.refresh, input)) {
        return std::optional<t::RefreshHandle>{
            t::CurrentRefreshHandle{value.id, value.subject, value.credential, value.refresh,
                                    value.rotation, value.refresh_expires_at}};
      }
    }
    for (const auto& value : state_.consumed) {
      if (same(value.input.input_verifier, input) &&
          value.receipt_scope.control_peer == lookup.control_peer &&
          value.receipt_scope.idempotency_key == lookup.idempotency_key &&
          now < value.recovery_until && now < value.input.input_expires_at) {
        return std::optional<t::RefreshHandle>{value};
      }
    }
    return std::optional<t::RefreshHandle>{};
  }
  t::Status record_refresh_consumption(const t::ConsumedRefreshHandle& value) override {
    for (const auto& old : state_.consumed) {
      if (same(old.input.input_verifier, value.input.input_verifier)) {
        return fail(t::ErrorCode::Conflict);
      }
    }
    state_.consumed.push_back(value);
    return std::monostate{};
  }
  t::Outcome<std::size_t> count_unexpired() override {
    if (store_.fail_count) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    return static_cast<std::size_t>(
        std::count_if(state_.sessions.begin(), state_.sessions.end(), [&](const auto& session) {
          return !session.revoked && session.refresh_expires_at > store_.now;
        }));
  }
  t::Outcome<std::vector<t::SessionMetadata>> list_metadata(const t::SubjectId& subject,
                                                            std::size_t maximum) override {
    if (store_.fail_list) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::SessionMetadata> result;
    for (const auto& value : state_.sessions) {
      if (value.subject == subject) {
        result.push_back(t::session_metadata(value));
      }
    }
    if (result.size() > maximum) {
      return fail(t::ErrorCode::RateLimited);
    }
    return result;
  }
  t::Status save(const t::SessionSecurity& value, std::optional<t::ResourceVersion> expected,
                 std::optional<std::uint64_t> rotation) override {
    for (auto& existing : state_.sessions) {
      if (existing.id != value.id) {
        continue;
      }
      if (!expected || !rotation || existing.version != *expected ||
          existing.rotation != *rotation || value.created_at != existing.created_at ||
          value.version.value != expected->value + 1) {
        return fail(t::ErrorCode::Conflict);
      }
      existing = value;
      return std::monostate{};
    }
    if (expected || rotation) {
      return fail(t::ErrorCode::Conflict);
    }
    state_.sessions.push_back(value);
    return std::monostate{};
  }

private:
  State& state_;
  Store& store_;
};
class Assignments final : public t::IdentityExactAssignmentRepository {
public:
  explicit Assignments(State& state) : state_(state) {}
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& subject) override {
    std::vector<t::ExactAssignment> result;
    for (const auto& value : state_.assignments) {
      if (value.subject == subject) {
        result.push_back(value);
      }
    }
    return result;
  }
  t::Status save(const t::ExactAssignment&, std::optional<t::ResourceVersion>) override {
    return fail();
  }

private:
  State& state_;
};
class SecretReceipts final : public t::IdentitySecretReceiptRepository {
public:
  explicit SecretReceipts(State& state) : state_(state) {}
  t::Outcome<std::optional<t::EncryptedSecretReceipt>>
  find(const t::SecretReceiptScope& scope) override {
    for (const auto& value : state_.receipts) {
      if (value.binding.scope == scope) {
        return std::optional{value};
      }
    }
    return std::optional<t::EncryptedSecretReceipt>{};
  }
  t::Outcome<t::SecretReceiptWrite> insert(const t::EncryptedSecretReceipt& receipt,
                                           t::SecretReceiptCapacity capacity) override {
    for (const auto& value : state_.receipts) {
      if (value.binding.scope == receipt.binding.scope) {
        if (value.binding.request != receipt.binding.request) {
          return fail(t::ErrorCode::Conflict);
        }
        return t::SecretReceiptWrite{value, true};
      }
    }
    if (state_.receipts.size() >= capacity.max_records) {
      return fail(t::ErrorCode::RateLimited);
    }
    state_.receipts.push_back(receipt);
    return t::SecretReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  State& state_;
};
class Bootstrap final : public t::IdentityBootstrapRepository {
public:
  t::Outcome<t::AdministratorBootstrap> state() override {
    return fail();
  }
  t::Status consume(const t::AdministratorBootstrap&, t::ResourceVersion) override {
    return fail();
  }
};
class Receipts final : public t::ReceiptRepository {
public:
  explicit Receipts(State& state) : state_(state) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey& key) override {
    for (const auto& value : state_.ordinary) {
      if (value.key == key) {
        return std::optional{value};
      }
    }
    return std::optional<t::CommandReceipt>{};
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt& receipt,
                                     t::ReceiptCapacity) override {
    for (const auto& existing : state_.ordinary) {
      if (existing.key == receipt.key) {
        if (existing.fingerprint != receipt.fingerprint) {
          return fail(t::ErrorCode::Conflict);
        }
        return t::ReceiptWrite{existing, true};
      }
    }
    state_.ordinary.push_back(receipt);
    return t::ReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  State& state_;
};
class Journal final : public t::JournalRepository {
public:
  Journal(State& state, Store& store) : state_(state), store_(store) {}
  t::Status append_audit(const t::AuditEntry& value) override {
    if (store_.fail_audit) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    state_.audits.push_back(value);
    if (store_.after_audit) {
      auto hook = std::move(store_.after_audit);
      store_.after_audit = {};
      hook();
    }
    return std::monostate{};
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    return fail();
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    return fail();
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    return fail();
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    return fail();
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    return fail();
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  State& state_;
  Store& store_;
};
class Operations final : public t::Repository<t::OperationId, t::Operation> {
public:
  t::Outcome<std::optional<t::Operation>> find(const t::OperationId&) override {
    return std::optional<t::Operation>{};
  }
  t::Status save(const t::Operation&, std::optional<t::ResourceVersion>) override {
    return fail();
  }
};
class Work final : public t::IdentitySecurityUnitOfWork {
public:
  explicit Work(Store& store)
      : store_(store), state_(store.state), revision_(store.revision),
        principals_(state_.principals), metadata_(state_.metadata),
        authorizations_(state_.authorizations), credentials_(state_), sessions_(state_, store),
        assignments_(state_), secrets_(state_), receipts_(state_), journal_(state_, store) {
    ++store_.active;
  }
  ~Work() override {
    rollback();
  }
  t::Outcome<t::TimePoint> current_time() override {
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
  t::Repository<t::SubjectId, t::Principal>& principals() override {
    return principals_;
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    return metadata_;
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
    return secrets_;
  }
  t::IdentityBootstrapRepository& bootstrap() override {
    return bootstrap_;
  }
  t::CommitResult commit() override {
    assert(active_);
    ++store_.commits;
    auto result = store_.next_commit;
    store_.next_commit = t::CommitResult::Committed;
    if (store_.revision != revision_) {
      result = t::CommitResult::RolledBack;
    }
    if (result != t::CommitResult::RolledBack) {
      store_.state = state_;
      ++store_.revision;
    }
    rollback();
    return result;
  }
  void rollback() noexcept override {
    if (active_) {
      --store_.active;
      active_ = false;
    }
  }

private:
  Store& store_;
  State state_;
  std::uint64_t revision_;
  bool active_ = true;
  Repo<t::SubjectId, t::Principal> principals_;
  Repo<t::SessionId, t::LoginSession> metadata_;
  std::vector<t::ResourceGrant> grant_values_;
  Repo<t::AssignmentId, t::ResourceGrant> grants_{grant_values_};
  Repo<t::SubjectId, t::AuthorizationState> authorizations_;
  Credentials credentials_;
  Sessions sessions_;
  Assignments assignments_;
  SecretReceipts secrets_;
  Bootstrap bootstrap_;
  Receipts receipts_;
  Journal journal_;
  Operations operations_;
};
class Transactions final : public t::IdentitySecurityUnitOfWorkFactory {
public:
  explicit Transactions(Store& store) : store_(store) {}
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    if (store_.before_begin) {
      auto hook = std::move(store_.before_begin);
      store_.before_begin = {};
      hook();
    }
    if (store_.fail_begin) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    return std::unique_ptr<t::IdentitySecurityUnitOfWork>{std::make_unique<Work>(store_)};
  }

private:
  Store& store_;
};
struct Passwords final : t::PasswordCryptoPort {
  explicit Passwords(Store& s) : store(s) {}
  t::Outcome<t::PasswordVerifier> derive(t::SecretView) override {
    assert(store.active == 0);
    return t::PasswordVerifier{"password", {1}};
  }
  t::Outcome<bool> verify(t::SecretView password, const t::PasswordVerifier& verifier) override {
    assert(store.active == 0);
    ++calls;
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    if (verifier.encoded == "dummy") {
      ++dummy_calls;
    }
    const bool accepted =
        std::ranges::equal(password, view("password")) && verifier.encoded == "password";
    if (after_verify) {
      auto hook = std::move(after_verify);
      after_verify = {};
      hook();
    }
    return accepted;
  }
  Store& store;
  int calls = 0;
  int dummy_calls = 0;
  bool unavailable = false;
  std::function<void()> after_verify;
};
struct Tokens final : t::TokenCryptoPort {
  explicit Tokens(Store& s) : store(s) {}
  t::Outcome<t::TokenVerifier> make_verifier(t::SecretView token, t::TokenPurpose purpose,
                                             const t::CryptoKeyRef& key) override {
    assert(store.active == 0);
    if (token.empty()) {
      return fail(t::ErrorCode::InvalidArgument);
    }
    const auto identity =
        std::tuple{std::string(reinterpret_cast<const char*>(token.data()), token.size()),
                   static_cast<unsigned>(purpose), key.value};
    auto found = tags.find(identity);
    if (found == tags.end()) {
      t::Sha256Digest tag;
      const auto ordinal = tags.size() + 1;
      for (std::size_t i = 0; i < sizeof(ordinal); ++i) {
        tag.bytes[i] = std::byte{static_cast<unsigned char>((ordinal >> (i * 8)) & 255)};
      }
      found = tags.emplace(identity, tag).first;
    }
    const auto tag = found->second;
    return t::TokenVerifier{purpose, key, tag};
  }
  t::Outcome<bool> verify(t::SecretView token, const t::TokenVerifier& stored) override {
    auto value = make_verifier(token, stored.purpose, stored.key);
    return same(std::get<t::TokenVerifier>(value), stored);
  }
  t::Outcome<t::RequestCommitment> commit_request(const t::SecretReceiptScope& scope,
                                                  const t::SecretRequestParameters& params,
                                                  t::SecretView secret,
                                                  const t::CryptoKeyRef& key) override {
    assert(store.active == 0);
    std::string encoded = std::to_string(scope.index()) + ":" + std::to_string(params.index());
    std::visit(
        [&](const auto& p) {
          if constexpr (requires { p.input_rotation; }) {
            encoded += std::to_string(p.input_rotation);
          }
        },
        params);
    std::visit([&](const auto& p) { encoded += p.idempotency_key; }, scope);
    if (!secret.empty()) {
      encoded.append(reinterpret_cast<const char*>(secret.data()), secret.size());
    }
    auto token = make_verifier(view(encoded), t::TokenPurpose::Access, key);
    const auto digest = std::get<t::TokenVerifier>(token).tag;
    return t::RequestCommitment{key, digest};
  }
  Store& store;
  std::map<std::tuple<std::string, unsigned, std::string>, t::Sha256Digest> tags;
};
struct Random final : t::RandomPort {
  explicit Random(Store& s) : store(s) {}
  t::Outcome<t::OwnedSecret> bytes(std::size_t count) override {
    assert(store.active == 0);
    if (unavailable) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<std::byte> value(count);
    ++sequence;
    for (std::size_t i = 0; i < count; ++i) {
      value[i] = std::byte{static_cast<unsigned char>((sequence + i) % 255)};
    }
    return copy(value);
  }
  t::Outcome<t::OwnedSecret> opaque_token() override {
    assert(store.active == 0);
    constexpr std::string_view hex = "0123456789abcdef";
    auto random = bytes(32);
    if (const auto* error = std::get_if<t::Failure>(&random)) {
      return *error;
    }
    const auto& source = std::get<t::OwnedSecret>(random)->bytes();
    std::array<std::byte, 64> encoded{};
    for (std::size_t index = 0; index < source.size(); ++index) {
      const auto byte = std::to_integer<unsigned char>(source[index]);
      encoded[index * 2] = std::byte{static_cast<unsigned char>(hex[byte >> 4])};
      encoded[index * 2 + 1] = std::byte{static_cast<unsigned char>(hex[byte & 15])};
    }
    return copy(encoded);
  }
  Store& store;
  unsigned sequence = 0;
  bool unavailable = false;
};
t::SecretReplayPayload clone(const t::SecretReplayPayload& payload) {
  if (const auto* pair = std::get_if<t::EphemeralTokenPair>(&payload)) {
    return t::EphemeralTokenPair{
        pair->session,           copy(pair->access->bytes()), copy(pair->refresh->bytes()),
        pair->access_expires_at, pair->refresh_expires_at,    pair->rotation};
  }
  const auto& delivery = std::get<t::EphemeralCredentialDelivery>(payload);
  return t::EphemeralCredentialDelivery{delivery.credential, copy(delivery.secret->bytes()),
                                        delivery.expires_at};
}
struct CryptoReceipts final : t::SecretReceiptCryptoPort {
  explicit CryptoReceipts(Store& s) : store(s) {}
  t::Outcome<t::SealedSecret> seal(const t::SecretReplayPayload& payload,
                                   const t::SecretReceiptBinding& binding,
                                   const t::CryptoKeyRef& key) override {
    assert(store.active == 0);
    if (fail_seal) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    auto index = static_cast<unsigned char>(values.size());
    values.emplace_back(binding, clone(payload));
    return t::SealedSecret{key, 1, std::vector<std::byte>(24), {std::byte{index}}};
  }
  t::Outcome<t::SecretReplayPayload> open(const t::SealedSecret& sealed,
                                          const t::SecretReceiptBinding& binding) override {
    assert(store.active == 0);
    ++opens;
    if (fail_open) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    if (sealed.ciphertext.size() != 1) {
      return fail();
    }
    const auto index = std::to_integer<unsigned char>(sealed.ciphertext[0]);
    if (index >= values.size() || values[index].first != binding) {
      return fail();
    }
    return clone(values[index].second);
  }
  Store& store;
  int opens = 0;
  bool fail_seal = false;
  bool fail_open = false;
  std::vector<std::pair<t::SecretReceiptBinding, t::SecretReplayPayload>> values;
};
struct Clock final : t::SecurityClockPort {
  explicit Clock(Store& s) : store(s) {}
  t::Outcome<t::SecurityClockSample> sample() override {
    assert(store.active == 0);
    return t::SecurityClockSample{store.now, store.monotonic, std::chrono::milliseconds{0}, true};
  }
  Store& store;
};
struct Delegations final : t::DelegationCryptoPort {
  explicit Delegations(Store& s) : store(s) {}
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims&, const t::CryptoKeyRef&) override {
    assert(store.active == 0);
    return copy(view("proof"));
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView, t::Service, const t::VerifiedPeer&,
                                           const t::SecurityClockSample&) override {
    assert(store.active == 0);
    if (!claims) {
      return fail(t::ErrorCode::Unauthenticated);
    }
    return t::VerifiedDelegation{*claims, {"signing"}};
  }
  Store& store;
  std::optional<t::DelegationClaims> claims;
};
struct Fixture {
  Store store;
  Transactions transactions{store};
  Passwords passwords{store};
  Tokens tokens{store};
  CryptoReceipts receipts{store};
  Random random{store};
  Clock clock{store};
  Delegations delegations{store};
  t::VerifiedPeer peer;
  std::unique_ptr<t::IdentitySecurityApplication> app;
  Fixture() {
    const auto user = t::SubjectId{std::string(kUser)},
               control = t::SubjectId{std::string(kControl)};
    const auto user_resource = t::ResourceRef{t::ResourceKind::Principal, {std::string(kUser)}};
    const auto peer_resource = t::ResourceRef{t::ResourceKind::Principal, {std::string(kControl)}};
    store.state.principals = {{user, t::PrincipalKind::Human, {}, {1}},
                              {control, t::PrincipalKind::PlatformService, peer_resource, {1}}};
    const auto created = store.now - std::chrono::hours{1},
               expires = store.now + std::chrono::hours{24};
    t::CertificateBinding cert{control, {},     "spiffe://control", t::Service::Control,
                               created, expires};
    cert.fingerprint.bytes[0] = std::byte{7};
    store.state.credentials = {{{{std::string(kPasswordCredential)}, {1}},
                                user,
                                t::PasswordVerifier{"password", {1}},
                                t::CredentialState::Active,
                                created,
                                {},
                                user_resource,
                                expires},
                               {{{std::string(kPeerCredential)}, {1}},
                                control,
                                cert,
                                t::CredentialState::Active,
                                created,
                                {},
                                peer_resource,
                                expires}};
    store.state.logins = {{"alice", user, {std::string(kPasswordCredential)}, {1}}};
    store.state.authorizations = {{user, {1}, false}, {control, {1}, false}};
    peer = {{t::PlatformPeer{t::Service::Control, control},
             cert,
             {{std::string(kPeerCredential)}, {1}}},
            store.now};
    rebuild();
  }
  t::IdentitySecurityOptions options() const {
    return {{"token"},
            {{"token"}, {"old-token"}},
            {"commit"},
            {"receipt"},
            {"dummy", {1}},
            std::chrono::hours{24},
            {100},
            10,
            10,
            100};
  }
  void rebuild() {
    auto result = t::make_identity_security_application(
        {transactions, passwords, tokens, receipts, random, clock, delegations}, options());
    app = std::move(std::get<std::unique_ptr<t::IdentitySecurityApplication>>(result));
  }
  t::IdentityRequestContext context(std::string key = "login-key") const {
    return {"request", "trace", std::move(key), store.now + std::chrono::seconds{30}};
  }
  t::LoginRequest login_request(std::string key = "login-key") const {
    return {context(std::move(key)), peer, "alice", view("password")};
  }
};
void login_and_validate() {
  Fixture f;
  auto result = f.app->login(f.login_request());
  assert(std::holds_alternative<t::IdentityTokenReply>(result));
  auto& pair = std::get<t::IdentityTokenReply>(result).tokens;
  assert(f.store.state.sessions.size() == 1 && f.store.state.metadata.size() == 1 &&
         f.store.state.receipts.size() == 1);
  assert(f.store.state.metadata[0].expires_at == pair.refresh_expires_at);
  auto validated = f.app->validate_session({f.context(), f.peer, pair.access->bytes()});
  assert(std::holds_alternative<t::ValidatedSession>(validated));
  auto principal = f.app->get_principal({f.context(), f.peer, pair.access->bytes()});
  assert(std::holds_alternative<t::CurrentPrincipal>(principal));
  auto replay = f.app->login(f.login_request());
  assert(std::holds_alternative<t::IdentityTokenReply>(replay));
  assert(f.store.state.sessions.size() == 1);
  assert(std::ranges::equal(pair.access->bytes(),
                            std::get<t::IdentityTokenReply>(replay).tokens.access->bytes()));
  const auto opens = f.receipts.opens;
  f.store.state.credentials[0].state = t::CredentialState::Revoked;
  f.store.state.credentials[0].revoked_at = f.store.now;
  assert(std::holds_alternative<t::Failure>(
      f.app->validate_session({f.context(), f.peer, pair.access->bytes()})));
  assert(std::holds_alternative<t::Failure>(f.app->login(f.login_request())));
  assert(f.receipts.opens == opens);
}
void missing_and_bad_password_have_one_verify() {
  Fixture missing;
  auto request = missing.login_request();
  request.username = "absent";
  auto result = missing.app->login(request);
  assert(std::holds_alternative<t::Failure>(result));
  assert(missing.passwords.calls == 1 && missing.passwords.dummy_calls == 1);
  Fixture bad;
  request = bad.login_request();
  request.password = view("wrong");
  result = bad.app->login(request);
  assert(std::holds_alternative<t::Failure>(result));
  assert(bad.passwords.calls == 1);
  assert(bad.store.state.sessions.empty());
}
void failed_commit_never_returns_secret() {
  Fixture unknown;
  unknown.store.next_commit = t::CommitResult::Unknown;
  auto result = unknown.app->login(unknown.login_request());
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).acceptance == t::Acceptance::Unknown);
  auto recovered = unknown.app->login(unknown.login_request());
  assert(std::holds_alternative<t::IdentityTokenReply>(recovered));
  assert(unknown.store.state.sessions.size() == 1);
  Fixture failed;
  failed.store.fail_audit = true;
  result = failed.app->login(failed.login_request());
  assert(std::holds_alternative<t::Failure>(result));
  assert(failed.store.state.sessions.empty() && failed.store.state.receipts.empty());
  Fixture rolled;
  rolled.store.next_commit = t::CommitResult::RolledBack;
  result = rolled.app->login(rolled.login_request());
  assert(std::holds_alternative<t::Failure>(result));
  assert(rolled.store.state.sessions.empty());
}
void refresh_only_recovers_original_key() {
  Fixture f;
  auto login = f.app->login(f.login_request());
  auto& original = std::get<t::IdentityTokenReply>(login).tokens;
  auto result =
      f.app->refresh_session({f.context("refresh-key"), f.peer, original.refresh->bytes()});
  assert(std::holds_alternative<t::IdentityTokenReply>(result));
  const auto& rotated = std::get<t::IdentityTokenReply>(result).tokens;
  assert(rotated.rotation == original.rotation + 1);
  assert(f.store.state.consumed.size() == 1);
  auto replay =
      f.app->refresh_session({f.context("refresh-key"), f.peer, original.refresh->bytes()});
  assert(std::holds_alternative<t::IdentityTokenReply>(replay));
  assert(std::ranges::equal(rotated.refresh->bytes(),
                            std::get<t::IdentityTokenReply>(replay).tokens.refresh->bytes()));
  assert(std::holds_alternative<t::Failure>(
      f.app->refresh_session({f.context("different-key"), f.peer, original.refresh->bytes()})));
  auto next = f.app->refresh_session({f.context("next-key"), f.peer, rotated.refresh->bytes()});
  assert(std::holds_alternative<t::IdentityTokenReply>(next));
  const auto opens = f.receipts.opens;
  assert(std::holds_alternative<t::Failure>(
      f.app->refresh_session({f.context("refresh-key"), f.peer, original.refresh->bytes()})));
  assert(f.receipts.opens == opens);
}
t::IdentityActorProof actor(Fixture& f, const t::EphemeralTokenPair& origin) {
  const auto now = f.store.now;
  f.delegations.claims = t::DelegationClaims{
      "identity",
      t::Service::Identity,
      {std::string(kUser)},
      {std::string(kControl)},
      {{std::string(kControl)}},
      {{t::Permission::IdentitySessionRead, {t::ResourceKind::Principal, {std::string(kUser)}}}},
      {"55555555-5555-4555-8555-555555555555"},
      f.peer.binding.certificate.fingerprint,
      {1},
      now,
      now,
      now + std::chrono::seconds{56},
      1,
      {{std::string(kPasswordCredential)}, {1}},
      origin.session};
  return {f.peer, view("proof")};
}
void snapshots_are_immutable_and_reauthorize() {
  Fixture f;
  auto first = f.app->login(f.login_request("first"));
  auto second = f.app->login(f.login_request("second"));
  auto proof = actor(f, std::get<t::IdentityTokenReply>(first).tokens);
  const t::SubjectId subject{std::string(kUser)};
  f.store.state.assignments.push_back(
      {{"66666666-6666-4666-8666-666666666666"},
       subject,
       {},
       {{t::Permission::IdentitySessionRead, {t::ResourceKind::Principal, {std::string(kUser)}}}},
       {1}});
  auto page = f.app->list_sessions({f.context(), proof, subject, {1, {}, {}}});
  assert(std::holds_alternative<t::SessionPageReply>(page));
  const auto first_page = std::get<t::SessionPageReply>(page);
  assert(first_page.sessions.size() == 1 && !first_page.next_page_token.empty());
  assert(first_page.expires_at <= f.store.now + std::chrono::minutes{10});
  auto third = f.app->login(f.login_request("third"));
  assert(std::holds_alternative<t::IdentityTokenReply>(third));
  auto continuation = t::SessionPageRequest{1, first_page.next_page_token, first_page.snapshot_id};
  auto next = f.app->list_sessions({f.context(), proof, subject, continuation});
  assert(std::holds_alternative<t::SessionPageReply>(next));
  assert(std::get<t::SessionPageReply>(next).sessions.size() == 1);
  assert(std::get<t::SessionPageReply>(next).next_page_token.empty());
  auto changed_size = continuation;
  changed_size.page_size = 2;
  assert(std::holds_alternative<t::Failure>(
      f.app->list_sessions({f.context(), proof, subject, changed_size})));
  f.store.state.authorizations[0].version.value = 2;
  assert(std::holds_alternative<t::Failure>(
      f.app->list_sessions({f.context(), proof, subject, continuation})));
  f.store.state.authorizations[0].version.value = 1;
  f.store.now += std::chrono::seconds{601};
  f.store.monotonic += std::chrono::seconds{601};
  f.peer.verified_at = f.store.now;
  proof = actor(f, std::get<t::IdentityTokenReply>(first).tokens);
  assert(std::holds_alternative<t::Failure>(
      f.app->list_sessions({f.context(), proof, subject, continuation})));
  auto fresh = f.app->list_sessions({f.context(), proof, subject, {1, {}, {}}});
  assert(std::holds_alternative<t::SessionPageReply>(fresh));
  const auto fresh_page = std::get<t::SessionPageReply>(fresh);
  f.rebuild();
  assert(std::holds_alternative<t::Failure>(f.app->list_sessions(
      {f.context(), proof, subject, {1, fresh_page.next_page_token, fresh_page.snapshot_id}})));
}
void revoke_changes_security_metadata_and_invalidation_atomically() {
  Fixture f;
  auto source = f.app->login(f.login_request("source"));
  auto target = f.app->login(f.login_request("target"));
  const auto target_id = std::get<t::IdentityTokenReply>(target).tokens.session;
  auto proof = actor(f, std::get<t::IdentityTokenReply>(source).tokens);
  assert(f.delegations.claims.has_value());
  f.delegations.claims->permissions = {
      {t::Permission::IdentitySessionRevoke, {t::ResourceKind::Session, {target_id.value}}}};
  auto revoked = f.app->revoke_session({f.context("revoke"), proof, target_id});
  assert(std::holds_alternative<t::RevokeSessionReply>(revoked));
  assert(std::get<t::RevokeSessionReply>(revoked).session.revoked);
  for (const auto& session : f.store.state.sessions) {
    if (session.id == target_id) {
      assert(session.revoked);
    }
  }
  for (const auto& metadata : f.store.state.metadata) {
    if (metadata.id == target_id) {
      assert(metadata.revoked);
    }
  }
  assert(f.store.state.authorizations[0].invalidation_dirty);
  const auto snapshot = std::get<t::RevokeSessionReply>(revoked).session;
  for (auto& session : f.store.state.sessions) {
    if (session.id == target_id) {
      ++session.version.value;
      ++session.rotation;
      session.refresh_expires_at += std::chrono::hours{1};
    }
  }
  for (auto& metadata : f.store.state.metadata) {
    if (metadata.id == target_id) {
      ++metadata.version.value;
      ++metadata.rotation;
      metadata.expires_at += std::chrono::hours{1};
    }
  }
  assert(f.delegations.claims.has_value());
  f.delegations.claims->authorization_version = f.store.state.authorizations[0].version;
  const auto replayed = f.app->revoke_session({f.context("revoke"), proof, target_id});
  assert(std::holds_alternative<t::RevokeSessionReply>(replayed));
  const auto& replay_snapshot = std::get<t::RevokeSessionReply>(replayed).session;
  assert(replay_snapshot.created_at == snapshot.created_at &&
         replay_snapshot.expires_at == snapshot.expires_at &&
         replay_snapshot.rotation == snapshot.rotation &&
         replay_snapshot.version == snapshot.version);
  assert(std::holds_alternative<t::Failure>(f.app->validate_session(
      {f.context(), f.peer, std::get<t::IdentityTokenReply>(target).tokens.access->bytes()})));
}
void dependency_deadline_and_capacities_never_fake_success() {
  Fixture unavailable;
  unavailable.store.fail_begin = true;
  auto result = unavailable.app->login(unavailable.login_request());
  assert(std::get<t::Failure>(result).code == t::ErrorCode::UpstreamUnavailable);
  Fixture expired;
  auto request = expired.login_request();
  request.context.deadline = expired.store.now;
  assert(std::holds_alternative<t::Failure>(expired.app->login(request)));
  assert(expired.store.state.sessions.empty());
  Fixture full;
  auto options = full.options();
  options.maximum_sessions_per_subject = 1;
  auto rebuilt = t::make_identity_security_application({full.transactions, full.passwords,
                                                        full.tokens, full.receipts, full.random,
                                                        full.clock, full.delegations},
                                                       options);
  full.app = std::move(std::get<std::unique_ptr<t::IdentitySecurityApplication>>(rebuilt));
  assert(std::holds_alternative<t::IdentityTokenReply>(full.app->login(full.login_request("one"))));
  assert(std::holds_alternative<t::Failure>(full.app->login(full.login_request("two"))));
  assert(full.store.state.sessions.size() == 1);
}

void final_transaction_rechecks_mutations_during_password_work() {
  for (int mutation = 0; mutation < 5; ++mutation) {
    Fixture f;
    f.passwords.after_verify = [&] {
      if (mutation == 0) {
        f.store.state.credentials[0].state = t::CredentialState::Revoked;
        f.store.state.credentials[0].revoked_at = f.store.now;
      } else if (mutation == 1) {
        f.store.state.principals[0].state = t::LifecycleState::Disabled;
      } else if (mutation == 2) {
        f.store.state.credentials[1].state = t::CredentialState::Revoked;
        f.store.state.credentials[1].revoked_at = f.store.now;
      } else if (mutation == 3) {
        f.store.state.credentials[0].material = t::PasswordVerifier{"changed", {1}};
      } else {
        f.store.now += std::chrono::seconds{31};
      }
      ++f.store.revision;
    };
    assert(std::holds_alternative<t::Failure>(f.app->login(f.login_request())));
    assert(f.store.state.sessions.empty() && f.store.state.metadata.empty() &&
           f.store.state.receipts.empty());
  }
}

void refresh_unknown_is_recovered_without_second_rotation() {
  Fixture f;
  auto logged = f.app->login(f.login_request());
  const auto& original = std::get<t::IdentityTokenReply>(logged).tokens;
  f.store.next_commit = t::CommitResult::Unknown;
  auto result =
      f.app->refresh_session({f.context("refresh-unknown"), f.peer, original.refresh->bytes()});
  assert(std::holds_alternative<t::Failure>(result));
  assert(std::get<t::Failure>(result).acceptance == t::Acceptance::Unknown);
  assert(f.store.state.sessions[0].rotation == original.rotation + 1);
  auto recovered =
      f.app->refresh_session({f.context("refresh-unknown"), f.peer, original.refresh->bytes()});
  assert(std::holds_alternative<t::IdentityTokenReply>(recovered));
  assert(std::get<t::IdentityTokenReply>(recovered).tokens.rotation == original.rotation + 1);
  assert(f.store.state.sessions[0].rotation == original.rotation + 1);
  assert(f.store.state.consumed.size() == 1);
}
void list_dependency_failure_never_becomes_empty_page() {
  Fixture f;
  auto logged = f.app->login(f.login_request());
  auto proof = actor(f, std::get<t::IdentityTokenReply>(logged).tokens);
  f.store.fail_list = true;
  auto page = f.app->list_sessions({f.context(), proof, {std::string(kUser)}, {}});
  assert(std::holds_alternative<t::Failure>(page));
  assert(std::get<t::Failure>(page).code == t::ErrorCode::UpstreamUnavailable);
}

void login_rate_limits_use_monotonic_time_and_reset_on_restart() {
  Fixture f;
  auto request = f.login_request();
  request.password = view("wrong");
  const t::SubjectId other_subject{"77777777-7777-4777-8777-777777777777"};
  const t::CredentialVersionRef other_credential{{"88888888-8888-4888-8888-888888888888"}, {1}};
  const t::ResourceRef other_binding{t::ResourceKind::Principal, {other_subject.value}};
  auto other_certificate = f.peer.binding.certificate;
  other_certificate.subject = other_subject;
  other_certificate.fingerprint.bytes[0] = std::byte{99};
  f.store.state.principals.push_back(
      {other_subject, t::PrincipalKind::PlatformService, other_binding, {1}});
  f.store.state.authorizations.push_back({other_subject, {1}, false});
  f.store.state.credentials.push_back({other_credential,
                                       other_subject,
                                       other_certificate,
                                       t::CredentialState::Active,
                                       f.store.now - std::chrono::hours{1},
                                       {},
                                       other_binding,
                                       f.store.now + std::chrono::hours{24}});
  const t::VerifiedPeer other_peer{
      {t::PlatformPeer{t::Service::Control, other_subject}, other_certificate, other_credential},
      f.store.now};
  for (unsigned attempt = 0; attempt < 30; ++attempt) {
    request.control_peer = attempt % 2 == 0 ? f.peer : other_peer;
    request.context.idempotency_key = "bad-" + std::to_string(attempt);
    const auto result = f.app->login(request);
    assert(std::get<t::Failure>(result).code == t::ErrorCode::Unauthenticated);
  }
  const auto cost = f.passwords.calls;
  auto limited = f.app->login(request);
  assert(std::get<t::Failure>(limited).code == t::ErrorCode::RateLimited);
  assert(f.passwords.calls == cost);
  f.store.monotonic += std::chrono::minutes{1};
  auto recovered = f.app->login(request);
  assert(std::get<t::Failure>(recovered).code == t::ErrorCode::Unauthenticated);
  assert(f.passwords.calls == cost + 1);
  f.rebuild();
  recovered = f.app->login(request);
  assert(std::get<t::Failure>(recovered).code == t::ErrorCode::Unauthenticated);
}
void invalid_control_peer_does_not_consume_login_budget() {
  Fixture f;
  auto request = f.login_request();
  request.password = view("wrong");
  request.control_peer.binding.credential.version.value = 99;
  for (unsigned attempt = 0; attempt < 10; ++attempt) {
    assert(std::holds_alternative<t::Failure>(f.app->login(request)));
  }
  assert(f.passwords.calls == 0);
  request.control_peer = f.peer;
  const auto result = f.app->login(request);
  assert(std::get<t::Failure>(result).code == t::ErrorCode::Unauthenticated);
  assert(f.passwords.calls == 1);
}
void peer_login_budget_applies_across_unknown_usernames() {
  Fixture f;
  auto request = f.login_request();
  request.password = view("wrong");
  for (unsigned attempt = 0; attempt < 60; ++attempt) {
    request.username = "missing" + std::to_string(attempt);
    const auto result = f.app->login(request);
    assert(std::get<t::Failure>(result).code == t::ErrorCode::Unauthenticated);
  }
  const auto cost = f.passwords.calls;
  request.username = "one-more";
  const auto result = f.app->login(request);
  assert(std::get<t::Failure>(result).code == t::ErrorCode::RateLimited);
  assert(f.passwords.calls == cost);
}

void login_key_capacity_never_evicts_active_budgets() {
  Fixture f;
  std::vector<t::VerifiedPeer> peers;
  const auto created = f.store.now - std::chrono::hours{1};
  const auto expires = f.store.now + std::chrono::hours{24};
  for (unsigned index = 0; index < 90; ++index) {
    auto suffix = std::to_string(index + 1);
    suffix.insert(0, 12 - suffix.size(), '0');
    const t::SubjectId subject{"22222222-2222-4222-8222-" + suffix};
    const t::CredentialVersionRef credential{{"44444444-4444-4444-8444-" + suffix}, {1}};
    const t::ResourceRef binding{t::ResourceKind::Principal, {subject.value}};
    auto certificate = f.peer.binding.certificate;
    certificate.subject = subject;
    certificate.fingerprint.bytes[0] = std::byte{100};
    certificate.fingerprint.bytes[1] = std::byte{static_cast<unsigned char>(index)};
    f.store.state.principals.push_back({subject, t::PrincipalKind::PlatformService, binding, {1}});
    f.store.state.authorizations.push_back({subject, {1}, false});
    f.store.state.credentials.push_back({credential,
                                         subject,
                                         certificate,
                                         t::CredentialState::Active,
                                         created,
                                         {},
                                         binding,
                                         expires});
    peers.push_back(
        {{t::PlatformPeer{t::Service::Control, subject}, certificate, credential}, f.store.now});
  }
  unsigned denied = 0;
  for (std::size_t index = 0; index < peers.size(); ++index) {
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
      auto request = f.login_request();
      request.control_peer = peers[index];
      request.username = "missing-" + std::to_string(index) + "-" + std::to_string(attempt);
      const auto result = f.app->login(request);
      const auto code = std::get<t::Failure>(result).code;
      assert(code == t::ErrorCode::Unauthenticated || code == t::ErrorCode::RateLimited);
      if (code == t::ErrorCode::RateLimited) {
        ++denied;
      }
    }
  }
  assert(denied > 0);
  assert(f.passwords.calls <= 4096);
  auto existing = f.login_request();
  existing.control_peer = peers[0];
  existing.username = "missing-0-0";
  const auto reused = f.app->login(existing);
  assert(std::get<t::Failure>(reused).code == t::ErrorCode::Unauthenticated);
  f.store.monotonic += std::chrono::minutes{1};
  existing.username = "new-after-expiry";
  const auto recovered = f.app->login(existing);
  assert(std::get<t::Failure>(recovered).code == t::ErrorCode::Unauthenticated);
}

void crypto_dependency_failure_preserves_atomic_state() {
  for (unsigned dependency = 0; dependency < 3; ++dependency) {
    Fixture f;
    f.passwords.unavailable = dependency == 0;
    f.random.unavailable = dependency == 1;
    f.receipts.fail_seal = dependency == 2;
    const auto result = f.app->login(f.login_request());
    assert(std::get<t::Failure>(result).code == t::ErrorCode::UpstreamUnavailable);
    assert(f.store.state.sessions.empty() && f.store.state.metadata.empty() &&
           f.store.state.receipts.empty());
  }
  Fixture f;
  auto first = f.app->login(f.login_request());
  assert(std::holds_alternative<t::IdentityTokenReply>(first));
  f.receipts.fail_open = true;
  const auto replayed = f.app->login(f.login_request());
  assert(std::get<t::Failure>(replayed).code == t::ErrorCode::UpstreamUnavailable);
  assert(f.store.state.sessions.size() == 1);
}

void final_database_deadline_is_checked_after_all_writes() {
  Fixture login;
  login.store.after_audit = [&] { login.store.now += std::chrono::seconds{31}; };
  const auto failed = login.app->login(login.login_request());
  assert(std::get<t::Failure>(failed).code == t::ErrorCode::Timeout);
  assert(login.store.state.sessions.empty() && login.store.state.receipts.empty());
  Fixture refresh;
  auto first = refresh.app->login(refresh.login_request());
  const auto& original = std::get<t::IdentityTokenReply>(first).tokens;
  refresh.store.after_audit = [&] { refresh.store.now += std::chrono::seconds{31}; };
  const auto rotated = refresh.app->refresh_session(
      {refresh.context("refresh"), refresh.peer, original.refresh->bytes()});
  assert(std::get<t::Failure>(rotated).code == t::ErrorCode::Timeout);
  assert(refresh.store.state.sessions[0].rotation == original.rotation);
  assert(refresh.store.state.consumed.empty());
}
void revoke_authorization_budget_is_rechecked_before_commit() {
  Fixture f;
  auto first = f.app->login(f.login_request("source"));
  auto second = f.app->login(f.login_request("target"));
  const auto target = std::get<t::IdentityTokenReply>(second).tokens.session;
  auto proof = actor(f, std::get<t::IdentityTokenReply>(first).tokens);
  assert(f.delegations.claims.has_value());
  f.delegations.claims->permissions = {
      {t::Permission::IdentitySessionRevoke, {t::ResourceKind::Session, {target.value}}}};
  f.store.after_audit = [&] { f.store.now += std::chrono::seconds{2}; };
  const auto revoked = f.app->revoke_session({f.context("revoke"), proof, target});
  assert(std::get<t::Failure>(revoked).code == t::ErrorCode::Timeout);
  for (const auto& session : f.store.state.sessions) {
    assert(!session.revoked);
  }
  assert(!f.store.state.authorizations[0].invalidation_dirty);
}

void backwards_monotonic_sample_fails_without_password_work() {
  Fixture f;
  f.store.monotonic += std::chrono::seconds{10};
  auto request = f.login_request();
  request.password = view("wrong");
  const auto first = f.app->login(request);
  assert(std::get<t::Failure>(first).code == t::ErrorCode::Unauthenticated);
  const auto cost = f.passwords.calls;
  f.store.monotonic -= std::chrono::seconds{1};
  const auto rejected = f.app->login(request);
  assert(std::get<t::Failure>(rejected).code == t::ErrorCode::UpstreamUnavailable);
  assert(f.passwords.calls == cost);
}

void authoritative_global_session_capacity_preserves_replay() {
  Fixture f;
  auto logged = f.app->login(f.login_request());
  const auto& original = std::get<t::IdentityTokenReply>(logged).tokens;
  const auto prototype = f.store.state.sessions[0];
  for (std::size_t index = 1; index < t::kMaximumUnexpiredSessions; ++index) {
    auto session = prototype;
    auto suffix = std::to_string(index);
    suffix.insert(0, 12 - suffix.size(), '0');
    session.id = {"99999999-9999-4999-8999-" + suffix};
    session.subject = {"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
    f.store.state.sessions.push_back(session);
  }
  assert(f.store.state.sessions.size() == t::kMaximumUnexpiredSessions);
  const auto refused = f.app->login(f.login_request("over-cap"));
  assert(std::get<t::Failure>(refused).code == t::ErrorCode::RateLimited);
  const auto replayed = f.app->login(f.login_request());
  assert(std::holds_alternative<t::IdentityTokenReply>(replayed));
  assert(std::get<t::IdentityTokenReply>(replayed).tokens.session == original.session);
  assert(f.store.state.sessions.size() == t::kMaximumUnexpiredSessions);
  auto extra = prototype;
  extra.id = {"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"};
  extra.subject = {"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"};
  f.store.state.sessions.push_back(extra);
  const auto refused_again = f.app->login(f.login_request("more-over-cap"));
  assert(std::get<t::Failure>(refused_again).code == t::ErrorCode::RateLimited);
  f.store.state.sessions[1].revoked = true;
  f.store.state.sessions[2].refresh_expires_at = f.store.now;
  const auto accepted = f.app->login(f.login_request("released-capacity"));
  assert(std::holds_alternative<t::IdentityTokenReply>(accepted));
  Fixture failure;
  failure.store.fail_count = true;
  const auto unavailable = failure.app->login(failure.login_request());
  assert(std::get<t::Failure>(unavailable).code == t::ErrorCode::UpstreamUnavailable);
  assert(failure.store.state.sessions.empty() && failure.store.state.receipts.empty());
}

} // namespace
int main(int argc, char** argv) try {
  const std::string_view group = argc == 2 ? argv[1] : "all";
  if (group == "all" || group == "login") {
    login_and_validate();
    missing_and_bad_password_have_one_verify();
    failed_commit_never_returns_secret();
    dependency_deadline_and_capacities_never_fake_success();
    final_transaction_rechecks_mutations_during_password_work();
    crypto_dependency_failure_preserves_atomic_state();
    final_database_deadline_is_checked_after_all_writes();
    login_rate_limits_use_monotonic_time_and_reset_on_restart();
    invalid_control_peer_does_not_consume_login_budget();
    peer_login_budget_applies_across_unknown_usernames();
    backwards_monotonic_sample_fails_without_password_work();
    authoritative_global_session_capacity_preserves_replay();
  }
  if (group == "all" || group == "rate_capacity") {
    login_key_capacity_never_evicts_active_budgets();
  }
  if (group == "all" || group == "refresh") {
    refresh_only_recovers_original_key();
    refresh_unknown_is_recovered_without_second_rotation();
  }
  if (group == "all" || group == "list") {
    snapshots_are_immutable_and_reauthorize();
    list_dependency_failure_never_becomes_empty_page();
  }
  if (group == "all" || group == "revoke") {
    revoke_changes_security_metadata_and_invalidation_atomically();
    revoke_authorization_budget_is_rechecked_before_commit();
  }
  if (group != "all" && group != "login" && group != "refresh" && group != "list" &&
      group != "rate_capacity" && group != "revoke") {
    return 2;
  }
} catch (...) {
  return 1;
}
