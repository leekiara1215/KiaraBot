#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kiarabot/core/application/target/runtime_bridge_application.hpp"
#include "kiarabot/core/application/target/runtime_lifecycle_participants.hpp"
#include "kiarabot/core/application/target/runtime_readiness.hpp"
#include "kiarabot/core/application/target/runtime_resource_application.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"

namespace {
namespace t = kiarabot::core::target;
using std::chrono_literals::operator""s;
constexpr auto kAccount = "11111111-1111-4111-8111-111111111111";
constexpr auto kAdapter = "22222222-2222-4222-8222-222222222222";
constexpr auto kHost = "55555555-5555-4555-8555-555555555555";
constexpr auto kSubject = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr auto kPeer = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::fprintf(stderr, "%s\n", scenario);
    std::exit(1);
  }
}
t::Failure failure(t::ErrorCode code) {
  return {code, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
template <typename Value> bool failure_is(const t::Outcome<Value>& status, t::ErrorCode code) {
  const auto* value = std::get_if<t::Failure>(&status);
  return value != nullptr && value->code == code;
}
std::string id_text(std::uint64_t value, std::uint64_t high) {
  std::array<char, 40> buffer{};
  std::snprintf(
      buffer.data(), buffer.size(), "%08llx-%04llx-4%03llx-8%03llx-%012llx",
      static_cast<unsigned long long>(value >> 32), static_cast<unsigned long long>(high & 0xffffu),
      static_cast<unsigned long long>(high & 0xfffu),
      static_cast<unsigned long long>(high & 0xfffu), static_cast<unsigned long long>(value));
  return buffer.data();
}

struct Store {
  t::TimePoint now{std::chrono::seconds{1000}};
  std::map<std::string, t::BotAccount> accounts;
  std::map<std::string, t::AdapterBindingRecord> bindings;
  std::map<std::string, t::AdapterRecord> adapters;
  std::map<std::string, t::HostRegistration> hosts;
  std::map<std::string, t::PluginInstallation> installations;
  std::map<std::string, t::PluginInstance> instances;
  std::map<std::string, t::InstanceConfiguration> configs;
  std::map<std::string, t::Operation> operations;
  std::map<std::string, std::vector<t::OperationParticipant>> operation_participants;
  std::map<std::pair<int, std::string>, t::LifecycleTombstone> tombstones;
  std::map<std::string, t::HostCommandOutboxRecord> host_commands;
  std::vector<t::BridgeCredentialRecord> bridge_credentials;
  std::vector<std::pair<t::ReceiptKey, t::CommandReceipt>> receipts;
  std::vector<t::AuditEntry> audits;
  bool fail_reads = false;
  bool fail_writes = false;
  bool fail_commit = false;
  bool unknown_commit = false;
  int commits = 0;
  int rollbacks = 0;
};
std::optional<t::AdapterRecord> find_adapter_by_instance(Store& store,
                                                         const std::string& instance) {
  for (const auto& [key, value] : store.adapters) {
    (void)key;
    if (value.current.instance.value == instance) {
      return value;
    }
  }
  return std::nullopt;
}
template <typename Value> std::optional<Value> lookup(Store& store, const std::string& key);
template <> std::optional<t::BotAccount> lookup(Store& store, const std::string& key) {
  const auto found = store.accounts.find(key);
  return found == store.accounts.end() ? std::nullopt : std::optional<t::BotAccount>{found->second};
}
template <> std::optional<t::AdapterBindingRecord> lookup(Store& store, const std::string& key) {
  const auto found = store.bindings.find(key);
  return found == store.bindings.end() ? std::nullopt
                                       : std::optional<t::AdapterBindingRecord>{found->second};
}
template <> std::optional<t::HostRegistration> lookup(Store& store, const std::string& key) {
  const auto found = store.hosts.find(key);
  return found == store.hosts.end() ? std::nullopt
                                    : std::optional<t::HostRegistration>{found->second};
}
template <> std::optional<t::PluginInstallation> lookup(Store& store, const std::string& key) {
  const auto found = store.installations.find(key);
  return found == store.installations.end() ? std::nullopt
                                            : std::optional<t::PluginInstallation>{found->second};
}
template <> std::optional<t::PluginInstance> lookup(Store& store, const std::string& key) {
  const auto found = store.instances.find(key);
  return found == store.instances.end() ? std::nullopt
                                        : std::optional<t::PluginInstance>{found->second};
}
template <> std::optional<t::InstanceConfiguration> lookup(Store& store, const std::string& key) {
  const auto found = store.configs.find(key);
  return found == store.configs.end() ? std::nullopt
                                      : std::optional<t::InstanceConfiguration>{found->second};
}
template <> std::optional<t::Operation> lookup(Store& store, const std::string& key) {
  const auto found = store.operations.find(key);
  return found == store.operations.end() ? std::nullopt
                                         : std::optional<t::Operation>{found->second};
}
template <typename Value> std::map<std::string, Value>& table(Store& store);
template <> std::map<std::string, t::BotAccount>& table(Store& store) {
  return store.accounts;
}
template <> std::map<std::string, t::AdapterBindingRecord>& table(Store& store) {
  return store.bindings;
}
template <> std::map<std::string, t::HostRegistration>& table(Store& store) {
  return store.hosts;
}
template <> std::map<std::string, t::PluginInstallation>& table(Store& store) {
  return store.installations;
}
template <> std::map<std::string, t::PluginInstance>& table(Store& store) {
  return store.instances;
}
template <> std::map<std::string, t::InstanceConfiguration>& table(Store& store) {
  return store.configs;
}
template <> std::map<std::string, t::Operation>& table(Store& store) {
  return store.operations;
}
template <typename Value> std::string record_key(const Value& value);
template <> std::string record_key(const t::BotAccount& value) {
  return value.id.value;
}
template <> std::string record_key(const t::AdapterBindingRecord& value) {
  return value.account.value;
}
template <> std::string record_key(const t::HostRegistration& value) {
  return value.host.value;
}
template <> std::string record_key(const t::PluginInstallation& value) {
  return value.id.value;
}
template <> std::string record_key(const t::PluginInstance& value) {
  return value.id.value;
}
template <> std::string record_key(const t::InstanceConfiguration& value) {
  return value.instance.value;
}
template <> std::string record_key(const t::Operation& value) {
  return value.ref.id.value;
}
template <typename Value> std::uint64_t version_of(const Value& value) {
  return value.version.value;
}
template <typename Key, typename Value>
class MemoryRepository final : public t::Repository<Key, Value> {
public:
  explicit MemoryRepository(Store& store) : store_(store) {}
  t::Outcome<std::optional<Value>> find(const Key& key) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return lookup<Value>(store_, key.value);
  }
  t::Status save(const Value& value, std::optional<t::ResourceVersion> expected) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    auto& rows = table<Value>(store_);
    const auto key = record_key(value);
    const auto found = rows.find(key);
    if (!expected) {
      if (found != rows.end()) {
        return failure(t::ErrorCode::Conflict);
      }
      rows.emplace(key, value);
      return std::monostate{};
    }
    if (found == rows.end() || found->second.version.value != expected->value ||
        value.version.value != expected->value + 1) {
      return failure(t::ErrorCode::Conflict);
    }
    rows[key] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};
template <typename Value> class MemoryList {
public:
  template <typename Map> static t::Outcome<std::vector<Value>> list(const Map& rows) {
    std::vector<Value> result;
    for (const auto& [key, value] : rows) {
      (void)key;
      result.push_back(value);
    }
    return result;
  }
};
class MemoryAdapters final : public t::AdapterRegistryRepository {
public:
  explicit MemoryAdapters(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::AdapterRecord>> find(const t::ResourceId& adapter) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    const auto found = store_.adapters.find(adapter.value);
    return found == store_.adapters.end() ? std::nullopt
                                          : std::optional<t::AdapterRecord>{found->second};
  }
  t::Outcome<std::optional<t::AdapterRecord>> by_instance(const t::InstanceId& instance) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return find_adapter_by_instance(store_, instance.value);
  }
  t::Status save(const t::AdapterRecord& value,
                 std::optional<t::ResourceVersion> expected) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    const auto found = store_.adapters.find(value.adapter.value);
    if (!expected) {
      if (found != store_.adapters.end()) {
        return failure(t::ErrorCode::Conflict);
      }
      store_.adapters.emplace(value.adapter.value, value);
      return std::monostate{};
    }
    if (found == store_.adapters.end() || found->second.version.value != expected->value ||
        value.version.value != expected->value + 1) {
      return failure(t::ErrorCode::Conflict);
    }
    store_.adapters[value.adapter.value] = value;
    return std::monostate{};
  }

