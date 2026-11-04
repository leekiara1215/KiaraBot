#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/application/target/runtime_storage_application.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"

namespace t = kiarabot::core::target;
using std::chrono_literals::operator""h;
using std::chrono_literals::operator""min;

void check(bool value, std::string_view name) {
  if (!value) {
    std::cerr << "P09a assertion: " << name << '\n';
    std::exit(1);
  }
}
t::Failure unavailable() {
  return {t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted, t::Recovery::Backoff,
          std::nullopt, std::nullopt};
}
template <typename Value> bool error(const t::Outcome<Value>& result, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&result);
  return failure && failure->code == code;
}
std::string uuid(unsigned char seed) {
  std::string value = "00000000-0000-4000-8000-000000000000";
  constexpr char digits[] = "0123456789abcdef";
  value[35] = digits[seed & 0x0f];
  value[34] = digits[(seed >> 4) & 0x0f];
  return value;
}
std::string scope_key(const t::NamespaceScope& scope) {
  return std::holds_alternative<t::PrivateNamespace>(scope)
             ? std::get<t::PrivateNamespace>(scope).instance.value
             : std::get<t::SharedNamespace>(scope).shared_namespace.value;
}
t::TimePoint at(std::chrono::seconds seconds) {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(seconds)};
}
t::Sha256Digest digest(unsigned char seed) {
  t::Sha256Digest value;
  value.bytes[0] = std::byte{seed};
  return value;
}
t::TimePoint base_now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}

struct Store {
  t::TimePoint now = base_now();
  bool fail_reads = false;
  bool fail_writes = false;
  bool unknown_commit = false;
  int commits = 0;
  int rollbacks = 0;
  std::map<std::string, t::PluginInstallation> installations;
  std::map<std::string, t::PluginInstance> instances;
  std::map<std::string, t::SharedNamespaceRecord> namespaces;
  std::map<std::string, std::vector<t::SharedMemberRecord>> members;
  std::map<std::string, t::NamespaceValue> values;
  std::map<std::string, t::NamespaceReceipt> receipts;
  std::map<std::string, t::Operation> operations;
  std::vector<t::AuditEntry> audits;
};

std::string value_key(const t::NamespaceScope& scope, const std::string& key) {
  return scope_key(scope) + '\n' + key;
}
std::string namespace_of(const t::NamespaceScope& scope) {
  return scope_key(scope);
}

template <typename Key, typename Value>
class MemoryRepository final : public t::Repository<Key, Value> {
public:
  explicit MemoryRepository(Store& store) : store_(store) {}
  t::Outcome<std::optional<Value>> find(const Key&) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    return std::optional<Value>{};
  }
  t::Status save(const Value&, std::optional<t::ResourceVersion>) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    return t::Failure{t::ErrorCode::CapabilityNotSupported, t::Acceptance::NotAccepted,
                      t::Recovery::Diagnose, std::nullopt, std::nullopt};
  }

private:
  Store& store_;
};
class InstallationRepository final
    : public t::Repository<t::InstallationId, t::PluginInstallation> {
public:
  explicit InstallationRepository(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::PluginInstallation>> find(const t::InstallationId& key) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    const auto found = store_.installations.find(key.value);
    return found == store_.installations.end()
               ? std::nullopt
               : std::optional<t::PluginInstallation>{found->second};
  }
  t::Status save(const t::PluginInstallation& value,
                 std::optional<t::ResourceVersion> expected) override {
    (void)value;
    (void)expected;
    return unavailable();
  }

private:
  Store& store_;
};
class InstanceRepository final : public t::Repository<t::InstanceId, t::PluginInstance> {
public:
  explicit InstanceRepository(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::PluginInstance>> find(const t::InstanceId& key) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    const auto found = store_.instances.find(key.value);
    return found == store_.instances.end() ? std::nullopt
                                           : std::optional<t::PluginInstance>{found->second};
  }
  t::Status save(const t::PluginInstance& value, std::optional<t::ResourceVersion>) override {
    store_.instances[value.id.value] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};
class OperationRepository final : public t::Repository<t::OperationId, t::Operation> {
public:
  explicit OperationRepository(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::Operation>> find(const t::OperationId& key) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    const auto found = store_.operations.find(key.value);
    return found == store_.operations.end() ? std::nullopt
                                            : std::optional<t::Operation>{found->second};
  }
  t::Status save(const t::Operation& value, std::optional<t::ResourceVersion> expected) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    const auto found = store_.operations.find(value.ref.id.value);
    if (!expected ? found != store_.operations.end()
                  : (found == store_.operations.end() ||
                     found->second.version.value != expected->value)) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    store_.operations[value.ref.id.value] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};
class Namespaces final : public t::NamespaceRepository {
public:
  explicit Namespaces(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::NamespaceValue>>
  get(const t::NamespaceScope& scope, const std::string& key, t::TimePoint now) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    const auto found = store_.values.find(value_key(scope, key));
    if (found == store_.values.end() || t::expired(found->second, now)) {
      return std::optional<t::NamespaceValue>{};
    }
    return std::optional<t::NamespaceValue>{found->second};
  }
  t::Outcome<t::ResourceVersion> compare_exchange(const t::NamespaceScope& scope,
                                                  const std::string& key,
                                                  const t::CompareCondition& expected,
                                                  const t::NamespaceValue& value,
                                                  t::TimePoint now) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    const auto found = store_.values.find(value_key(scope, key));
    const bool present = found != store_.values.end() && !t::expired(found->second, now);
    if (const auto* absent = std::get_if<t::ExpectedAbsent>(&expected)) {
      (void)absent;
      if (present) {
        return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                          t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
      }
    } else if (!present || found->second.version != std::get<t::ResourceVersion>(expected)) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    store_.values[value_key(scope, key)] = value;
    return value.version;
  }
  t::Status compare_remove(const t::NamespaceScope& scope, const std::string& key,
                           const t::CompareCondition& expected, t::TimePoint now) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    const auto found = store_.values.find(value_key(scope, key));
    const bool present = found != store_.values.end() && !t::expired(found->second, now);
    if (!present || std::holds_alternative<t::ExpectedAbsent>(expected) ||
        found->second.version != std::get<t::ResourceVersion>(expected)) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    store_.values.erase(found);
    return std::monostate{};
  }

