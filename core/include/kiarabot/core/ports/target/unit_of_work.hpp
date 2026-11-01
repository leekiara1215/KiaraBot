#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::core::target {

// Read absence is a successful empty optional. Corruption/dependency failure is Failure.
// Repositories borrow their owning UoW lifetime and never commit independently.
template <typename Key, typename Value> class Repository {
public:
  virtual ~Repository() = default;
  virtual Outcome<std::optional<Value>> find(const Key& key) = 0;
  // null expected = create only; update requires the currently stored owner version.
  virtual Status save(const Value& value, std::optional<ResourceVersion> expected) = 0;
};
struct ReceiptKey {
  SubjectId subject;
  std::string method;
  ResourceId resource;
  std::string idempotency_key;
};
struct CommandReceipt {
  ReceiptKey key;
  std::string fingerprint;
  OperationId operation;
  TimePoint retain_until;
};
struct AuditEntry {
  SubjectId actor;
  ResourceId resource;
  std::string request_id;
  std::string trace_id;
  std::string method;
  std::optional<ErrorCode> rejection;
  ResourceVersion authorization_version;
  TimePoint occurred_at;
};
struct OutboxEntry {
  HandoffContext context;
  // Opaque internal reference, not a protocol/SQL/JSON object or secret.
  ResourceId payload_reference;
};
struct OutboxClaim {
  OutboxEntry entry;
  std::string claim_token;
  TimePoint lease_until;
};
class ReceiptRepository {
public:
  virtual ~ReceiptRepository() = default;
  virtual Outcome<std::optional<CommandReceipt>> find(const ReceiptKey& key) = 0;
  // Unique scope insertion; different fingerprint must produce Conflict.
  virtual Status insert(const CommandReceipt& receipt) = 0;
};
class JournalRepository {
public:
  virtual ~JournalRepository() = default;
  virtual Status append_audit(const AuditEntry& entry) = 0;
  virtual Status enqueue(const OutboxEntry& entry) = 0;
  virtual Outcome<std::vector<OutboxClaim>> claim(std::size_t limit, TimePoint now) = 0;
  // Must condition on matching claim token; stale workers cannot complete newer claims.
  virtual Status confirm(const MessageId& message, const std::string& claim_token) = 0;
  virtual Outcome<std::optional<HandoffContext>> received(Service source,
                                                          const MessageId& message) = 0;
  virtual Status record_received(const HandoffContext& context) = 0;
};
enum class CommitResult { Committed, RolledBack, Unknown };
class UnitOfWork {
public:
  virtual ~UnitOfWork() = default;
  virtual ReceiptRepository& receipts() = 0;
  virtual JournalRepository& journal() = 0;
  // Terminal call. Unknown forbids rollback/re-execution assumptions: query original identity.
  // Implementation must release resources on every outcome. No network calls inside this UoW.
  virtual CommitResult commit() = 0;
  virtual void rollback() noexcept = 0;
  // Destruction of an active, uncommitted UoW rolls back. Not thread safe; one use case owns it.
};
class RuntimeUnitOfWork : public UnitOfWork {
public:
  virtual Repository<InstanceId, PluginInstance>& instances() = 0;
  virtual Repository<OperationId, Operation>& operations() = 0;
};
class RuntimeUnitOfWorkFactory {
public:
  virtual ~RuntimeUnitOfWorkFactory() = default;
  virtual Outcome<std::unique_ptr<RuntimeUnitOfWork>> begin() = 0;
};

} // namespace kiarabot::core::target