private:
  Store& store_;
};
class MemoryReceipts final : public t::ReceiptRepository {
public:
  explicit MemoryReceipts(Store& store) : store_(store) {}
  t::Outcome<std::optional<t::CommandReceipt>> find(const t::ReceiptKey& key) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& [stored_key, receipt] : store_.receipts) {
      if (stored_key == key) {
        return std::optional<t::CommandReceipt>{receipt};
      }
    }
    return std::optional<t::CommandReceipt>{};
  }
  t::Outcome<t::ReceiptWrite> insert(const t::CommandReceipt& receipt,
                                     t::ReceiptCapacity) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& [key, stored] : store_.receipts) {
      if (key == receipt.key) {
        if (stored.fingerprint != receipt.fingerprint) {
          return failure(t::ErrorCode::Conflict);
        }
        return t::ReceiptWrite{stored, true};
      }
    }
    store_.receipts.emplace_back(receipt.key, receipt);
    return t::ReceiptWrite{receipt, false};
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class MemoryJournal final : public t::JournalRepository {
public:
  explicit MemoryJournal(Store& store) : store_(store) {}
  t::Status append_audit(const t::AuditEntry& entry) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    store_.audits.push_back(entry);
    return std::monostate{};
  }
  t::Status enqueue(const t::OutboxEntry&, t::JournalCapacity, t::TimePoint) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::vector<t::OutboxClaim>> claim(t::Service, std::size_t, t::TimePoint,
                                                std::chrono::seconds) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Status confirm(const t::HandoffKey&, const t::ClaimToken&, t::TimePoint) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::optional<t::InboxRecord>> received(const t::HandoffKey&) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<t::InboxWrite> record_received(const t::InboxRecord&, t::JournalCapacity,
                                            t::TimePoint) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::size_t> prune_received(t::TimePoint, std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class MemoryCredentials final : public t::BridgeCredentialRepository {
public:
  explicit MemoryCredentials(Store& store) : store_(store) {}
  t::Outcome<bool> active(const std::string& resource_id) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& record : store_.bridge_credentials) {
      if (record.resource_id == resource_id && !record.revoked) {
        return true;
      }
    }
    return false;
  }
  t::Outcome<std::optional<t::BridgeCredentialRecord>>
  by_digest(const t::Sha256Digest& digest) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& record : store_.bridge_credentials) {
      if (!record.revoked && record.digest == digest) {
        return std::optional<t::BridgeCredentialRecord>{record};
      }
    }
    return std::optional<t::BridgeCredentialRecord>{};
  }
  t::Status insert(const t::BridgeCredentialRecord& record) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (const auto& existing : store_.bridge_credentials) {
      if (existing.resource_id == record.resource_id && !existing.revoked) {
        return failure(t::ErrorCode::Conflict);
      }
    }
    store_.bridge_credentials.push_back(record);
    return std::monostate{};
  }
  t::Status revoke(const std::string& resource_id, t::TimePoint) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    for (auto& record : store_.bridge_credentials) {
      if (record.resource_id == resource_id) {
        record.revoked = true;
      }
    }
    return std::monostate{};
  }