private:
  Store& store_;
};
class SharedNamespaces final : public t::SharedNamespaceRepository {
public:
  explicit SharedNamespaces(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::SharedNamespaceRecord>> find(const t::ResourceId& id) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    const auto found = store_.namespaces.find(id.value);
    return found == store_.namespaces.end()
               ? std::nullopt
               : std::optional<t::SharedNamespaceRecord>{found->second};
  }
  t::Outcome<std::optional<t::SharedNamespaceRecord>>
  by_installation(const t::InstallationId& installation) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    for (const auto& [id, record] : store_.namespaces) {
      (void)id;
      if (record.installation == installation) {
        return std::optional<t::SharedNamespaceRecord>{record};
      }
    }
    return std::optional<t::SharedNamespaceRecord>{};
  }
  t::Status save(const t::SharedNamespaceRecord& record,
                 std::optional<t::ResourceVersion> expected) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    const auto found = store_.namespaces.find(record.namespace_id.value);
    if (!expected ? found != store_.namespaces.end()
                  : (found == store_.namespaces.end() || found->second.version != *expected ||
                     record.version.value != expected->value + 1)) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    store_.namespaces[record.namespace_id.value] = record;
    return std::monostate{};
  }
  t::Outcome<std::vector<t::SharedMemberRecord>> members(const t::ResourceId& id) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    return store_.members[id.value];
  }
  t::Status save_members(const t::ResourceId& id, t::ResourceVersion expected,
                         const std::vector<t::SharedMember>& members,
                         t::ResourceVersion next_version, t::TimePoint now) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    const auto found = store_.namespaces.find(id.value);
    if (found == store_.namespaces.end() || found->second.version != expected) {
      return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    found->second.version = next_version;
    found->second.updated_at = now;
    auto& list = store_.members[id.value];
    list.clear();
    for (const auto& member : members) {
      list.push_back({member, {1}});
    }
    return std::monostate{};
  }

private:
  Store& store_;
};
class Quota final : public t::NamespaceQuotaRepository {
public:
  explicit Quota(Store& store) : store_(store) {}
  t::Outcome<t::NamespaceQuota> usage(const t::NamespaceScope& scope,
                                      const t::InstallationId&) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    return usage_for(scope);
  }
  t::Status admit(const t::NamespaceScope& scope, const t::InstallationId&,
                  t::NamespaceQuotaChange change) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    auto current = usage_for(scope);
    const auto keys = static_cast<std::int64_t>(current.namespace_keys_used) + change.keys;
    const auto bytes = static_cast<std::int64_t>(current.namespace_bytes_used) + change.bytes;
    if (keys < 0 || bytes < 0 || keys > static_cast<std::int64_t>(t::kMaximumNamespaceKeys) ||
        bytes > static_cast<std::int64_t>(t::kMaximumNamespaceBytes)) {

      return t::Failure{t::ErrorCode::RateLimited, t::Acceptance::NotAccepted, t::Recovery::Backoff,
                        std::nullopt, std::chrono::milliseconds{1000}};
    }
    return std::monostate{};
  }

