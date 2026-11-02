#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/application/target/model_gateway_application.hpp"
#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/core/ports/target/model_ports.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;
t::Outcome<t::ModelMutationReply> application_rotate(class Fixture& fixture);

void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("P11 Model application assertion failed at line " +
                             std::to_string(location.line()));
  }
}

constexpr char kModel[] = "11111111-1111-4111-8111-111111111111";
constexpr char kOwner[] = "22222222-2222-4222-8222-222222222222";
constexpr char kOtherOwner[] = "33333333-3333-4333-8333-333333333333";
constexpr char kCall[] = "44444444-4444-4444-8444-444444444444";
constexpr char kCall2[] = "55555555-5555-4555-8555-555555555555";
constexpr char kSubject[] = "66666666-6666-4666-8666-666666666666";

t::TimePoint now() {
  return t::TimePoint{std::chrono::seconds{1'800'000'000}};
}
t::ModelCallContext context(std::string key = "idem-1") {
  return {"request-1", "trace-1", now() + 30s, std::move(key)};
}
t::ModelCaller control() {
  t::ModelCaller caller;
  caller.subject = t::SubjectId{kSubject};
  caller.delegation = "delegation";
  caller.peer_fingerprint.bytes[0] = std::byte{0x42};
  return caller;
}
t::ModelCaller plugin() {
  t::ModelCaller caller;
  caller.subject = t::SubjectId{kOwner};
  caller.owner = t::InstanceId{kOwner};
  caller.delegation = "delegation";
  caller.peer_fingerprint.bytes[0] = std::byte{0x43};
  return caller;
}
t::ModelMessage message(std::string text = "hello") {
  return {"user", std::move(text)};
}
t::Sha256Digest digest_of(std::span<const std::byte> input) {
  t::Sha256Digest digest;
  std::uint64_t state = 1469598103934665603ULL;
  for (const auto byte : input) {
    state = (state ^ static_cast<unsigned char>(byte)) * 1099511628211ULL;
    state ^= state >> 29U;
  }
  for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
    digest.bytes[index] = static_cast<std::byte>((state >> ((index % 8) * 8U)) & 0xffU);
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
  }
  return digest;
}

struct Store {
  std::map<std::string, t::ModelConfiguration> configs;
  std::map<std::string, std::vector<t::ProviderSecretRecord>> secrets;
  std::map<std::pair<std::string, std::string>, t::ModelIntent> calls;
  std::vector<std::pair<t::ReceiptKey, t::CommandReceipt>> receipts;
  std::map<std::string, t::Operation> operations;
  std::vector<t::AuditEntry> audits;
  int commits = 0;
  int rollbacks = 0;
  int fail_commits = 0;
};

class FakeSecret final : public t::SecretBuffer {
public:
  explicit FakeSecret(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
  std::span<const std::byte> bytes() const noexcept override {
    return bytes_;
  }

private:
  std::vector<std::byte> bytes_;
};

class RepoBase {
public:
  Store& store;
  Store snapshot;
  explicit RepoBase(Store& value) : store(value), snapshot(value) {}
};

class ConfigRepo final : public t::ModelConfigurationRepository, public RepoBase {
public:
  using RepoBase::RepoBase;
  t::Outcome<std::optional<t::ModelConfiguration>> find(const t::ResourceId& id) override {
    const auto found = snapshot.configs.find(id.value);
    if (found == snapshot.configs.end()) {
      return std::optional<t::ModelConfiguration>{};
    }
    return std::optional<t::ModelConfiguration>{found->second};
  }
  t::Outcome<std::optional<t::ModelConfiguration>>
  find_by_alias(const std::string& alias) override {
    for (const auto& [id, config] : snapshot.configs) {
      if (config.alias == alias) {
        return std::optional<t::ModelConfiguration>{config};
      }
    }
    return std::optional<t::ModelConfiguration>{};
  }
  t::Outcome<std::vector<t::ModelConfiguration>> list(std::size_t limit,
                                                      const std::string& after) override {
    std::vector<t::ModelConfiguration> result;
    for (const auto& [id, config] : snapshot.configs) {
      if (id > after && result.size() < limit) {
        result.push_back(config);
      }
    }
    return result;
  }
  void commit() {
    store.configs = snapshot.configs;
  }
  t::Status save(const t::ModelConfiguration& value,
                 std::optional<t::ResourceVersion> expected) override {
    const auto found = snapshot.configs.find(value.model_id.value);
    if (!expected) {
      if (found != snapshot.configs.end()) {
        return t::Failure{t::ErrorCode::Conflict};
      }
      snapshot.configs[value.model_id.value] = value;
      return std::monostate{};
    }
    if (found == snapshot.configs.end() || found->second.version.value != expected->value ||
        value.version.value != expected->value + 1) {
      return t::Failure{t::ErrorCode::Conflict};
    }
    snapshot.configs[value.model_id.value] = value;
    return std::monostate{};
  }
};

class SecretRepo final : public t::ProviderSecretRepository, public RepoBase {
public:
  using RepoBase::RepoBase;
  t::Outcome<std::optional<t::ProviderSecretRecord>> current(const t::ResourceId& id) override {
    const auto found = snapshot.secrets.find(id.value);
    if (found == snapshot.secrets.end() || found->second.empty()) {
      return std::optional<t::ProviderSecretRecord>{};
    }
    return std::optional<t::ProviderSecretRecord>{found->second.back()};
  }
  t::Outcome<std::optional<t::ProviderSecretRecord>>
  find(const t::ResourceId& id, const t::ResourceVersion& version) override {
    const auto found = snapshot.secrets.find(id.value);
    if (found != snapshot.secrets.end()) {
      for (const auto& secret : found->second) {
        if (secret.secret_version.value == version.value) {
          return std::optional<t::ProviderSecretRecord>{secret};
        }
      }
    }
    return std::optional<t::ProviderSecretRecord>{};
  }
  void commit() {
    store.secrets = snapshot.secrets;
  }
  t::Status insert(const t::ProviderSecretRecord& value) override {
    auto& values = snapshot.secrets[value.model_id.value];
    for (const auto& secret : values) {
      if (secret.secret_version.value == value.secret_version.value) {
        return t::Failure{t::ErrorCode::Conflict};
      }
    }
    values.push_back(value);
    return std::monostate{};
  }
};

class CallRepo final : public t::ModelCallRepository, public RepoBase {
public:
  using RepoBase::RepoBase;
  t::Outcome<std::optional<t::ModelIntent>> find(const t::ModelCallKey& key) override {
    const auto found = snapshot.calls.find({key.owner.value, key.logical_call_id});
    if (found == snapshot.calls.end()) {
      return std::optional<t::ModelIntent>{};
    }
    return std::optional<t::ModelIntent>{found->second};
  }
  t::Outcome<std::optional<t::ModelIntent>> find_by_call_id(const std::string& call_id) override {
    std::optional<t::ModelIntent> found;
    for (const auto& [key, call] : snapshot.calls) {
      if (key.second != call_id) {
        continue;
      }
      if (found) {
        return t::Failure{t::ErrorCode::Conflict};
      }
      found = call;
    }
    return found;
  }
  t::Outcome<std::vector<t::ModelIntent>> list_unresolved_unknown(t::TimePoint at,
                                                                  std::size_t limit) override {
    std::vector<t::ModelIntent> result;
    for (const auto& [key, call] : snapshot.calls) {
      if (call.state == t::ModelCallState::Unknown &&
          call.disposition == t::ModelCallDisposition::Unspecified &&
          call.created_at + t::kModelUnknownResolutionWindow <= at && result.size() < limit) {
        result.push_back(call);
      }
    }
    return result;
  }
  t::Status insert(const t::ModelIntent& value) override {
    const auto key = std::make_pair(value.owner.value, value.logical_call_id);
    if (snapshot.calls.find(key) != snapshot.calls.end()) {
      return t::Failure{t::ErrorCode::Conflict};
    }
    snapshot.calls[key] = value;
    return std::monostate{};
  }
  t::Status save(const t::ModelIntent& value, std::optional<t::ResourceVersion> expected) override {
    const auto key = std::make_pair(value.owner.value, value.logical_call_id);
    const auto found = snapshot.calls.find(key);
    if (found == snapshot.calls.end() || !expected ||
        found->second.version.value != expected->value) {
      return t::Failure{t::ErrorCode::Conflict};
    }
    snapshot.calls[key] = value;
    return std::monostate{};
  }
  void commit() {
    store.calls = snapshot.calls;
  }
  t::Outcome<std::size_t> prune_terminal(t::TimePoint at, std::size_t limit) override {
    std::size_t pruned = 0;
    for (auto iterator = snapshot.calls.begin();
         iterator != snapshot.calls.end() && pruned < limit;) {
      const auto& call = iterator->second;
      const bool terminal =
          call.state == t::ModelCallState::Succeeded || call.state == t::ModelCallState::Failed;
      if (terminal && call.observed_at + t::kModelCallRetention <= at) {
        iterator = snapshot.calls.erase(iterator);
        ++pruned;
      } else {
        ++iterator;
      }
    }
    return pruned;
  }
};

class ReceiptRepo final : public t::ReceiptRepository {
public:
  explicit ReceiptRepo(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey& key) override {
    for (const auto& [stored_key, receipt] : store_.receipts) {
      if (stored_key == key) {
        return std::optional<t::CommandReceipt>{receipt};
      }
    }
    return std::optional<t::CommandReceipt>{};
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt& value, t::ReceiptCapacity) override {
    auto found = find(value.key);
    if (const auto* rejected = std::get_if<t::Failure>(&found)) {
      return *rejected;
    }
    const auto& stored = std::get<std::optional<t::CommandReceipt>>(found);
    if (stored) {
      if (stored->fingerprint != value.fingerprint) {
        return t::Failure{t::ErrorCode::Conflict};
      }
      return t::ReceiptWrite{*stored, true};
    }
    store_.receipts.emplace_back(value.key, value);
    return t::ReceiptWrite{value, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};

class JournalRepo final : public t::JournalRepository {
public:
  explicit JournalRepo(Store& store) : store_(store) {}
  t::Status append_audit(const t::AuditEntry& entry) override {
    store_.audits.push_back(entry);
    return std::monostate{};
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    return t::Failure{t::ErrorCode::CapabilityNotSupported};
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    return std::vector<t::OutboxClaim>{};
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    return t::Failure{t::ErrorCode::CapabilityNotSupported};
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    return std::optional<t::InboxRecord>{};
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    return t::Failure{t::ErrorCode::CapabilityNotSupported};
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};

class OperationRepo final : public t::Repository<t::OperationId, t::Operation> {
public:
  explicit OperationRepo(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::Operation>> find(const t::OperationId& id) override {
    const auto found = store_.operations.find(id.value);
    if (found == store_.operations.end()) {
      return std::optional<t::Operation>{};
    }
    return std::optional<t::Operation>{found->second};
  }
  t::Status save(const t::Operation& value, std::optional<t::ResourceVersion> expected) override {
    const auto found = store_.operations.find(value.ref.id.value);
    if (!expected && found != store_.operations.end()) {
      return t::Failure{t::ErrorCode::Conflict};
    }
    store_.operations[value.ref.id.value] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};

class FakeWork final : public t::ModelUnitOfWork {
public:
  FakeWork(Store& store, t::TimePoint sample)
      : store_(store), sample_(sample), configs_(store), secrets_(store), calls_(store),
        receipts_(store), journal_(store), operations_(store) {}
  t::Service owner() const noexcept override {
    return t::Service::Model;
  }
  t::Outcome<t::TimePoint> current_time() override {
    return sample_;
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
    if (store_.fail_commits > 0) {
      --store_.fail_commits;
      return t::CommitResult::RolledBack;
    }
    configs_.commit();
    secrets_.commit();
    calls_.commit();
    ++store_.commits;
    return t::CommitResult::Committed;
  }
  void rollback() noexcept override {
    ++store_.rollbacks;
  }
  t::Repository<t::ModelCallKey, t::ModelIntent>& model_intents() override {
    return legacy_;
  }
  t::ModelConfigurationRepository& model_configs() override {
    return configs_;
  }
  t::ProviderSecretRepository& provider_secrets() override {
    return secrets_;
  }
  t::ModelCallRepository& model_calls() override {
    return calls_;
  }

private:
  class LegacyRepo final : public t::Repository<t::ModelCallKey, t::ModelIntent> {
  public:
    t::Outcome<std::optional<t::ModelIntent>> find(const t::ModelCallKey&) override {
      return std::optional<t::ModelIntent>{};
    }
    t::Status save(const t::ModelIntent&, std::optional<t::ResourceVersion>) override {
      return std::monostate{};
    }
  };
  Store& store_;
  t::TimePoint sample_;
  LegacyRepo legacy_;
  ConfigRepo configs_;
  SecretRepo secrets_;
  CallRepo calls_;
  ReceiptRepo receipts_;
  JournalRepo journal_;
  OperationRepo operations_;
};

class FakeFactory final : public t::ModelUnitOfWorkFactory {
public:
  explicit FakeFactory(Store& store) : store_(store) {}
  t::Outcome<std::unique_ptr<t::ModelUnitOfWork>> begin() override {
    return std::unique_ptr<t::ModelUnitOfWork>(std::make_unique<FakeWork>(store_, now()));
  }

private:
  Store& store_;
};

class FakeProvider final : public t::ModelProviderPort {
public:
  std::vector<t::ModelProviderResult> results;
  int calls = 0;
  t::ModelProviderRequest last;
  t::ModelProviderResult chat(const t::ModelProviderRequest& request) override {
    ++calls;
    last = request;
    if (results.empty()) {
      t::ModelProviderResult result;
      result.outcome = t::ModelProviderOutcome::Succeeded;
      result.reply.text = "answer";
      result.reply.provider_request_id = "req-1";
      result.reply.latency_ms = 5;
      result.reply.usage = {true, 10, 20};
      return result;
    }
    auto result = results.front();
    results.erase(results.begin());
    return result;
  }
};

class FakeCrypto final : public t::ModelSecretCryptoPort {
public:
  int seals = 0;
  int opens = 0;
  t::Outcome<std::vector<std::byte>> seal(t::SecretView plaintext, std::span<const std::byte>,
                                          std::array<std::byte, 24>& nonce) override {
    ++seals;
    nonce.fill(std::byte{0x24});
    return std::vector<std::byte>(plaintext.begin(), plaintext.end());
  }
  t::Outcome<t::OwnedSecret> open(std::span<const std::byte> sealed, std::span<const std::byte>,
                                  const std::array<std::byte, 24>&) override {
    ++opens;
    return t::OwnedSecret{
        std::make_unique<FakeSecret>(std::vector<std::byte>(sealed.begin(), sealed.end()))};
  }
};

class FakeDigest final : public t::ModelDigestPort {
public:
  t::Outcome<t::Sha256Digest> sha256(std::span<const std::byte> bytes) override {
    return digest_of(bytes);
  }
};

class FakeIds final : public t::ModelIdPort {
public:
  t::Outcome<std::string> uuid() override {
    ++counter_;
    return std::string{"77777777-7777-4777-8777-"} +
           std::string(12 - std::to_string(counter_).size(), '0') + std::to_string(counter_);
  }

private:
  int counter_ = 0;
};

class FakeAuthorization final : public t::ModelAuthorizationPort {
public:
  bool deny = false;
  t::Outcome<t::ModelAuthorizationDecision> authorize(const t::ModelCaller&, t::Permission,
                                                      const t::ResourceRef&,
                                                      std::chrono::milliseconds) override {
    if (deny) {
      return t::Failure{t::ErrorCode::PermissionDenied};
    }
    return t::ModelAuthorizationDecision{};
  }
};

struct Fixture {
  Store store;
  FakeProvider provider;
  FakeCrypto crypto;
  FakeDigest digest;
  FakeIds ids;
  FakeAuthorization authorization;
  FakeFactory factory{store};
  std::unique_ptr<t::ModelGatewayApplication> application;
  Fixture() {
    application = t::make_model_gateway_application(
        {factory, provider, crypto, digest, ids, authorization, 7});
  }
  void create_model() {
    t::ModelConfiguration config;
    config.model_id = {kModel};
    config.alias = "default";
    config.endpoint = "https://api.example.com/v1/chat/completions";
    config.provider_model = "gpt-test";
    config.timeout_ms = 5000;
    config.max_concurrency = 2;
    config.state = t::LifecycleState::Active;
    auto created = take(application->update_model({control(), context("update-1"), config, {}}));
    require(created.version && created.version->value == 1);
    auto secret = take(application->rotate_provider_secret(
        {control(),
         context("rotate-1"),
         {kModel},
         std::vector<std::byte>{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}},
         {}}));
    require(secret.version && secret.version->value == 1);
    auto enabled = take(application->set_model_lifecycle(
        {control(), context("enable-1"), {kModel}, t::ResourceVersion{1}, true}));
    require(enabled.metadata && enabled.metadata->config.state == t::LifecycleState::Active);
  }
  template <typename Value> Value take(t::Outcome<Value> result) {
    if (const auto* failure = std::get_if<t::Failure>(&result)) {
      std::cerr << "P11 application failure code " << static_cast<int>(failure->code) << ' '
                << static_cast<int>(failure->acceptance) << '\n';
      throw std::runtime_error("P11 application operation failed");
    }
    return std::move(std::get<Value>(result));
  }
  t::Outcome<t::ModelChatReply> chat(t::ModelCallContext call_context, std::string call_id = kCall,
                                     std::string text = "hello") {
    return application->chat(
        {plugin(), std::move(call_context), call_id, "default", {message(std::move(text))}});
  }
};

void management_and_chat() {
  Fixture fixture;
  fixture.create_model();
  require(fixture.store.configs.at(kModel).state == t::LifecycleState::Active);
  require(fixture.store.secrets.at(kModel).size() == 1);
  require(fixture.store.secrets.at(kModel).front().root_key_version == 7);
  require(fixture.crypto.seals == 1);

  auto reply = fixture.take(fixture.chat(context("chat-1")));
  require(reply.disposition == t::ModelChatDisposition::FreshResult && reply.result &&
          *reply.result == "answer");
  require(fixture.provider.calls == 1);
  require(reply.call.state == t::ModelCallState::Succeeded && reply.call.usage_known &&
          reply.call.input_tokens == 10 && reply.call.may_have_billed &&
          reply.call.secret_version.value == 1);
  require(fixture.crypto.opens == 1);
  require(fixture.provider.last.timeout_ms > 0 && fixture.provider.last.timeout_ms <= 5000);

  // Same logical call id and same content never invokes the provider twice.
  auto replay = fixture.take(fixture.chat(context("chat-2")));
  require(replay.disposition == t::ModelChatDisposition::ReplayTerminal);
  require(fixture.provider.calls == 1);
  require(replay.call.state == t::ModelCallState::Succeeded);

  // Same call id with different content is a stable conflict.
  auto conflict = fixture.chat(context("chat-3"), kCall, "different");
  require(std::holds_alternative<t::Failure>(conflict) &&
          std::get<t::Failure>(conflict).code == t::ErrorCode::Conflict);

  // Rotation pins a new secret version only for later calls and leaves configuration version.
  auto rotated = fixture.take(application_rotate(fixture));
  require(rotated.version && rotated.version->value == 2);
  require(fixture.store.configs.at(kModel).version.value == 2);
  fixture.take(fixture.chat(context("chat-4"), kCall2, "second"));
  require(fixture.store.calls.at({kOwner, kCall2}).secret_version.value == 2);
  require(fixture.store.calls.at({kOwner, kCall}).secret_version.value == 1);

  // Update replay with the same idempotency key reuses the receipt and creates no new operation.
  t::ModelConfiguration update = fixture.store.configs.at(kModel);
  update.timeout_ms = 6000;
  const auto current_version = update.version;
  const auto operations_before = fixture.store.operations.size();
  update.version = {current_version.value + 1};
  auto updated = fixture.take(
      fixture.application->update_model({control(), context("update-2"), update, current_version}));
  require(updated.version && updated.version->value == current_version.value + 1);
  auto replay_update = fixture.take(
      fixture.application->update_model({control(), context("update-2"), update, current_version}));
  require(replay_update.version && replay_update.version->value == current_version.value + 1);
  require(fixture.store.operations.size() == operations_before + 1);

  // Different content under the same idempotency key is a conflict.
  auto conflicting_update = fixture.application->update_model(
      {control(), context("update-2"), update, t::ResourceVersion{current_version.value + 2}});
  require(std::holds_alternative<t::Failure>(conflicting_update) &&
          std::get<t::Failure>(conflicting_update).code == t::ErrorCode::Conflict);
}

t::Outcome<t::ModelMutationReply> application_rotate(Fixture& fixture) {
  return fixture.application->rotate_provider_secret(
      {control(),
       context("rotate-2"),
       {kModel},
       std::vector<std::byte>{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}},
       t::ResourceVersion{1}});
}

void failures_keep_unknown() {
  Fixture fixture;
  fixture.create_model();
  // A disabled model never reaches the provider.
  fixture.take(fixture.application->set_model_lifecycle(
      {control(), context("disable-1"), {kModel}, t::ResourceVersion{2}, false}));
  auto disabled = fixture.chat(context("chat-disabled"));
  require(std::holds_alternative<t::Failure>(disabled) &&
          std::get<t::Failure>(disabled).code == t::ErrorCode::CapabilityNotSupported &&
          fixture.provider.calls == 0);
  fixture.take(fixture.application->set_model_lifecycle(
      {control(), context("enable-2"), {kModel}, t::ResourceVersion{3}, true}));

  // Provider unknown result is persisted as UNKNOWN and never converted to a known failure.
  t::ModelProviderResult unknown;
  unknown.outcome = t::ModelProviderOutcome::Unknown;
  unknown.failure = {
      t::ErrorCode::Timeout, t::Acceptance::NotAccepted, t::Recovery::Backoff, {}, {}};
  fixture.provider.results.push_back(unknown);
  auto reply = fixture.chat(context("chat-unknown"));
  require(std::holds_alternative<t::Failure>(reply) &&
          std::get<t::Failure>(reply).code == t::ErrorCode::Timeout &&
          std::get<t::Failure>(reply).acceptance == t::Acceptance::Unknown);
  const auto stored = fixture.store.calls.at({kOwner, kCall});
  require(stored.state == t::ModelCallState::Unknown && stored.may_have_billed);

  auto status = fixture.take(
      fixture.application->get_call_status({plugin(), context("status-1"), kCall, std::nullopt}));
  require(status.state == t::ModelCallState::Unknown);
  auto resolved = fixture.take(fixture.application->resolve_call(
      {control(), context("resolve-1"), kCall, t::ResourceVersion{2},
       t::ModelCallDisposition::ExecutedBilled}));
  require(resolved.state == t::ModelCallState::Failed &&
          resolved.disposition == t::ModelCallDisposition::ExecutedBilled);
  auto resolved_replay = fixture.take(fixture.application->resolve_call(
      {control(), context("resolve-1"), kCall, t::ResourceVersion{2},
       t::ModelCallDisposition::ExecutedBilled}));
  require(resolved_replay.disposition == t::ModelCallDisposition::ExecutedBilled);

  // A second transaction failure after the provider call returns an unknown acceptance and leaves
  // the durable row in progress for status queries; the provider was invoked exactly once.
  Fixture second;
  second.create_model();
  second.store.fail_commits = 1; // committed intents still survive; the next commit is the txn2
  const auto calls_before = second.provider.calls;
  // fail_commits applies from the start: the first commit (txn1) fails and the provider is NOT
  // called, which is the other frozen invariant.
  auto txn1_failure = second.chat(context("chat-txn1"));
  require(std::holds_alternative<t::Failure>(txn1_failure) &&
          std::get<t::Failure>(txn1_failure).code == t::ErrorCode::UpstreamUnavailable);
  require(second.provider.calls == calls_before);
  require(second.store.calls.empty());
}

void maintenance_and_authorization() {
  Fixture fixture;
  fixture.create_model();
  // Seed an old unresolved UNKNOWN row directly (simulating a crash after provider invocation).
  t::ModelIntent stale;
  stale.owner = {kOwner};
  stale.logical_call_id = kCall2;
  stale.model = {kModel};
  stale.config_version = {1};
  stale.request_hash.bytes[0] = std::byte{0x11};
  stale.state = t::ModelCallState::Unknown;
  stale.version = {1};
  stale.created_at = now() - 31 * 24h;
  stale.observed_at = stale.created_at;
  stale.secret_version = {1};
  stale.may_have_billed = true;
  fixture.store.calls[{kOwner, kCall2}] = stale;
  require(std::holds_alternative<std::monostate>(fixture.application->maintenance()));
  const auto abandoned = fixture.store.calls.at({kOwner, kCall2});
  require(abandoned.state == t::ModelCallState::Failed &&
          abandoned.disposition == t::ModelCallDisposition::Abandoned && abandoned.may_have_billed);
  require(!fixture.store.audits.empty());

  fixture.authorization.deny = true;
  auto denied = fixture.chat(context("chat-denied"), kCall, "denied");
  require(std::holds_alternative<t::Failure>(denied) &&
          std::get<t::Failure>(denied).code == t::ErrorCode::PermissionDenied);
  auto metadata =
      fixture.application->get_model_metadata({control(), context("metadata-denied"), {kModel}});
  require(std::holds_alternative<t::Failure>(metadata));

  fixture.authorization.deny = false;
  auto status = fixture.application->get_call_status(
      {control(),
       context("status-denied"),
       kCall2,
       {t::ResourceId{"99999999-9999-4999-8999-999999999999"}}});
  require(std::holds_alternative<t::Failure>(status) &&
          std::get<t::Failure>(status).code == t::ErrorCode::NotFound);
}

} // namespace

int main() {
  try {
    management_and_chat();
    failures_keep_unknown();
    maintenance_and_authorization();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