private:
  Store& store_;
};
// P05 runtime resource tests do not exercise event delivery; the merged UoW contract still
// requires an EventRepository. Every call fails closed so an accidental dependency is visible.
class UnusedEvents final : public t::EventRepository {
public:
  t::Outcome<t::EventAcceptanceResult> accept(const t::EventRecord&,
                                              const std::vector<t::DeliveryRecord>&,
                                              const t::EventCapacityLimits&) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::vector<t::DeliveryClaim>> claim(const t::DeliveryClaimRequest&) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<t::DeliveryReceipt> acknowledge(const t::DeliveryAcknowledgement&,
                                             t::TimePoint) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<t::DeliveryReceipt> renew(const t::DeliveryAttempt&, t::TimePoint) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<t::DeliverySweep> sweep(t::TimePoint, const t::RetryPolicy&, double) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::size_t> recover(t::TimePoint, const t::RetryPolicy&, double) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::vector<t::DeadLetter>> list_dead_letters(const std::optional<t::InstanceId>&,
                                                           std::size_t) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::optional<t::DeliveryRecord>> replay(const t::ReplayRequest&) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<t::EventCapacity> capacity(const t::EventCapacityLimits&) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
  t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
    return failure(t::ErrorCode::CapabilityNotSupported);
  }
};
// P07 Action ports are not exercised by this P05/P06 application fixture. They stay fail-closed
// exactly like a missing PG relation so an accidental call is visible instead of looking absent.
class MemoryActionPorts final : public t::ActionRepository,
                                public t::ActionQuotaRepository,
                                public t::ActionPolicyRepository,
                                public t::ActionEvidenceRepository {
public:
  t::Outcome<std::optional<t::ActionRecord>> find_by_id(const t::ActionId&) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::optional<t::ActionRecord>> find_by_owner_key(const t::InstanceId&,
                                                               std::string_view) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<t::ActionRecord> insert(const t::ActionRecord&, t::ActionCapacity) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Status mark_dispatched(const t::ActionId&, const t::AttemptId&, const t::InstanceId&,
                            t::TimePoint) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<t::ActionTerminalReceipt> record_terminal(const t::ActionRecord&,
                                                       const t::ActionTerminalResult&,
                                                       std::optional<t::AttemptId>,
                                                       std::optional<t::InstanceId>,
                                                       t::TimePoint) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Status cancel(const t::ActionId&, t::TimePoint) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::vector<t::ActionRecord>> expire_due(std::size_t, t::TimePoint) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::vector<t::ActionRecord>> list_owner(const t::InstanceId&,
                                                      const t::ActionPageRequest&) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::vector<t::ActionRecord>> list_control(const t::ActionControlQuery&) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::vector<t::ActionRecord>> list_due(const t::ResourceId&, std::size_t,
                                                    t::TimePoint) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::size_t> prune_terminal(t::TimePoint, std::size_t) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Status admit(const t::ActionId&, t::ActionKind, std::span<const t::ActionScope>,
                  const t::ActionQuotaLimits&, t::TimePoint) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::size_t> prune_windows(t::TimePoint, std::size_t) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::optional<t::ActionPolicy>> get(const t::ResourceId&) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Status upsert(const t::ActionPolicy&, std::optional<t::ResourceVersion>) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Status append(const t::ActionEvidenceRecord&) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::vector<t::ActionEvidenceRecord>> list(const t::ActionId&) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
  t::Outcome<std::size_t> prune(t::TimePoint, std::size_t) override {
    return failure(t::ErrorCode::UpstreamUnavailable);
  }
};
// P08b-3 outbox fake: only the enqueue path is exercised by P05 application tests. Delivery
// methods stay unsupported so a hidden transport dependency is visible, never fake-successful.
class MemoryHostCommands final : public t::HostCommandOutbox {
public:
  explicit MemoryHostCommands(Store& store) : store_(store) {}
  t::Outcome<t::PersistedHostCommand> insert(const t::HostCommandOutboxRecord& record) override {
    auto found = store_.host_commands.find(record.command_id);
    if (found != store_.host_commands.end()) {
      if (found->second.payload_sha256 != record.payload_sha256) {
        return failure(t::ErrorCode::Conflict);
      }
      return t::PersistedHostCommand{found->second, true};
    }
    store_.host_commands.emplace(record.command_id, record);
    return t::PersistedHostCommand{record, false};
  }
  t::Outcome<std::vector<t::HostCommandOutboxRecord>> pending(const std::string& host_id,
                                                              std::uint64_t, std::size_t) override {
    std::vector<t::HostCommandOutboxRecord> result;
    for (const auto& [key, value] : store_.host_commands) {
      (void)key;
      if (value.host_id == host_id && value.state == t::HostCommandDeliveryState::Pending) {
        result.push_back(value);
      }
    }
    return result;
  }
  t::Outcome<std::optional<t::HostCommandOutboxRecord>>
  find(const std::string& command_id) override {
    const auto found = store_.host_commands.find(command_id);
    return found == store_.host_commands.end()
               ? std::optional<t::HostCommandOutboxRecord>{}
               : std::optional<t::HostCommandOutboxRecord>{found->second};
  }
  t::Status mark_dispatched(const std::string& command_id, std::uint64_t, std::string_view,
                            t::TimePoint) override {
    const auto found = store_.host_commands.find(command_id);
    if (found == store_.host_commands.end()) {
      return failure(t::ErrorCode::NotFound);
    }
    found->second.state = t::HostCommandDeliveryState::Dispatched;
    return std::monostate{};
  }
  t::Status record_receipt(const std::string& command_id, bool persisted,
                           std::optional<t::ExecutionOutcome> outcome,
                           std::optional<std::string> marker, t::TimePoint) override {
    const auto found = store_.host_commands.find(command_id);
    if (found == store_.host_commands.end()) {
      return failure(t::ErrorCode::NotFound);
    }
    found->second.state = t::HostCommandDeliveryState::Receipted;
    found->second.receipt_persisted = persisted;
    found->second.receipt_outcome = outcome;
    found->second.receipt_process_marker = std::move(marker);
    return std::monostate{};
  }
  t::Status mark_unknown(const std::string& command_id, t::TimePoint) override {
    const auto found = store_.host_commands.find(command_id);
    if (found == store_.host_commands.end()) {
      return failure(t::ErrorCode::NotFound);
    }
    found->second.state = t::HostCommandDeliveryState::Unknown;
    return std::monostate{};
  }
  t::Outcome<bool> record_observation(const t::HostObservationRecord&) override {
    return false;
  }
  t::Outcome<std::vector<t::HostObservationRecord>> observations(const std::string&, std::uint64_t,
                                                                 std::size_t) override {
    return std::vector<t::HostObservationRecord>{};
  }
  t::Outcome<std::size_t> expire_dispatched(t::TimePoint, std::chrono::seconds) override {
    return std::size_t{0};
  }
  t::Outcome<std::size_t> prune(t::TimePoint, std::chrono::seconds, std::chrono::seconds,
                                std::size_t) override {
    return std::size_t{0};
  }

private:
  Store& store_;
};
class MemoryWork final : public t::RuntimeUnitOfWork {
public:
  explicit MemoryWork(Store& store)
      : store_(store), accounts_(store), bindings_(store), adapters_(store), hosts_(store),
        installations_(store), instances_(store), configs_(store), operations_(store),
        credentials_(store), receipts_(store), journal_(store) {}
  t::Service owner() const noexcept override {
    return t::Service::Runtime;
  }
  t::Outcome<t::TimePoint> current_time() override {
    return store_.now;
  }
  t::HostCommandOutbox& host_commands() override {
    return host_commands_;
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
    if (store_.unknown_commit) {
      return t::CommitResult::Unknown;
    }
    return t::CommitResult::Committed;
  }
  void rollback() noexcept override {
    ++store_.rollbacks;
  }
  t::Repository<t::ResourceId, t::BotAccount>& accounts() override {
    return accounts_;
  }
  t::Repository<t::ResourceId, t::AdapterBindingRecord>& adapter_bindings() override {
    return bindings_;
  }
  t::AdapterRegistryRepository& adapters() override {
    return adapters_;
  }
  t::BridgeCredentialRepository& bridge_credentials() override {
    return credentials_;
  }
  t::ActionRepository& actions() override {
    return memory_actions_;
  }
  t::ActionQuotaRepository& action_quota() override {
    return memory_actions_;
  }
  t::ActionPolicyRepository& action_policies() override {
    return memory_actions_;
  }
  t::ActionEvidenceRepository& action_evidence() override {
    return memory_actions_;
  }
  t::Outcome<std::size_t> active_bindings(const t::InstanceId& adapter_instance) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::size_t count = 0;
    for (const auto& [key, binding] : store_.bindings) {
      (void)key;
      if (binding.adapter_instance == adapter_instance &&
          binding.state == t::LifecycleState::Active) {
        ++count;
      }
    }
    return count;
  }
  t::Repository<t::ResourceId, t::HostRegistration>& hosts() override {
    return hosts_;
  }
  t::Repository<t::InstallationId, t::PluginInstallation>& installations() override {
    return installations_;
  }
  t::Repository<t::InstanceId, t::PluginInstance>& instances() override {
    return instances_;
  }
  t::Repository<t::InstanceId, t::InstanceConfiguration>& instance_configs() override {
    return configs_;
  }
  t::EventRepository& events() override {
    return events_;
  }
  t::Outcome<std::vector<t::BotAccount>> list_accounts(std::size_t limit,
                                                       const std::string& after) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::BotAccount> result;
    for (const auto& [key, value] : store_.accounts) {
      if (key > after) {
        result.push_back(value);
      }
      if (result.size() > limit) {
        break;
      }
    }
    return result;
  }
  t::Outcome<std::vector<t::AdapterRecord>> list_adapters(std::size_t limit,
                                                          const std::string& after) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::AdapterRecord> result;
    for (const auto& [key, value] : store_.adapters) {
      if (key > after) {
        result.push_back(value);
      }
      if (result.size() > limit) {
        break;
      }
    }
    return result;
  }
  t::Outcome<std::vector<t::HostRegistration>> list_hosts(std::size_t limit,
                                                          const std::string& after) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::HostRegistration> result;
    for (const auto& [key, value] : store_.hosts) {
      if (key > after) {
        result.push_back(value);
      }
      if (result.size() > limit) {
        break;
      }
    }
    return result;
  }
  t::Outcome<std::vector<t::PluginInstallation>>
  list_installations(std::size_t limit, const std::string& after) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::PluginInstallation> result;
    for (const auto& [key, value] : store_.installations) {
      if (key > after) {
        result.push_back(value);
      }
      if (result.size() > limit) {
        break;
      }
    }
    return result;
  }
  t::Outcome<std::vector<t::PluginInstance>> list_instances(std::size_t limit,
                                                            const std::string& after) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    std::vector<t::PluginInstance> result;
    for (const auto& [key, value] : store_.instances) {
      if (key > after) {
        result.push_back(value);
      }
      if (result.size() > limit) {
        break;
      }
    }
    return result;
  }

  t::Outcome<std::vector<t::OperationParticipant>>
  operation_participants(const t::OperationId& id) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return store_.operation_participants[id.value];
  }
  t::Status
  save_operation_participants(const t::OperationId& id,
                              const std::vector<t::OperationParticipant>& participants) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    store_.operation_participants[id.value] = participants;
    return std::monostate{};
  }
  t::Outcome<std::size_t> prune_outbox(t::TimePoint, std::size_t) override {
    return std::size_t{1};
  }
  t::Outcome<std::size_t> prune_audit(t::TimePoint, std::size_t) override {
    return std::size_t{1};
  }
  t::Outcome<std::size_t> prune_operations(t::TimePoint, std::size_t) override {
    return std::size_t{1};
  }
  t::Outcome<std::optional<t::LifecycleTombstone>>
  find_lifecycle_tombstone(const t::ResourceRef& resource) override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    const auto found = store_.tombstones.find({static_cast<int>(resource.kind), resource.id.value});
    return found == store_.tombstones.end() ? std::optional<t::LifecycleTombstone>{}
                                            : std::optional<t::LifecycleTombstone>{found->second};
  }
  t::Status save_lifecycle_tombstone(const t::LifecycleTombstone& value) override {
    if (store_.fail_writes) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    store_.tombstones[{static_cast<int>(value.resource.kind), value.resource.id.value}] = value;
    return std::monostate{};
  }
  t::Status delete_lifecycle_tombstone(const t::ResourceRef& resource) override {
    store_.tombstones.erase({static_cast<int>(resource.kind), resource.id.value});
    return std::monostate{};
  }
  t::Outcome<std::vector<t::LifecycleTombstone>>
  list_lifecycle_tombstones(std::size_t limit, const std::string& after) override {
    std::vector<t::LifecycleTombstone> result;
    for (const auto& [key, value] : store_.tombstones) {
      if (value.resource.id.value > after) {
        result.push_back(value);
      }
      if (result.size() >= limit) {
        break;
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
      return left.resource.id.value < right.resource.id.value;
    });
    return result;
  }
  t::StorageReceiptRepository& storage_receipts() override {
    return storage_receipts_;
  }
  class MemoryStorageReceipts final : public t::StorageReceiptRepository {
  public:
    t::Outcome<std::optional<t::NamespaceReceipt>> find(const t::ReceiptKey&) override {
      return std::optional<t::NamespaceReceipt>{};
    }
    t::Outcome<t::NamespaceReceiptWrite> insert(const t::NamespaceReceipt&,
                                                t::NamespaceReceiptCapacity) override {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    t::Outcome<std::size_t> prune_expired(t::TimePoint, std::size_t) override {
      return std::size_t{0};
    }
  };

private:
  Store& store_;
  MemoryRepository<t::ResourceId, t::BotAccount> accounts_;
  MemoryRepository<t::ResourceId, t::AdapterBindingRecord> bindings_;
  MemoryAdapters adapters_;
  MemoryRepository<t::ResourceId, t::HostRegistration> hosts_;
  MemoryRepository<t::InstallationId, t::PluginInstallation> installations_;
  MemoryRepository<t::InstanceId, t::PluginInstance> instances_;
  MemoryRepository<t::InstanceId, t::InstanceConfiguration> configs_;
  UnusedEvents events_;
  MemoryRepository<t::OperationId, t::Operation> operations_;
  MemoryCredentials credentials_;
  MemoryReceipts receipts_;
  MemoryJournal journal_;
  MemoryHostCommands host_commands_{store_};
  MemoryActionPorts memory_actions_;
  MemoryStorageReceipts storage_receipts_;
};
class MemoryFactory final : public t::RuntimeUnitOfWorkFactory {
public:
  explicit MemoryFactory(Store& store) : store_(store) {}
  t::Outcome<std::unique_ptr<t::RuntimeUnitOfWork>> begin() override {
    if (store_.fail_reads) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    return std::unique_ptr<t::RuntimeUnitOfWork>(std::make_unique<MemoryWork>(store_));
  }

private:
  Store& store_;
};
class Ids final : public t::RuntimeIdPort {
public:
  t::Outcome<t::ResourceId> account_id() override {
    return t::ResourceId{next()};
  }
  t::Outcome<t::InstallationId> installation_id() override {
    return t::InstallationId{next()};
  }
  t::Outcome<t::InstanceId> instance_id() override {
    return t::InstanceId{next()};
  }
  t::Outcome<t::InstanceId> adapter_instance_id() override {
    return t::InstanceId{next()};
  }
  t::Outcome<t::ConnectionId> connection_id() override {
    return t::ConnectionId{next()};
  }
  t::Outcome<t::OperationId> operation_id() override {
    return t::OperationId{next()};
  }
  t::Outcome<t::RuntimeEpoch> runtime_epoch() override {
    return t::RuntimeEpoch{"epoch-1"};
  }
  t::Outcome<std::string> snapshot_id() override {
    return next();
  }

private:
  std::string next() {
    return id_text(++counter_, 0x9000);
  }
  std::uint64_t counter_ = 0;
};
class Secrets final : public t::RuntimeSecretPort {
public:
  t::Outcome<std::string> bootstrap_credential() override {
    char value[65] = {};
    std::snprintf(value, sizeof(value), "%064llx", static_cast<unsigned long long>(++counter_));
    return std::string{value};
  }
  t::Outcome<std::array<std::byte, 32>> opaque_secret() override {
    std::array<std::byte, 32> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::byte>((counter_ + index) & 0xffU);
    }
    ++counter_;
    return bytes;
  }
  t::Outcome<t::Sha256Digest> sha256(std::string_view value) override {
    t::Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
      digest.bytes[index] = static_cast<std::byte>(
          ~(index < value.size() ? static_cast<unsigned char>(value[index]) : 0U));
    }
    return digest;
  }
  std::uint64_t counter_ = 0;
};
class Authorization final : public t::RuntimeAuthorizationPort {
public:
  bool allowed = true;
  bool deny = false;
  bool unavailable = false;
  bool chain_available = true;
  bool invalid_binding = false;
  bool stale_decision = false;
  std::set<std::string> denied_resources;
  bool available() const noexcept override {
    return chain_available && !unavailable;
  }
  t::Outcome<t::RuntimeAuthorizationDecision>
  check(const t::RuntimeAuthorizationRequest& request) override {
    if (unavailable) {
      return failure(t::ErrorCode::UpstreamUnavailable);
    }
    const bool denied_resource =
        !request.scope.empty() && denied_resources.count(request.scope.front().id.value) > 0;
    t::RuntimeAuthorizationDecision decision;
    decision.allowed = allowed && !deny && !denied_resource;
    decision.subject = request.authentication.subject;
    decision.permission = invalid_binding ? t::Permission::Unspecified : request.permission;
    decision.scope = request.scope;
    decision.authorization_version = {7};
    decision.checked_at = stale_decision
                              ? t::TimePoint{std::chrono::seconds{1}}
                              : t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())};
    if (!decision.allowed) {
      decision.denial = failure(t::ErrorCode::PermissionDenied);
    }
    return decision;
  }
};
class LifecycleHost final : public t::HostLifecycleParticipantPort {
public:
  int calls = 0;
  bool done = true;
  bool unknown = false;
  t::LifecycleParticipantResult stop_instance(const t::ResourceRef&) override {
    ++calls;
    if (unknown) {
      return {t::LifecycleParticipantDisposition::Unknown, std::nullopt, std::nullopt};
    }
    if (!done) {
      return {t::LifecycleParticipantDisposition::Blocked, t::ErrorCode::UpstreamUnavailable,
              std::nullopt};
    }
    return {t::LifecycleParticipantDisposition::Done, std::nullopt, std::nullopt};
  }
};
class LifecycleScheduler final : public t::SchedulerLifecycleParticipantPort {
public:
  int calls = 0;
  bool done = true;
  t::LifecycleParticipantResult cancel_tasks(const t::ResourceRef&) override {
    ++calls;
    if (!done) {
      return {t::LifecycleParticipantDisposition::Blocked, t::ErrorCode::UpstreamUnavailable,
              std::nullopt};
    }
    return {t::LifecycleParticipantDisposition::Done, std::nullopt, std::nullopt};
  }
};
t::AuthenticationContext authentication() {
  return {t::SubjectId{kSubject},
          t::PlatformPeer{t::Service::Control, t::SubjectId{kPeer}},
          t::Service::Runtime,
          {},
          {},
          {7},
          t::TimePoint{std::chrono::seconds{900}},
          t::TimePoint{std::chrono::hours{1}}};
}
t::AuthenticationContext adapter_authentication(const std::string& adapter) {
  return {t::SubjectId{kSubject},
          t::AdapterPeer{t::ResourceId{adapter}, t::SubjectId{kSubject}},
          t::Service::Runtime,
          {},
          {},
          {7},
          t::TimePoint{std::chrono::seconds{900}},
          t::TimePoint{std::chrono::hours{1}}};
}
t::CommandContext context(std::string method, t::Sha256Digest fingerprint) {
  static std::uint64_t sequence = 0;
  ++sequence;
  char request[40] = {};
  std::snprintf(request, sizeof(request), "cccccccc-cccc-4ccc-8ccc-%012llx",
                static_cast<unsigned long long>(sequence));
  char trace[40] = {};
  std::snprintf(trace, sizeof(trace), "dddddddd-dddd-4ddd-8ddd-%012llx",
                static_cast<unsigned long long>(sequence));
  t::CommandContext result;
  result.authentication = authentication();
  result.request_id = request;
  result.trace_id = trace;
  result.method = std::move(method);
  result.idempotency_key =
      "idempotency-key-" + std::to_string(static_cast<int>(fingerprint.bytes[0]));
  result.fingerprint = fingerprint;
  result.deadline = t::TimePoint{std::chrono::seconds{1300}};
  result.expected_version = std::nullopt;
  return result;
}
t::Sha256Digest digest(std::uint8_t seed) {
  t::Sha256Digest value;
  value.bytes[0] = static_cast<std::byte>(seed);
  return value;
}
struct Harness {
  Store store;
  MemoryFactory factory{store};
  Ids ids;
  Secrets secrets;
  Authorization authorization;
  LifecycleHost host_lifecycle;
  LifecycleScheduler scheduler_lifecycle;
  std::unique_ptr<t::RuntimeResourceApplication> application;
  Harness() {
    auto built = t::make_runtime_resource_application({factory,
                                                       ids,
                                                       secrets,
                                                       authorization,
                                                       {"action.send_message.text.v1"},
                                                       &host_lifecycle,
                                                       &scheduler_lifecycle});
    check(std::holds_alternative<std::unique_ptr<t::RuntimeResourceApplication>>(built),
          "application construction failed");
    application = std::move(std::get<std::unique_ptr<t::RuntimeResourceApplication>>(built));
  }
};
void create_account(Harness& harness, t::BotAccount* output = nullptr) {
  t::CreateAccountRequest request{context("runtime.create_account", digest(1)), "qq", "10001",
                                  "Bot"};
  auto created = harness.application->create_account(request);
  check(std::holds_alternative<t::BotAccount>(created), "create account failed");
  if (output) {
    *output = std::get<t::BotAccount>(created);
  }
  // Same idempotency key and payload replays the original snapshot and produces no second account.
  auto replay = harness.application->create_account(request);
  check(std::holds_alternative<t::BotAccount>(replay) &&
            std::get<t::BotAccount>(replay).id.value == std::get<t::BotAccount>(created).id.value,
        "create account replay lost original snapshot");
  check(harness.store.accounts.size() == 1, "replay created a second account");
  check(harness.store.commits == 1, "replay committed a second effect");
  // Same key with a different payload must conflict.
  auto changed_request = request;
  changed_request.context.fingerprint = digest(2);
  check(failure_is(harness.application->create_account(changed_request), t::ErrorCode::Conflict),
        "same key different payload was accepted");
}
void account_flow(Harness& harness) {
  t::BotAccount account;
  create_account(harness, &account);
  t::UpdateAccountRequest update{context("runtime.update_account", digest(3)), account.id,
                                 "Renamed"};
  update.context.expected_version = t::ResourceVersion{1};
  auto updated = harness.application->update_account(update);
  check(std::holds_alternative<t::AccountMutation>(updated) &&
            std::get<t::AccountMutation>(updated).account.version.value == 2,
        "update account version did not advance");
  auto stale = update;
  stale.context.idempotency_key = "idempotency-key-0002";
  stale.context.fingerprint = digest(4);
  check(failure_is(harness.application->update_account(stale), t::ErrorCode::Conflict),
        "stale expected version overwrote the account");
  // A missing expected version is rejected for updates.
  auto missing = update;
  missing.context.idempotency_key = "idempotency-key-0003";
  missing.context.fingerprint = digest(5);
  missing.context.expected_version = std::nullopt;
  check(failure_is(harness.application->update_account(missing), t::ErrorCode::InvalidArgument),
        "update accepted without expected version");
  // Reads never create a receipt.
  t::AccountQuery query{context("runtime.get_account", digest(6)), account.id};
  auto fetched = harness.application->get_account(query);
  check(std::holds_alternative<t::BotAccount>(fetched) &&
            std::get<t::BotAccount>(fetched).display_name == "Renamed",
        "get account lost update");
}
std::string ids_next(Harness& harness) {
  auto value = harness.ids.snapshot_id();
  return std::get<std::string>(value);
}
void adapter_flow(Harness& harness) {
  t::BotAccount account;
  create_account(harness, &account);
  t::AdapterRegistrationCommand registration;
  registration.authentication = adapter_authentication(kAdapter);
  registration.request_id = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
  registration.trace_id = "ffffffff-ffff-4fff-8fff-ffffffffffff";
  registration.deadline = t::TimePoint{std::chrono::seconds{1300}};
  registration.adapter = t::ResourceId{kAdapter};
  registration.metadata = {
      "nonebot", "1.0.0", "kiarabot.v1", {{"action.send_message.text.v1", "1"}}, {}};
  registration.declared_accounts = {account.id};
  auto registered = harness.application->register_adapter(registration);
  check(std::holds_alternative<t::AdapterRegistrationResult>(registered),
        "adapter registration failed");
  const auto& result = std::get<t::AdapterRegistrationResult>(registered);
  check(result.adapter.current.connection_generation.value == 1 &&
            !result.adapter.current.instance.value.empty() &&
            t::valid_adapter_fence(result.adapter.current),
        "adapter registration did not establish connection generation 1");
  check(result.adapter.current.runtime_epoch.value == "epoch-1",
        "adapter registration lost runtime epoch");
  // Declared but unknown account must reject registration without an effect.
  auto unknown = registration;
  unknown.adapter = t::ResourceId{ids_next(harness)};
  unknown.authentication = adapter_authentication(unknown.adapter.value);
  unknown.declared_accounts = {t::ResourceId{"99999999-9999-4999-8999-999999999999"}};
  check(failure_is(harness.application->register_adapter(unknown), t::ErrorCode::NotFound),
        "registration with unknown declared account accepted");
  // Reconnect must present the current instance; only the connection generation advances.
  auto reconnect = registration;
  reconnect.presented_instance = result.adapter.current.instance;
  auto accepted = harness.application->register_adapter(reconnect);
  check(std::holds_alternative<t::AdapterRegistrationResult>(accepted) &&
            std::get<t::AdapterRegistrationResult>(accepted)
                    .adapter.current.connection_generation.value == 2,
        "reconnect did not advance the connection generation");
  t::HeartbeatCommand heartbeat;
  heartbeat.authentication = adapter_authentication(kAdapter);
  heartbeat.request_id = "12121212-1212-4212-8212-121212121212";
  heartbeat.trace_id = "13131313-1313-4313-8313-131313131313";
  heartbeat.deadline = t::TimePoint{std::chrono::seconds{1300}};
  heartbeat.fence = t::AdapterFenceClaim{result.adapter.current.instance,
                                         t::RuntimeEpoch{"epoch-1"}, t::ConnectionGeneration{1}};
  check(failure_is(harness.application->heartbeat(heartbeat), t::ErrorCode::Conflict),
        "old connection generation heartbeat was accepted");
  heartbeat.fence.connection_generation = {2};
  check(std::holds_alternative<t::HeartbeatResult>(harness.application->heartbeat(heartbeat)),
        "current connection heartbeat rejected");
  heartbeat.fence.runtime_epoch = t::RuntimeEpoch{"epoch-0"};
  check(failure_is(harness.application->heartbeat(heartbeat), t::ErrorCode::Unauthenticated),
        "pre-restart epoch heartbeat was accepted");
  // Explicit management binding after registration.
  t::BindAdapterRequest bind{context("runtime.bind_adapter", digest(7)), account.id,
                             result.adapter.current.instance};
  bind.context.expected_version = std::nullopt;
  auto bound_result = harness.application->bind_adapter(bind);
  check(std::holds_alternative<t::BindingMutation>(bound_result),
        "bind adapter after registration failed");
  const auto& bound_mutation = std::get<t::BindingMutation>(bound_result);
  check(bound_mutation.bootstrap_credential.has_value() &&
            bound_mutation.bootstrap_credential->size() == 64,
        "bind did not issue a one-time external-bridge bootstrap credential");
  check(harness.store.bridge_credentials.size() == 1 &&
            !harness.store.bridge_credentials.front().revoked,
        "bind did not persist the active credential record");
  bool plaintext_stored = false;
  for (const auto& record : harness.store.bridge_credentials) {
    plaintext_stored = plaintext_stored ||
                       record.resource_id == *bound_mutation.bootstrap_credential ||
                       record.id == *bound_mutation.bootstrap_credential;
  }
  check(!plaintext_stored, "bootstrap plaintext was persisted instead of only its digest");
  auto duplicate = bind;
  duplicate.context.idempotency_key = "idempotency-key-0010";
  duplicate.context.fingerprint = digest(8);
  check(failure_is(harness.application->bind_adapter(duplicate), t::ErrorCode::Conflict),
        "duplicate bind accepted");
  // Disabling an account with an active binding conflicts.
  t::DisableAccountRequest disable{context("runtime.disable_account", digest(9)), account.id};
  disable.context.expected_version = harness.store.accounts[account.id.value].version;
  check(failure_is(harness.application->disable_account(disable), t::ErrorCode::Conflict),
        "disable with active binding accepted");
  // A disabled account can no longer be an adapter binding participant.
  harness.store.accounts[account.id.value].state = t::LifecycleState::Disabled;
  auto second = bind;
  second.context.idempotency_key = "idempotency-key-0011";
  second.context.fingerprint = digest(10);
  auto disabled_bind = harness.application->bind_adapter(second);
  check(std::holds_alternative<t::Failure>(disabled_bind), "bind accepted a disabled account");
  // Unbinding the last binding revokes the adapter bootstrap credential.
  harness.store.accounts[account.id.value].state = t::LifecycleState::Active;
  t::UnbindAdapterRequest unbind{context("runtime.unbind_adapter", digest(11)), account.id};
  unbind.context.expected_version = t::ResourceVersion{1};
  check(std::holds_alternative<t::BindingMutation>(harness.application->unbind_adapter(unbind)),
        "unbind failed");
  check(!harness.store.bridge_credentials.empty() &&
            harness.store.bridge_credentials.front().revoked,
        "unbind did not revoke the last adapter bootstrap credential");
}
void authorization_negative_flow() {
  Harness harness;
  t::BotAccount account;
  create_account(harness, &account);
  t::AdapterRegistrationCommand registration;
  registration.authentication = adapter_authentication(kAdapter);
  registration.request_id = "14141414-1414-4414-8414-141414141414";
  registration.trace_id = "15151515-1515-4515-8515-151515151515";
  registration.deadline = t::TimePoint{std::chrono::seconds{1300}};
  registration.adapter = t::ResourceId{kAdapter};
  registration.metadata = {
      "nonebot", "1.0.0", "kiarabot.v1", {{"action.send_message.text.v1", "1"}}, {}};
  registration.declared_accounts = {account.id};
  // Missing AdapterConnect authorization must not register anything.
  harness.authorization.deny = true;
  check(failure_is(harness.application->register_adapter(registration),
                   t::ErrorCode::PermissionDenied),
        "unauthorized adapter registration accepted");
  check(harness.store.adapters.empty(), "unauthorized registration produced an adapter");
  harness.authorization.deny = false;
  // A platform-service direct peer cannot register an adapter identity.
  auto forged = registration;
  forged.authentication = authentication();
  check(failure_is(harness.application->register_adapter(forged), t::ErrorCode::PermissionDenied),
        "platform peer registered an adapter identity");
  // Declared account count is bounded before any effect.
  auto oversized = registration;
  oversized.declared_accounts.clear();
  for (int index = 0; index < 65; ++index) {
    oversized.declared_accounts.push_back(
        t::ResourceId{id_text(static_cast<std::uint64_t>(index) + 1, 0x8000)});
  }
  check(failure_is(harness.application->register_adapter(oversized), t::ErrorCode::InvalidArgument),
        "oversized declared account list accepted");
  // Accepted registration then heartbeat with a forged platform peer must be denied.
  auto registered = harness.application->register_adapter(registration);
  check(std::holds_alternative<t::AdapterRegistrationResult>(registered),
        "authorized registration rejected");
  const auto& record = std::get<t::AdapterRegistrationResult>(registered);
  t::HeartbeatCommand heartbeat;
  heartbeat.authentication = authentication();
  heartbeat.request_id = "16161616-1616-4616-8616-161616161616";
  heartbeat.trace_id = "17171717-1717-4717-8717-171717171717";
  heartbeat.deadline = t::TimePoint{std::chrono::seconds{1300}};
  heartbeat.fence = t::AdapterFenceClaim{record.adapter.current.instance,
                                         t::RuntimeEpoch{"epoch-1"}, t::ConnectionGeneration{1}};
  check(failure_is(harness.application->heartbeat(heartbeat), t::ErrorCode::PermissionDenied),
        "platform peer heartbeat accepted for an adapter connection");
  // An invalid authorization decision binding (wrong permission) fails closed as an internal
  // upstream fault and performs no effect.
  harness.authorization.invalid_binding = true;
  t::CreateAccountRequest request{context("runtime.create_account", digest(41)), "qq", "10002",
                                  "Bot"};
  const auto before = harness.store.accounts.size();
  check(failure_is(harness.application->create_account(request), t::ErrorCode::Internal),
        "mismatched authorization decision was treated as a grant");
  check(harness.store.accounts.size() == before, "invalid decision produced an effect");
  harness.authorization.invalid_binding = false;
  harness.authorization.stale_decision = true;
  t::CreateAccountRequest stale{context("runtime.create_account", digest(43)), "qq", "10004",
                                "Bot"};
  const auto stale_before = harness.store.accounts.size();
  check(failure_is(harness.application->create_account(stale), t::ErrorCode::Internal),
        "stale authorization decision was treated as a current grant");
  check(harness.store.accounts.size() == stale_before, "stale decision produced an effect");
  harness.authorization.stale_decision = false;
  // Expired command deadlines are rejected before any effect.
  t::CreateAccountRequest expired{context("runtime.create_account", digest(42)), "qq", "10003",
                                  "Bot"};
  expired.context.deadline = t::TimePoint{std::chrono::seconds{900}};
  check(failure_is(harness.application->create_account(expired), t::ErrorCode::Timeout),
        "expired command deadline was accepted");
}
void mutation_replay_operation_flow() {
  Harness harness;
  t::BotAccount account;
  create_account(harness, &account);
  t::DisableAccountRequest disable{context("runtime.disable_account", digest(60)), account.id};
  disable.context.expected_version = t::ResourceVersion{1};
  auto first = harness.application->disable_account(disable);
  check(std::holds_alternative<t::AccountMutation>(first) &&
            std::get<t::AccountMutation>(first).operation.has_value(),
        "mutation did not return an accepted operation reference");
  const auto operation_id = std::get<t::AccountMutation>(first).operation->operation.id.value;
  auto replay = harness.application->disable_account(disable);
  check(std::holds_alternative<t::AccountMutation>(replay) &&
            std::get<t::AccountMutation>(replay).operation.has_value() &&
            std::get<t::AccountMutation>(replay).operation->operation.id.value == operation_id,
        "idempotent replay lost the original operation reference");
  check(harness.store.accounts[account.id.value].state == t::LifecycleState::Disabled,
        "mutation effect missing");
}
void installation_instance_flow(Harness& harness) {
  t::CreateInstallationRequest install{
      context("runtime.create_installation", digest(20)), "type", {std::byte{1}}};
  auto created = harness.application->create_installation(install);
  check(std::holds_alternative<t::PluginInstallation>(created), "installation creation failed");
  const auto& installation = std::get<t::PluginInstallation>(created);
  check(installation.manifest.size() == 1, "installation manifest lost");
  t::CreateInstanceRequest create{context("runtime.create_instance", digest(21)), installation.id,
                                  t::ResourceId{kHost}, false};
  auto instance = harness.application->create_instance(create);
  check(std::holds_alternative<t::InstanceCreation>(instance), "instance creation failed");
  const auto creation = std::get<t::InstanceCreation>(instance);
  check(creation.bootstrap_credential.has_value() && creation.bootstrap_credential->size() == 64,
        "managed instance missing one-time Host deployment bootstrap");
  const auto id = creation.instance.id;
  t::SetDesiredStateRequest desired{context("runtime.set_desired_state", digest(22)), id,
                                    t::DesiredState::Stopped};
  desired.context.expected_version = t::ResourceVersion{1};
  auto changed = harness.application->apply_desired_state(desired);
  check(std::holds_alternative<t::InstanceMutation>(changed) &&
            std::get<t::InstanceMutation>(changed).instance.desired == t::DesiredState::Stopped &&
            std::get<t::InstanceMutation>(changed).operation.has_value(),
        "desired state did not advance with an accepted operation");
  t::UpdateConfigRequest config{
      context("runtime.update_config", digest(23)), id, {std::byte{1}, std::byte{2}}};
  config.context.expected_version = t::ResourceVersion{2};
  auto configured = harness.application->update_config(config);
  check(std::holds_alternative<t::ConfigMutation>(configured) &&
            std::get<t::ConfigMutation>(configured).config.desired_version.value == 2,
        "config update did not advance the desired config version");
  t::InstanceConfigQuery fetch{context("runtime.get_instance_config", digest(24)), id};
  check(std::holds_alternative<t::InstanceConfiguration>(
            harness.application->get_instance_config(fetch)),
        "instance config query failed");
  t::ResourceStateQuery state{context("runtime.get_resource_state", digest(25)),
                              {t::ResourceKind::Instance, {id.value}}};
  auto resource = harness.application->get_resource_state(state);
  check(std::holds_alternative<t::ResourceState>(resource) &&
            std::get<t::ResourceState>(resource).desired == t::DesiredState::Stopped,
        "resource state lost desired run state");
  // Externally-managed instances receive a one-time bootstrap credential; Runtime stores only the
  // digest and a second creation cannot reuse the plaintext.
  t::CreateInstanceRequest external{context("runtime.create_instance", digest(26)), installation.id,
                                    std::nullopt, true};
  auto external_result = harness.application->create_instance(external);
  check(std::holds_alternative<t::InstanceCreation>(external_result),
        "external instance creation failed");
  const auto& external_creation = std::get<t::InstanceCreation>(external_result);
  check(external_creation.bootstrap_credential.has_value() &&
            external_creation.bootstrap_credential->size() == 64,
        "externally-managed instance did not receive a bootstrap credential");
  check(harness.store.bridge_credentials.size() == 2,
        "managed/external instance credentials were not each persisted exactly once");
  bool plaintext_stored = false;
  for (const auto& record : harness.store.bridge_credentials) {
    plaintext_stored = plaintext_stored ||
                       record.resource_id == *external_creation.bootstrap_credential ||
                       record.id == *external_creation.bootstrap_credential ||
                       record.resource_id == *creation.bootstrap_credential ||
                       record.id == *creation.bootstrap_credential;
  }
  check(!plaintext_stored, "instance bootstrap plaintext was persisted");
}
void authorization_flow() {
  Harness denied;
  denied.authorization.deny = true;
  t::CreateAccountRequest request{context("runtime.create_account", digest(30)), "qq", "10001",
                                  "Bot"};
  check(failure_is(denied.application->create_account(request), t::ErrorCode::PermissionDenied),
        "denied authorization was accepted");
  check(denied.store.accounts.empty() && denied.store.commits == 0,
        "denied request produced an effect");
  Harness unavailable;
  unavailable.authorization.unavailable = true;
  t::AccountQuery query{context("runtime.get_account", digest(31)), t::ResourceId{kAccount}};
  check(failure_is(unavailable.application->get_account(query), t::ErrorCode::UpstreamUnavailable),
        "authorization dependency failure was swallowed");
}
void pagination_flow(Harness& harness) {
  for (int index = 0; index < 3; ++index) {
    t::CreateAccountRequest request{
        context("runtime.create_account", digest(static_cast<std::uint8_t>(40 + index))), "qq",
        std::to_string(20000 + index), "Bot"};
    request.context.authentication.subject = t::SubjectId{kSubject};
    check(std::holds_alternative<t::BotAccount>(harness.application->create_account(request)),
          "pagination fixture creation failed");
  }
  t::AccountListQuery first{context("runtime.list_accounts", digest(50)), {2, {}}};
  auto page = harness.application->list_accounts(first);
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(page) &&
            std::get<t::ListResult<t::BotAccount>>(page).items.size() == 2 &&
            !std::get<t::ListResult<t::BotAccount>>(page).page.next_page_token.empty(),
        "first page did not return a bound next-page token");
  const auto token = std::get<t::ListResult<t::BotAccount>>(page).page.next_page_token;
  t::AccountListQuery second{context("runtime.list_accounts", digest(50)), {2, token}};
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(
            harness.application->list_accounts(second)),
        "second page rejected its own token");
  auto forged = second;
  forged.page.page_token = "00000000-0000-4000-8000-000000000000";
  check(failure_is(harness.application->list_accounts(forged), t::ErrorCode::Conflict),
        "forged page token was accepted");
  auto wrong_size = second;
  wrong_size.page.page_size = 1;
  check(failure_is(harness.application->list_accounts(wrong_size), t::ErrorCode::Conflict),
        "page token was reused with a different page size");
  harness.store.fail_reads = true;
  check(failure_is(harness.application->list_accounts(first), t::ErrorCode::UpstreamUnavailable),
        "dependency failure returned an empty page");
  harness.store.fail_reads = false;
  // A page token is immutable: two concurrent retries of the same token return the same page.
  auto retried = harness.application->list_accounts(second);
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(retried),
        "immutable page token reuse failed");
  auto original = harness.application->list_accounts(second);
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(original), "page token retry failed");
  const auto& retried_page = std::get<t::ListResult<t::BotAccount>>(retried);
  const auto& original_page = std::get<t::ListResult<t::BotAccount>>(original);
  check(retried_page.items.size() == original_page.items.size() &&
            retried_page.items.front().id.value == original_page.items.front().id.value,
        "page token retry advanced the cursor");
  // Per-item authorization filtering hides resources the caller is not granted.
  harness.authorization.denied_resources.insert(retried_page.items.front().id.value);
  auto filtered = harness.application->list_accounts(second);
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(filtered),
        "visibility-filtered list failed");
  for (const auto& item : std::get<t::ListResult<t::BotAccount>>(filtered).items) {
    check(item.id.value != retried_page.items.front().id.value,
          "unauthorized child resource leaked through List");
  }
  harness.authorization.denied_resources.clear();
  // Keyset paging reaches rows beyond the first max-page window.
  Harness many;
  for (int index = 0; index < 105; ++index) {
    t::CreateAccountRequest request{
        context("runtime.create_account", digest(static_cast<std::uint8_t>(index + 60))), "qq",
        std::to_string(30000 + index), "Bot"};
    check(std::holds_alternative<t::BotAccount>(many.application->create_account(request)),
          "large pagination fixture creation failed");
  }
  t::AccountListQuery page1{context("runtime.list_accounts", digest(90)), {100, {}}};
  auto first_big = many.application->list_accounts(page1);
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(first_big) &&
            std::get<t::ListResult<t::BotAccount>>(first_big).items.size() == 100 &&
            !std::get<t::ListResult<t::BotAccount>>(first_big).page.next_page_token.empty(),
        "first 100-row page did not fill or expose the next token");
  t::AccountListQuery page2{
      context("runtime.list_accounts", digest(91)),
      {100, std::get<t::ListResult<t::BotAccount>>(first_big).page.next_page_token}};
  auto second_big = many.application->list_accounts(page2);
  check(std::holds_alternative<t::ListResult<t::BotAccount>>(second_big) &&
            std::get<t::ListResult<t::BotAccount>>(second_big).items.size() == 5 &&
            std::get<t::ListResult<t::BotAccount>>(second_big).page.next_page_token.empty(),
        "keyset page 2 did not reach the remaining rows");
}
void readiness_flow() {
  Harness harness;
  const auto readiness = harness.application->readiness();
  check(!readiness.ready, "readiness claimed true with an unverified database/identity");
  check(readiness.dependencies.size() >= 2, "readiness dependency detail missing");
  const auto not_observed =
      t::evaluate_runtime_readiness({t::DependencyState::Available, t::DependencyState::Unverified,
                                     t::DependencyState::Unverified});
  check(!not_observed.ready, "unobserved dependencies reported ready");
  const auto database_only =
      t::evaluate_runtime_readiness(t::observed_runtime_ready(true, true, false));
  check(!database_only.ready, "missing identity authorization chain reported ready");
  const auto identity_only =
      t::evaluate_runtime_readiness(t::observed_runtime_ready(true, false, true));
  check(!identity_only.ready, "missing database reported ready");
  const auto all_available =
      t::evaluate_runtime_readiness(t::observed_runtime_ready(true, true, true));
  check(all_available.ready, "assembled application with available dependencies not ready");
  const auto unimplemented =
      t::evaluate_runtime_readiness(t::observed_runtime_ready(false, true, true));
  check(!unimplemented.ready, "unimplemented business reported ready");
  check(all_available.dependencies.size() == 3, "readiness dependency detail incomplete");
  // The injected authorization port exposes an availability observation for the composition root;
  // a test fake must never mutate the composed readiness, it can only feed this judgement.
  check(harness.authorization.available(), "authorization availability observation missing");
  harness.authorization.chain_available = false;
  check(!harness.authorization.available(), "masked unavailable authorization chain");
}
t::TimePoint bridged_now(Harness& harness) {
  return harness.store.now;
}
void bridge_flow() {
  Harness harness;
  t::BotAccount account;
  create_account(harness, &account);
  auto built = t::make_runtime_bridge_application({harness.factory,
                                                   harness.ids,
                                                   harness.secrets,
                                                   *harness.application,
                                                   nullptr,
                                                   {"plugin.config.v1"}});
  check(std::holds_alternative<std::unique_ptr<t::RuntimeBridgeApplication>>(built),
        "bridge application construction failed");
  auto bridge = std::move(std::get<std::unique_ptr<t::RuntimeBridgeApplication>>(built));
  const std::string adapter_instance = "33333333-3333-4333-8333-333333333333";

  // A1: the account binding is created before the adapter's first bridge registration; BindAdapter
  // still issues the one-time bootstrap credential for the external identity.
  t::BindAdapterRequest bind{context("runtime.bind_adapter", digest(30)), account.id,
                             t::InstanceId{adapter_instance}};
  auto bound = harness.application->bind_adapter(bind);
  check(std::holds_alternative<t::BindingMutation>(bound),
        "A1 bind before adapter registration failed");
  const auto bootstrap = std::get<t::BindingMutation>(bound).bootstrap_credential;
  check(bootstrap.has_value() && bootstrap->size() == 64, "A1 bind did not issue bootstrap");

  auto adapter_command = [&](std::uint8_t seed) {
    t::BridgeAdapterRegistration command;
    command.request_id = id_text(0x9100 + seed, 0x9100);
    command.trace_id = id_text(0x9200 + seed, 0x9200);
    command.deadline = t::TimePoint{std::chrono::seconds{1300}};
    command.adapter = t::ResourceId{kAdapter};
    command.implementation = "nonebot";
    command.implementation_version = "1.0.0";
    command.protocol_version = "kiarabot.v1";
    command.presented_instance = t::InstanceId{adapter_instance};
    command.declared_accounts = {account.id};
    command.declared_capabilities = {{"action.send_message.text.v1", "1"}};
    command.bearer_credential = *bootstrap;
    return command;
  };
  auto registered = bridge->register_adapter(adapter_command(1));
  check(std::holds_alternative<t::BridgeRegistrationResult>(registered),
        "bridge adapter registration failed");
  const auto first = std::get<t::BridgeRegistrationResult>(registered);
  check(first.assigned_instance_id.value == adapter_instance,
        "assigned instance differs from the bootstrap resource");
  check(first.authorization.fence.instance.value == adapter_instance &&
            first.authorization.fence.execution_generation.value == 1 &&
            first.authorization.fence.connection_generation.value == 1,
        "adapter fence does not carry assigned/execution/connection identity");
  check(first.heartbeat_interval_seconds == 30 && first.required_protocol_version == "kiarabot.v1",
        "adapter registration response metadata incomplete");
  check(first.authorization.authorization_version == 1, "adapter authorization version missing");
  check(std::holds_alternative<std::monostate>(bridge->authorize_frame(
            t::BridgePeerKind::Adapter, first.authorization, first.authorization.fence,
            first.authorization.proof, bridged_now(harness))),
        "current adapter frame rejected");

  // Reconnect advances only the connection generation; the old connection material is invalid.
  auto reconnected = bridge->register_adapter(adapter_command(2));
  check(std::holds_alternative<t::BridgeRegistrationResult>(reconnected),
        "bridge adapter reconnect failed");
  const auto second = std::get<t::BridgeRegistrationResult>(reconnected);
  check(second.authorization.fence.connection_generation.value == 2 &&
            second.authorization.fence.execution_generation.value == 1,
        "adapter reconnect did not advance only the connection generation");
  check(std::holds_alternative<t::Failure>(bridge->authorize_frame(
            t::BridgePeerKind::Adapter, first.authorization, first.authorization.fence,
            first.authorization.proof, bridged_now(harness))),
        "old adapter connection accepted after reconnect");

  // Single-flight refresh rotates both secrets and keeps the fence/credential version.
  t::BridgeRefreshRequest refresh;
  refresh.fence = second.authorization.fence;
  refresh.proof = second.authorization.proof;
  refresh.refresh_credential = second.authorization.refresh_credential;
  refresh.refresh_generation = second.authorization.refresh_generation;
  auto renewed = bridge->refresh(t::BridgePeerKind::Adapter, t::InstanceId{adapter_instance},
                                 second.authorization, refresh);
  check(std::holds_alternative<t::BridgeAuthorization>(renewed), "adapter refresh failed");
  const auto third = std::get<t::BridgeAuthorization>(renewed);
  check(third.refresh_generation == 2 && third.fence == second.authorization.fence &&
            third.credential_version == second.authorization.credential_version &&
            third.proof != second.authorization.proof &&
            third.refresh_credential != second.authorization.refresh_credential,
        "adapter refresh semantics incorrect");
  check(std::holds_alternative<t::Failure>(bridge->refresh(t::BridgePeerKind::Adapter,
                                                           t::InstanceId{adapter_instance},
                                                           second.authorization, refresh)),
        "stale adapter refresh material was accepted");

  // Unbinding the last account revokes the bootstrap credential and stops refresh.
  t::UnbindAdapterRequest unbind{context("runtime.unbind_adapter", digest(31)), account.id};
  unbind.context.expected_version = t::ResourceVersion{1};
  check(std::holds_alternative<t::BindingMutation>(harness.application->unbind_adapter(unbind)),
        "unbind failed");
  check(!harness.store.bridge_credentials.empty() &&
            harness.store.bridge_credentials.front().revoked,
        "unbind did not revoke adapter bootstrap");
  auto after_revoke = refresh;
  after_revoke.fence = third.fence;
  after_revoke.proof = third.proof;
  after_revoke.refresh_credential = third.refresh_credential;
  after_revoke.refresh_generation = third.refresh_generation;
  check(std::holds_alternative<t::Failure>(bridge->refresh(
            t::BridgePeerKind::Adapter, t::InstanceId{adapter_instance}, third, after_revoke)),
        "revoked adapter bootstrap still refreshed");

  // Plugin path: CreateInstance(externally_managed) issues bootstrap; the first bridge
  // registration allocates execution generation 1 and the reconnect advances connection only.
  const t::InstallationId installation{"44444444-4444-4444-8444-444444444444"};
  harness.store.installations[installation.value] = {installation,
                                                     t::ResourceId{"plugin.type"},
                                                     t::ResourceVersion{1},
                                                     t::LifecycleState::Active,
                                                     {}};
  t::CreateInstanceRequest create{context("runtime.create_instance", digest(32)), installation,
                                  std::nullopt, true};
  auto created = harness.application->create_instance(create);
  check(std::holds_alternative<t::InstanceCreation>(created),
        "externally-managed instance creation failed");
  const auto creation = std::get<t::InstanceCreation>(created);
  check(creation.bootstrap_credential.has_value() && creation.bootstrap_credential->size() == 64,
        "externally-managed instance did not issue bootstrap");
  auto plugin_command = [&]() {
    t::BridgePluginRegistration command;
    command.request_id = "plugin-register-test";
    command.instance_id = creation.instance.id.value;
    command.manifest = {"plugin.test",
                        "Plugin",
                        "1.0.0",
                        "kiarabot.plugin.v1",
                        "python-rpc",
                        "module:factory",
                        "process",
                        "{}",
                        false,
                        {"event.message.received"},
                        {"message.read"},
                        {}};
    command.supported_features = {"plugin.config.v1", "action.result.v1"};
    command.installation_id = installation;
    command.bearer_credential = *creation.bootstrap_credential;
    return command;
  };
  auto plugin_registered = bridge->register_plugin(plugin_command());
  check(std::holds_alternative<t::BridgeRegistrationResult>(plugin_registered),
        "bridge plugin registration failed");
  const auto plugin_first = std::get<t::BridgeRegistrationResult>(plugin_registered);
  check(plugin_first.assigned_instance_id.value == creation.instance.id.value &&
            plugin_first.authorization.fence.execution_generation.value == 1 &&
            plugin_first.authorization.fence.connection_generation.value == 1,
        "plugin fence identity/execution generation incorrect");
  check(plugin_first.granted_permissions == std::vector<std::string>{"message.read"},
        "plugin granted permissions differ from manifest declaration");
  check(plugin_first.enabled_features == std::vector<std::string>{"plugin.config.v1"},
        "plugin enabled features are not the server-supported intersection");
  auto plugin_reconnected = bridge->register_plugin(plugin_command());
  check(std::holds_alternative<t::BridgeRegistrationResult>(plugin_reconnected),
        "bridge plugin reconnect failed");
  const auto plugin_second = std::get<t::BridgeRegistrationResult>(plugin_reconnected);
  check(plugin_second.authorization.fence.connection_generation.value == 2 &&
            plugin_second.authorization.fence.execution_generation.value == 1,
        "plugin reconnect did not keep execution generation");
  check(std::holds_alternative<t::Failure>(bridge->authorize_frame(
            t::BridgePeerKind::Plugin, plugin_first.authorization, plugin_first.authorization.fence,
            plugin_first.authorization.proof, bridged_now(harness))),
        "old plugin connection accepted after reconnect");
}