private:
  t::NamespaceQuota usage_for(const t::NamespaceScope& scope) {
    t::NamespaceQuota result;
    const auto id = namespace_of(scope);
    for (const auto& [key, value] : store_.values) {
      if (key.rfind(id + '\n', 0) != 0) {
        continue;
      }
      ++result.namespace_keys_used;
      result.namespace_bytes_used += value.bytes.size();
    }
    return result;
  }
  Store& store_;
};
class StorageReceipts final : public t::StorageReceiptRepository {
public:
  explicit StorageReceipts(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::NamespaceReceipt>> find(const t::ReceiptKey& key) override {
    if (store_.fail_reads) {
      return unavailable();
    }
    const auto found = store_.receipts.find(key.idempotency_key);
    return found == store_.receipts.end() ? std::nullopt
                                          : std::optional<t::NamespaceReceipt>{found->second};
  }
  t::Outcome<t::NamespaceReceiptWrite> insert(const t::NamespaceReceipt& receipt,
                                              t::NamespaceReceiptCapacity capacity) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    const auto found = store_.receipts.find(receipt.key.idempotency_key);
    if (found != store_.receipts.end()) {
      if (found->second.fingerprint != receipt.fingerprint) {
        return t::Failure{t::ErrorCode::Conflict, t::Acceptance::NotAccepted,
                          t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
      }
      return t::NamespaceReceiptWrite{found->second, true};
    }
    std::size_t instance_count = 0;
    std::size_t installation_count = 0;
    for (const auto& [key, stored] : store_.receipts) {
      (void)key;
      if (!receipt.instance.value.empty() && stored.instance == receipt.instance) {
        ++instance_count;
      }
      if (stored.installation == receipt.installation) {
        ++installation_count;
      }
    }
    if (instance_count >= capacity.instance_max ||
        installation_count >= capacity.installation_max) {
      return t::Failure{t::ErrorCode::RateLimited, t::Acceptance::NotAccepted, t::Recovery::Backoff,
                        std::nullopt, std::chrono::milliseconds{1000}};
    }
    store_.receipts[receipt.key.idempotency_key] = receipt;
    return t::NamespaceReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class CommandReceipts final : public t::ReceiptRepository {
public:
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey&) override {
    return std::optional<t::CommandReceipt>{};
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt&, t::ReceiptCapacity) override {
    return unavailable();
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }
};
class Journal final : public t::JournalRepository {
public:
  explicit Journal(Store& store) : store_(store) {}
  t::Status append_audit(const t::AuditEntry& entry) override {
    if (store_.fail_writes) {
      return unavailable();
    }
    store_.audits.push_back(entry);
    return std::monostate{};
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    return unavailable();
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    return unavailable();
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    return unavailable();
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    return unavailable();
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    return unavailable();
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};

class MemoryWork final : public t::RuntimeUnitOfWork {
public:
  explicit MemoryWork(Store& store)
      : store_(store), namespaces_(store), shared_(store), quota_(store), receipts_(store),
        journal_(store), installations_(store), instances_(store), operations_(store),
        unused_accounts_(store), unused_bindings_(store), unused_hosts_(store),
        unused_configs_(store) {}
  t::Service owner() const noexcept override {
    return t::Service::Runtime;
  }
  t::Outcome<t::TimePoint> current_time() override {
    return store_.now;
  }
  t::ReceiptRepository& receipts() override {
    return command_receipts_;
  }
  t::JournalRepository& journal() override {
    return journal_;
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    return operations_;
  }
  t::CommitResult commit() override {
    ++store_.commits;
    return store_.unknown_commit ? t::CommitResult::Unknown : t::CommitResult::Committed;
  }
  void rollback() noexcept override {
    ++store_.rollbacks;
  }
  t::Repository<t::ResourceId, t::BotAccount>& accounts() override {
    return unused_accounts_;
  }
  t::Repository<t::ResourceId, t::AdapterBindingRecord>& adapter_bindings() override {
    return unused_bindings_;
  }
  t::AdapterRegistryRepository& adapters() override {
    return unused_adapters_;
  }
  t::BridgeCredentialRepository& bridge_credentials() override {
    return unused_credentials_;
  }
  t::Outcome<std::size_t> active_bindings(const t::InstanceId&) override {
    return std::size_t{0};
  }
  t::Repository<t::ResourceId, t::HostRegistration>& hosts() override {
    return unused_hosts_;
  }
  t::Repository<t::InstallationId, t::PluginInstallation>& installations() override {
    return installations_;
  }
  t::Repository<t::InstanceId, t::PluginInstance>& instances() override {
    return instances_;
  }
  t::Repository<t::InstanceId, t::InstanceConfiguration>& instance_configs() override {
    return unused_configs_;
  }
  t::EventRepository& events() override {
    return unused_events_;
  }
  t::ActionRepository& actions() override {
    return unused_actions_;
  }
  t::ActionQuotaRepository& action_quota() override {
    return unused_actions_;
  }
  t::ActionPolicyRepository& action_policies() override {
    return unused_actions_;
  }
  t::ActionEvidenceRepository& action_evidence() override {
    return unused_actions_;
  }
  t::Outcome<std::vector<t::BotAccount>> list_accounts(std::size_t, const std::string&) override {
    return std::vector<t::BotAccount>{};
  }
  t::Outcome<std::vector<t::AdapterRecord>> list_adapters(std::size_t,
                                                          const std::string&) override {
    return std::vector<t::AdapterRecord>{};
  }
  t::Outcome<std::vector<t::HostRegistration>> list_hosts(std::size_t,
                                                          const std::string&) override {
    return std::vector<t::HostRegistration>{};
  }
  t::Outcome<std::vector<t::PluginInstallation>> list_installations(std::size_t,
                                                                    const std::string&) override {
    return std::vector<t::PluginInstallation>{};
  }
  t::Outcome<std::vector<t::PluginInstance>> list_instances(std::size_t,
                                                            const std::string&) override {
    return std::vector<t::PluginInstance>{};
  }
  t::NamespaceRepository& namespaces() override {
    return namespaces_;
  }
  t::SharedNamespaceRepository& shared_namespaces() override {
    return shared_;
  }
  t::NamespaceQuotaRepository& namespace_quota() override {
    return quota_;
  }
  t::StorageReceiptRepository& storage_receipts() override {
    return receipts_;
  }

private:
  Store& store_;
  Namespaces namespaces_;
  SharedNamespaces shared_;
  Quota quota_;
  StorageReceipts receipts_;
  CommandReceipts command_receipts_;
  Journal journal_;
  InstallationRepository installations_;
  InstanceRepository instances_;
  OperationRepository operations_;
  MemoryRepository<t::ResourceId, t::BotAccount> unused_accounts_;
  MemoryRepository<t::ResourceId, t::AdapterBindingRecord> unused_bindings_;
  MemoryRepository<t::ResourceId, t::HostRegistration> unused_hosts_;
  MemoryRepository<t::InstanceId, t::InstanceConfiguration> unused_configs_;
  class UnusedAdapters final : public t::AdapterRegistryRepository {
  public:
    t::Outcome<std::optional<t::AdapterRecord>> find(const t::ResourceId&) override {
      return std::optional<t::AdapterRecord>{};
    }
    t::Outcome<std::optional<t::AdapterRecord>> by_instance(const t::InstanceId&) override {
      return std::optional<t::AdapterRecord>{};
    }
    t::Status save(const t::AdapterRecord&, std::optional<t::ResourceVersion>) override {
      return unavailable();
    }
  } unused_adapters_;
  class UnusedCredentials final : public t::BridgeCredentialRepository {
  public:
    t::Outcome<bool> active(const std::string&) override {
      return false;
    }
    t::Outcome<std::optional<t::BridgeCredentialRecord>>
    by_digest(const t::Sha256Digest&) override {
      return std::optional<t::BridgeCredentialRecord>{};
    }
    t::Status insert(const t::BridgeCredentialRecord&) override {
      return unavailable();
    }
    t::Status revoke(const std::string&, t::TimePoint) override {
      return unavailable();
    }
  } unused_credentials_;
  class UnusedEvents final : public t::EventRepository {
  public:
    t::Outcome<t::EventAcceptanceResult> accept(const t::EventRecord&,
                                                const std::vector<t::DeliveryRecord>&,
                                                const t::EventCapacityLimits&) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::DeliveryClaim>> claim(const t::DeliveryClaimRequest&) override {
      return unavailable();
    }
    t::Outcome<std::optional<t::DeliveryAttempt>>
    find_attempt(const t::InstanceId&, std::string_view, const t::AttemptId&) override {
      return unavailable();
    }
    t::Outcome<t::DeliveryReceipt> acknowledge(const t::DeliveryAcknowledgement&,
                                               t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<t::DeliveryReceipt> renew(const t::DeliveryAttempt&, t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<t::DeliverySweep> sweep(t::TimePoint, const t::RetryPolicy&, double) override {
      return unavailable();
    }
    t::Outcome<std::size_t> recover(t::TimePoint, const t::RetryPolicy&, double) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::DeadLetter>> list_dead_letters(const std::optional<t::InstanceId>&,
                                                             std::size_t) override {
      return unavailable();
    }
    t::Outcome<std::optional<t::DeliveryRecord>> replay(const t::ReplayRequest&) override {
      return unavailable();
    }
    t::Outcome<t::EventCapacity> capacity(const t::EventCapacityLimits&) override {
      return unavailable();
    }
    t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
      return unavailable();
    }
  } unused_events_;
  class UnusedActions final : public t::ActionRepository,
                              public t::ActionQuotaRepository,
                              public t::ActionPolicyRepository,
                              public t::ActionEvidenceRepository {
  public:
    t::Outcome<std::optional<t::ActionRecord>> find_by_id(const t::ActionId&) override {
      return unavailable();
    }
    t::Outcome<std::optional<t::ActionRecord>> find_by_owner_key(const t::InstanceId&,
                                                                 std::string_view) override {
      return unavailable();
    }
    t::Outcome<t::ActionRecord> insert(const t::ActionRecord&, t::ActionCapacity) override {
      return unavailable();
    }
    t::Status mark_dispatched(const t::ActionId&, const t::AttemptId&, const t::InstanceId&,
                              t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<t::ActionTerminalReceipt> record_terminal(const t::ActionRecord&,
                                                         const t::ActionTerminalResult&,
                                                         std::optional<t::AttemptId>,
                                                         std::optional<t::InstanceId>,
                                                         t::TimePoint) override {
      return unavailable();
    }
    t::Status cancel(const t::ActionId&, t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::ActionRecord>> expire_due(std::size_t, t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::ActionRecord>> list_owner(const t::InstanceId&,
                                                        const t::ActionPageRequest&) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::ActionRecord>> list_control(const t::ActionControlQuery&) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::ActionRecord>> list_due(const t::ResourceId&, std::size_t,
                                                      t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<std::size_t> prune_terminal(t::TimePoint, std::size_t) override {
      return unavailable();
    }
    t::Status admit(const t::ActionId&, t::ActionKind, std::span<const t::ActionScope>,
                    const t::ActionQuotaLimits&, t::TimePoint) override {
      return unavailable();
    }
    t::Outcome<std::size_t> prune_windows(t::TimePoint, std::size_t) override {
      return unavailable();
    }
    t::Outcome<std::optional<t::ActionPolicy>> get(const t::ResourceId&) override {
      return unavailable();
    }
    t::Status upsert(const t::ActionPolicy&, std::optional<t::ResourceVersion>) override {
      return unavailable();
    }
    t::Status append(const t::ActionEvidenceRecord&) override {
      return unavailable();
    }
    t::Outcome<std::vector<t::ActionEvidenceRecord>> list(const t::ActionId&) override {
      return unavailable();
    }
    t::Outcome<std::size_t> prune(t::TimePoint, std::size_t) override {
      return unavailable();
    }
  } unused_actions_;
};
class MemoryFactory final : public t::RuntimeUnitOfWorkFactory {
public:
  explicit MemoryFactory(Store& store) : store_(store) {}
  t::Outcome<std::unique_ptr<t::RuntimeUnitOfWork>> begin() override {
    if (store_.fail_reads) {
      return unavailable();
    }
    return std::unique_ptr<t::RuntimeUnitOfWork>(std::make_unique<MemoryWork>(store_));
  }

private:
  Store& store_;
};
class Ids final : public t::RuntimeIdPort {
public:
  t::Outcome<t::ResourceId> account_id() override {
    return t::ResourceId{uuid(++seed_)};
  }
  t::Outcome<t::InstallationId> installation_id() override {
    return t::InstallationId{uuid(++seed_)};
  }
  t::Outcome<t::InstanceId> instance_id() override {
    return t::InstanceId{uuid(++seed_)};
  }
  t::Outcome<t::InstanceId> adapter_instance_id() override {
    return t::InstanceId{uuid(++seed_)};
  }
  t::Outcome<t::ConnectionId> connection_id() override {
    return t::ConnectionId{uuid(++seed_)};
  }
  t::Outcome<t::OperationId> operation_id() override {
    return t::OperationId{uuid(++seed_)};
  }
  t::Outcome<t::RuntimeEpoch> runtime_epoch() override {
    return t::RuntimeEpoch{"epoch"};
  }
  t::Outcome<std::string> snapshot_id() override {
    return uuid(++seed_);
  }

private:
  unsigned char seed_ = 0;
};
class Authorizer final : public t::RuntimeAuthorizationPort {
public:
  bool allow = true;
  t::Permission last_permission = t::Permission::Unspecified;
  std::vector<t::ResourceRef> last_scope;
  bool available() const noexcept override {
    return true;
  }
  t::Outcome<t::RuntimeAuthorizationDecision>
  check(const t::RuntimeAuthorizationRequest& request) override {
    last_permission = request.permission;
    last_scope = request.scope;
    if (!allow) {
      return t::Failure{t::ErrorCode::PermissionDenied, t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent, std::nullopt, std::nullopt};
    }
    return t::RuntimeAuthorizationDecision{true,
                                           request.authentication.subject,
                                           request.permission,
                                           request.scope,
                                           {7},
                                           base_now(),
                                           std::nullopt};
  }
};

struct Fixture {
  Store store;
  MemoryFactory factory{store};
  Ids ids;
  Authorizer authorizer;
  std::unique_ptr<t::RuntimeStorageApplication> app;
  std::string installation = uuid(1);
  std::string instance = uuid(2);
  std::string namespace_id = uuid(3);
  std::string subject = uuid(4);

  Fixture() {
    t::PluginInstallation row;
    row.id = t::InstallationId{installation};
    row.plugin_type = {"plugin"};
    row.manifest = {};
    row.version = {1};
    store.installations[installation] = row;
    t::PluginInstance plugin;
    plugin.id = t::InstanceId{instance};
    plugin.installation = t::InstallationId{installation};
    plugin.plugin_type = {"plugin"};
    plugin.desired_version = {1};
    plugin.applied_config_version = {1};
    plugin.version = {1};
    store.instances[instance] = plugin;
    app = std::move(std::get<std::unique_ptr<t::RuntimeStorageApplication>>(
        t::make_runtime_storage_application({factory, ids, authorizer})));
  }
  t::AuthenticationContext authentication() const {
    return {
        t::SubjectId{subject},
        t::PlatformPeer{t::Service::Control, t::SubjectId{subject}},
        t::Service::Runtime,
        {t::SubjectId{subject}},
        {{t::ResourceRef{t::ResourceKind::Instance, t::ResourceId{instance}}, "storage.namespace"}},
        {1},
        base_now(),
        base_now() + std::chrono::seconds{10}};
  }
  t::StorageCommandContext plugin_context(std::string method = "SetStorage",
                                          std::string idempotency = "0123456789abcdef") const {
    return {authentication(),
            uuid(10),
            uuid(11),
            std::move(method),
            std::move(idempotency),
            t::Sha256Digest{},
            base_now() + std::chrono::seconds{20},
            t::InstanceId{instance},
            t::RequestedScopeKind::DefaultPrivate,
            {},
            std::nullopt};
  }
  t::CommandContext management_context(std::string method = "CreateSharedNamespace",
                                       std::string idempotency = "0123456789abcdef") const {
    return {authentication(),
            uuid(10),
            uuid(11),
            std::move(method),
            {t::ResourceKind::Installation, t::ResourceId{installation}},
            std::move(idempotency),
            t::Sha256Digest{},
            base_now() + std::chrono::seconds{20},
            std::nullopt};
  }
  void shared_namespace() {
    store.namespaces[namespace_id] = {t::ResourceId{namespace_id},
                                      t::InstallationId{installation},
                                      {1},
                                      false,
                                      base_now(),
                                      base_now()};
  }
};

void storage_tests() {
  Fixture fixture;
  auto context = fixture.plugin_context();
  t::StorageSetRequest set{
      context, "key", {std::byte{1}, std::byte{2}}, std::nullopt, std::nullopt};
  auto first = fixture.app->set_storage(set);
  check(std::holds_alternative<t::StorageEntryResult>(first), "private set succeeds");
  check(std::get<t::StorageEntryResult>(first).value.version.value == 1, "private set v1");
  context.idempotency_key = "fedcba9876543210";
  t::StorageGetRequest get{context, "key"};
  auto fetched = fixture.app->get_storage(get);
  check(std::holds_alternative<t::StorageEntryResult>(fetched), "private get succeeds");
  check(std::get<t::StorageEntryResult>(fetched).value.bytes.size() == 2, "private value");
  // Replay returns the original version and never extends TTL.
  auto replay = fixture.app->set_storage(set);
  check(std::holds_alternative<t::StorageEntryResult>(replay), "set replay");
  check(std::get<t::StorageEntryResult>(replay).value.version.value == 1,
        "replay original version");
  auto changed = set;
  changed.value = {std::byte{9}};
  changed.context.fingerprint = digest(9);
  check(error(fixture.app->set_storage(changed), t::ErrorCode::Conflict),
        "same idempotency key changed content conflicts");
  // Private Set must omit condition.
  auto conditioned = set;
  conditioned.context.idempotency_key = "aaaaaaaaaaaaaaaa";
  conditioned.condition = t::ExpectedAbsent{};
  check(error(fixture.app->set_storage(conditioned), t::ErrorCode::InvalidArgument),
        "private set rejects condition");
  // TTL and value/key bounds.
  auto ttl = set;
  ttl.context.idempotency_key = "bbbbbbbbbbbbbbbb";
  ttl.expires_at = fixture.store.now + t::kMaximumStorageTtl + std::chrono::seconds{1};
  check(error(fixture.app->set_storage(ttl), t::ErrorCode::InvalidArgument), "TTL upper bound");
  ttl.expires_at = fixture.store.now;
  check(error(fixture.app->set_storage(ttl), t::ErrorCode::InvalidArgument), "TTL must be future");
  auto oversized = set;
  oversized.context.idempotency_key = "cccccccccccccccc";
  oversized.value.assign(t::kMaximumStorageValueBytes + 1, std::byte{1});
  check(error(fixture.app->set_storage(oversized), t::ErrorCode::RateLimited), "value quota");
  auto long_key = set;
  long_key.context.idempotency_key = "dddddddddddddddd";
  long_key.key = std::string(t::kMaximumStorageKeyBytes + 1, 'k');
  check(error(fixture.app->set_storage(long_key), t::ErrorCode::InvalidArgument), "key bound");
  // CAS: absent create then version mismatch.
  auto cas = t::StorageCompareAndSetRequest{
      context, "cas", {std::byte{1}}, t::ExpectedAbsent{}, std::nullopt};
  cas.context.idempotency_key = "eeeeeeeeeeeeeeee";
  auto created = fixture.app->compare_and_set_storage(cas);
  check(std::holds_alternative<t::StorageEntryResult>(created), "CAS absent creates");
  auto conflict = cas;
  conflict.context.idempotency_key = "ffffffffffffffff";
  check(error(fixture.app->compare_and_set_storage(conflict), t::ErrorCode::Conflict),
        "CAS absent conflicts on existing");
  // Delete absent succeeds, delete existing removes.
  t::StorageDeleteRequest absent{context, "missing", std::nullopt};
  absent.context.idempotency_key = "1111111111111111";
  auto deleted_absent = fixture.app->delete_storage(absent);
  check(std::holds_alternative<t::StorageDeleteResult>(deleted_absent) &&
            !std::get<t::StorageDeleteResult>(deleted_absent).deleted,
        "private delete absent succeeds");
  t::StorageDeleteRequest existing{context, "key", std::nullopt};
  existing.context.idempotency_key = "2222222222222222";
  auto deleted = fixture.app->delete_storage(existing);
  check(std::holds_alternative<t::StorageDeleteResult>(deleted) &&
            std::get<t::StorageDeleteResult>(deleted).deleted,
        "private delete removes");
  // Authorization denial fails closed.
  fixture.authorizer.allow = false;
  auto denied = fixture.app->get_storage(get);
  check(error(denied, t::ErrorCode::PermissionDenied), "authorization denial");
  fixture.authorizer.allow = true;
}

void shared_tests() {
  Fixture fixture;
  fixture.shared_namespace();
  auto context = fixture.plugin_context("SetStorage", "3333333333333333");
  context.requested = t::RequestedScopeKind::InstallationShared;
  t::StorageSetRequest set{context, "key", {std::byte{1}}, std::nullopt, t::ExpectedAbsent{}};
  check(error(fixture.app->set_storage(set), t::ErrorCode::PermissionDenied),
        "shared write needs member");
  fixture.store.members[fixture.namespace_id] = {
      {{t::InstanceId{fixture.instance}, true, true}, {1}}};
  auto written = fixture.app->set_storage(set);
  check(std::holds_alternative<t::StorageEntryResult>(written), "shared set with member");
  check(fixture.authorizer.last_permission == t::Permission::StorageNamespace,
        "shared storage permission");
  // Shared Set requires a condition; version update works.
  auto unconditioned = set;
  unconditioned.context.idempotency_key = "4444444444444444";
  unconditioned.condition = std::nullopt;
  check(error(fixture.app->set_storage(unconditioned), t::ErrorCode::InvalidArgument),
        "shared set requires condition");
  auto update = set;
  update.context.idempotency_key = "5555555555555555";
  update.condition = t::ResourceVersion{1};
  update.value = {std::byte{2}};
  auto updated = fixture.app->set_storage(update);
  check(std::holds_alternative<t::StorageEntryResult>(updated) &&
            std::get<t::StorageEntryResult>(updated).value.version.value == 2,
        "shared version update");
  auto mismatch = update;
  mismatch.context.idempotency_key = "6666666666666666";
  mismatch.condition = t::ResourceVersion{1};
  check(error(fixture.app->set_storage(mismatch), t::ErrorCode::Conflict),
        "shared stale version conflicts");
  // Read gate.
  fixture.store.members[fixture.namespace_id][0].member.can_read = false;
  auto read = fixture.plugin_context("GetStorage", "7777777777777777");
  read.requested = t::RequestedScopeKind::InstallationShared;
  check(error(fixture.app->get_storage({read, "key"}), t::ErrorCode::PermissionDenied),
        "shared read gate");
  fixture.store.members[fixture.namespace_id][0].member.can_read = true;
  // Disabled namespace fails closed.
  fixture.store.namespaces[fixture.namespace_id].disabled = true;
  check(error(fixture.app->get_storage({read, "key"}), t::ErrorCode::Conflict),
        "disabled shared namespace");
  fixture.store.namespaces[fixture.namespace_id].disabled = false;
  // Unresolved shared namespace -> NOT_FOUND.
  fixture.store.namespaces.clear();
  check(error(fixture.app->get_storage({read, "key"}), t::ErrorCode::NotFound),
        "missing shared namespace");
  // Explicit installation_shared_id must match the resolved namespace.
  fixture.shared_namespace();
  auto mismatch_scope = read;
  mismatch_scope.requested = t::RequestedScopeKind::InstallationSharedId;
  mismatch_scope.shared_namespace_id = t::ResourceId{uuid(99)};
  check(error(fixture.app->get_storage({mismatch_scope, "key"}), t::ErrorCode::PermissionDenied),
        "installation_shared_id mismatch denied");
  // Dependency failure never becomes absence.
  fixture.store.fail_reads = true;
  check(error(fixture.app->get_storage({read, "key"}), t::ErrorCode::UpstreamUnavailable),
        "read failure not absence");
  fixture.store.fail_reads = false;
}

void management_tests() {
  Fixture fixture;
  auto create = t::CreateSharedNamespaceRequest{
      fixture.management_context(), t::InstallationId{fixture.installation}, std::nullopt};
  auto created = fixture.app->create_shared_namespace(create);
  check(std::holds_alternative<t::NamespaceMutation>(created), "create namespace");
  const auto& mutation = std::get<t::NamespaceMutation>(created);
  check(mutation.namespace_record.version.value == 1 && mutation.operation.has_value(),
        "create v1 + operation");
  check(fixture.authorizer.last_permission == t::Permission::NamespaceManage, "create permission");
  const auto namespace_id = mutation.namespace_record.namespace_id;
  auto duplicate = create;
  duplicate.context.idempotency_key = "9999999999999999";
  check(error(fixture.app->create_shared_namespace(duplicate), t::ErrorCode::Conflict),
        "second namespace per installation conflicts");
  auto replay = fixture.app->create_shared_namespace(create);
  check(std::holds_alternative<t::NamespaceMutation>(replay), "create idempotent replay");
  auto get = t::GetSharedNamespaceQuery{
      fixture.management_context("GetSharedNamespace", "0000000000000000"), namespace_id};
  auto view = fixture.app->get_shared_namespace(get);
  check(std::holds_alternative<t::NamespaceView>(view), "get namespace");
  check(fixture.authorizer.last_permission == t::Permission::NamespaceRead, "get permission");
  // Members require the exact namespace/installation pair.
  auto members =
      t::SetSharedMembersRequest{fixture.management_context("SetSharedMembers", "abcdefabcdefabcd"),
                                 namespace_id,
                                 t::InstallationId{fixture.installation},
                                 {{t::InstanceId{fixture.instance}, true, true}},
                                 std::nullopt};
  auto set_members = fixture.app->set_shared_members(members);
  check(std::holds_alternative<t::SharedMembersMutation>(set_members), "set members");
  check(std::get<t::SharedMembersMutation>(set_members).version.value == 2, "members v2");
  auto stale = members;
  stale.context.idempotency_key = "abcdefabcdefabce";
  stale.expected_version = t::ResourceVersion{1};
  check(error(fixture.app->set_shared_members(stale), t::ErrorCode::Conflict),
        "stale expected members version");
  auto disable = t::DisableSharedNamespaceRequest{
      fixture.management_context("DisableSharedNamespace", "bacdefabcdefabcd"), namespace_id,
      std::nullopt};
  auto disabled = fixture.app->disable_shared_namespace(disable);
  check(std::holds_alternative<t::SharedMembersMutation>(disabled), "disable namespace");
  check(std::get<t::SharedMembersMutation>(disabled).version.value == 3, "disable v3");
  check(fixture.store.namespaces[namespace_id.value].disabled, "namespace disabled");
  auto again = disable;
  again.context.idempotency_key = "bacdefabcdefabce";
  check(error(fixture.app->disable_shared_namespace(again), t::ErrorCode::Conflict),
        "double disable conflicts");
  fixture.authorizer.allow = false;
  check(error(fixture.app->create_shared_namespace(create), t::ErrorCode::PermissionDenied),
        "management denial");
  fixture.authorizer.allow = true;
  // Unknown commit must not be reported as success.
  Fixture unknown_fixture;
  unknown_fixture.store.unknown_commit = true;
  auto unknown = unknown_fixture.app->create_shared_namespace(t::CreateSharedNamespaceRequest{
      unknown_fixture.management_context(), t::InstallationId{unknown_fixture.installation},
      std::nullopt});
  check(error(unknown, t::ErrorCode::Internal), "unknown commit not success");
}

int main() {
  storage_tests();
  shared_tests();
  management_tests();
  std::cout << "P09a storage/namespace application passed\n";
  return 0;
}