void lifecycle_flow(Harness& harness) {
  t::BotAccount account;
  create_account(harness, &account);
  const t::ResourceRef resource{t::ResourceKind::Account, account.id};
  t::BeginDeleteRequest begin{context("runtime.begin_delete", digest(40)), resource};
  begin.context.expected_version = t::ResourceVersion{1};
  auto removed = harness.application->begin_delete(begin);
  check(std::holds_alternative<t::Operation>(removed), "begin delete failed");
  const auto operation = std::get<t::Operation>(removed);
  check(operation.state == t::OperationState::Succeeded &&
            operation.execution_outcome == t::ExecutionOutcome::KnownSucceeded,
        "runtime-only deletion did not complete");
  check(operation.participants.size() == 1 &&
            operation.participants.front().participant == "runtime",
        "runtime-only deletion participants incorrect");
  check(harness.store.accounts[account.id.value].state == t::LifecycleState::Deleted &&
            harness.store.accounts[account.id.value].version.value == 2,
        "runtime-only deletion did not mark the resource deleted");
  check(harness.store.tombstones.size() == 1, "deletion did not record a tombstone");
  // Duplicate delete is a state conflict, not a second tombstone.
  auto duplicate = begin;
  duplicate.context = context("runtime.begin_delete", digest(41));
  duplicate.context.expected_version = t::ResourceVersion{2};
  check(failure_is(harness.application->begin_delete(duplicate), t::ErrorCode::Conflict),
        "duplicate deletion was accepted");
  // Restore within the 30-day window re-activates the record without reviving other state.
  t::RestoreResourceRequest restore{context("runtime.restore_resource", digest(42)), resource};
  restore.context.expected_version = t::ResourceVersion{2};
  auto restored = harness.application->restore_resource(restore);
  check(std::holds_alternative<t::Operation>(restored), "restore failed");
  check(harness.store.accounts[account.id.value].state == t::LifecycleState::Active &&
            harness.store.accounts[account.id.value].version.value == 3,
        "restore did not re-activate the resource version");
  check(harness.store.tombstones.empty(), "restore left the tombstone behind");
  auto second_restore = restore;
  second_restore.context = context("runtime.restore_resource", digest(43));
  second_restore.context.expected_version = t::ResourceVersion{3};
  check(failure_is(harness.application->restore_resource(second_restore), t::ErrorCode::Conflict),
        "restore of a non-deleted resource was accepted");
}

void lifecycle_participant_flow(Harness& harness) {
  constexpr auto kInstance = "33333333-3333-4333-8333-333333333333";
  constexpr auto kInstallation = "44444444-4444-4444-8444-444444444444";
  t::PluginInstance instance;
  instance.id = {kInstance};
  instance.installation = {kInstallation};
  instance.plugin_type = {kHost};
  instance.version = t::ResourceVersion{1};
  instance.state = t::LifecycleState::Active;
  instance.host = t::ResourceId{kHost};
  instance.desired = t::DesiredState::Running;
  harness.store.instances[kInstance] = instance;
  harness.host_lifecycle.done = false;
  harness.scheduler_lifecycle.done = false;
  const t::ResourceRef resource{t::ResourceKind::Instance, {kInstance}};
  t::BeginDeleteRequest begin{context("runtime.begin_delete", digest(44)), resource};
  begin.context.expected_version = t::ResourceVersion{1};
  auto started = harness.application->begin_delete(begin);
  check(std::holds_alternative<t::Operation>(started), "participant deletion begin failed");
  auto operation = std::get<t::Operation>(started);
  check(operation.state == t::OperationState::Accepted && operation.participants.size() == 3,
        "participant deletion did not stay accepted with host/scheduler rows");
  check(harness.store.instances[kInstance].state == t::LifecycleState::Deleting &&
            harness.store.instances[kInstance].desired == t::DesiredState::Stopped,
        "deletion did not close the instance gate/desired state");
  // Dependency offline: blocked, never falsely completed.
  auto blocked = harness.application->lifecycle_maintenance();
  check(std::holds_alternative<t::LifecycleMaintenance>(blocked) &&
            harness.store.instances[kInstance].state == t::LifecycleState::Deleting,
        "offline participants completed the deletion");
  check(harness.host_lifecycle.calls == 1 && harness.scheduler_lifecycle.calls == 1,
        "participant ports were not consulted exactly once");
  auto operation_blocked = harness.application->get_operation(
      {context("runtime.get_operation", digest(45)),
       {t::Service::Runtime, t::OperationId{operation.ref.id.value}, resource}});
  check(std::holds_alternative<t::Operation>(operation_blocked) &&
            std::get<t::Operation>(operation_blocked).state == t::OperationState::Blocked,
        "blocked participant operation was not observable");
  // Dependencies recover: the next maintenance round completes exactly once.
  harness.host_lifecycle.done = true;
  harness.scheduler_lifecycle.done = true;
  harness.store.now += std::chrono::seconds{10};
  auto completed = harness.application->lifecycle_maintenance();
  check(std::holds_alternative<t::LifecycleMaintenance>(completed),
        "participant maintenance failed");
  check(harness.store.instances[kInstance].state == t::LifecycleState::Deleted,
        "recovered participants did not complete deletion");
  auto after = harness.application->get_operation(
      {context("runtime.get_operation", digest(46)),
       {t::Service::Runtime, t::OperationId{operation.ref.id.value}, resource}});
  check(std::holds_alternative<t::Operation>(after) &&
            std::get<t::Operation>(after).state == t::OperationState::Succeeded,
        "completed participant operation not observable as succeeded");
  // Restore after the 30-day window is refused; the resource stays deleted.
  harness.store.now += std::chrono::days{31};
  t::RestoreResourceRequest restore{context("runtime.restore_resource", digest(47)), resource};
  restore.context.expected_version = t::ResourceVersion{2};
  restore.context.deadline = harness.store.now + std::chrono::seconds{100};
  check(failure_is(harness.application->restore_resource(restore), t::ErrorCode::Conflict),
        "expired restore window was accepted");
  check(harness.store.instances[kInstance].state == t::LifecycleState::Deleted,
        "refused restore changed the resource state");
  // Purge after the 180-day tombstone retention removes the tombstone and marks PURGED.
  harness.store.now += std::chrono::days{150};
  auto purged = harness.application->lifecycle_maintenance();
  check(std::holds_alternative<t::LifecycleMaintenance>(purged), "purge maintenance failed");
  check(harness.store.instances[kInstance].state == t::LifecycleState::Purged &&
            harness.store.tombstones.empty(),
        "purge did not transition the resource or clear the tombstone");
}

void lifecycle_cleanup_flow(Harness& harness) {
  t::LifecycleMaintenance last;
  for (int round = 0; round < 60; ++round) {
    auto maintained = harness.application->lifecycle_maintenance();
    check(std::holds_alternative<t::LifecycleMaintenance>(maintained),
          "cleanup maintenance failed");
    last = std::get<t::LifecycleMaintenance>(maintained);
  }
  check(last.pruned == 3 && !last.cleanup_failed,
        "60s retention batch did not prune stubbed outbox/audit/operations exactly once");
}

void lifecycle_host_participant_flow() {
  Harness harness;
  constexpr auto kLifecycleInstance = "77777777-7777-4777-8777-777777777777";
  t::PluginInstallation installation;
  installation.id = {kLifecycleInstance};
  installation.plugin_type = {kHost};
  installation.version = t::ResourceVersion{1};
  installation.manifest = {std::byte{1}, std::byte{2}};
  harness.store.installations[installation.id.value] = installation;
  t::PluginInstance instance;
  instance.id = {kLifecycleInstance};
  instance.installation = installation.id;
  instance.plugin_type = {kHost};
  instance.version = t::ResourceVersion{1};
  instance.state = t::LifecycleState::Deleting;
  instance.host = t::ResourceId{kHost};
  instance.desired = t::DesiredState::Stopped;
  harness.store.instances[kLifecycleInstance] = instance;
  harness.host_lifecycle.done = false;
  harness.scheduler_lifecycle.done = false;
  auto participant = t::make_runtime_host_lifecycle_participant(harness.factory, harness.secrets);
  check(std::holds_alternative<std::unique_ptr<t::HostLifecycleParticipantPort>>(participant),
        "host lifecycle participant construction failed");
  auto& host = *std::get<std::unique_ptr<t::HostLifecycleParticipantPort>>(participant);
  const t::ResourceRef resource{t::ResourceKind::Instance, {kLifecycleInstance}};
  auto pending = host.stop_instance(resource);
  check(pending.disposition == t::LifecycleParticipantDisposition::Blocked &&
            pending.reason == t::ErrorCode::UpstreamUnavailable,
        "host participant must stay blocked before a receipt");
  check(harness.store.host_commands.size() == 1,
        "host participant did not enqueue a durable stop command");
  const auto& command = harness.store.host_commands.begin()->second;
  check(command.kind == t::HostCommandKind::StopExecution && command.host_id == kHost &&
            command.instance_id == kLifecycleInstance &&
            command.installation_id == installation.id.value &&
            command.execution_generation == 1 &&
            command.desired_version == instance.desired_version.value &&
            command.desired_state == t::DesiredState::Stopped &&
            command.package_sha256.size() == 64 && command.launch_profile.empty() &&
            command.arguments.empty() && command.environment_names.empty(),
        "host stop command snapshot incorrect");
  // Runtime restart/retry: a fresh participant reads the same durable snapshot instead of
  // enqueuing a second command, and still stays blocked until a known receipt arrives.
  auto restarted = t::make_runtime_host_lifecycle_participant(harness.factory, harness.secrets);
  check(std::holds_alternative<std::unique_ptr<t::HostLifecycleParticipantPort>>(restarted),
        "restarted host lifecycle participant construction failed");
  auto retry = std::get<std::unique_ptr<t::HostLifecycleParticipantPort>>(restarted)->stop_instance(
      resource);
  check(retry.disposition == t::LifecycleParticipantDisposition::Blocked &&
            harness.store.host_commands.size() == 1,
        "restart/retry duplicated or completed the stop command");
  auto& stored = harness.store.host_commands.begin()->second;
  stored.state = t::HostCommandDeliveryState::Receipted;
  stored.receipt_persisted = true;
  stored.receipt_outcome = t::ExecutionOutcome::KnownSucceeded;
  check(host.stop_instance(resource).disposition == t::LifecycleParticipantDisposition::Done,
        "known host receipt did not converge participant");
  stored.receipt_outcome = t::ExecutionOutcome::Unknown;
  auto unknown = host.stop_instance(resource);
  check(unknown.disposition == t::LifecycleParticipantDisposition::Blocked &&
            unknown.reason == t::ErrorCode::UpstreamUnavailable,
        "UNKNOWN process outcome was treated as stopped");
  stored.receipt_outcome = std::nullopt;
  stored.receipt_persisted = false;
  auto rejected = host.stop_instance(resource);
  check(rejected.disposition == t::LifecycleParticipantDisposition::Blocked &&
            rejected.reason == t::ErrorCode::Conflict,
        "rejected host stop was treated as done");
}

} // namespace

int main() {
  {
    Harness harness;
    account_flow(harness);
  }
  // Use fresh harnesses to keep assertions independent of receipt/audit history.
  {
    Harness harness;
    adapter_flow(harness);
  }
  {
    Harness harness;
    installation_instance_flow(harness);
  }
  authorization_flow();
  authorization_negative_flow();
  mutation_replay_operation_flow();
  {
    Harness harness;
    pagination_flow(harness);
  }
  readiness_flow();
  {
    Harness harness;
    lifecycle_flow(harness);
  }
  {
    Harness harness;
    lifecycle_participant_flow(harness);
  }
  {
    Harness harness;
    lifecycle_cleanup_flow(harness);
  }
  lifecycle_host_participant_flow();
  return 0;
}
