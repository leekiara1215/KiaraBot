#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/namespace_rules.hpp"
#include "kiarabot/core/domain/target/persistence.hpp"

namespace kiarabot::core::target {

// P09 namespace repositories borrow the Runtime UoW lifetime and never commit. Empty optional is
// absence; every fault is a Failure (never absence). Capacity/quota arguments are trusted
// application policy, never request-controlled. Time arguments are application samples while
// implementations still adjudicate expiry/conditions against authoritative DB UTC in the same
// conditional write. PG implementation lands in P09 phase 2 after P07a-A; phase 1 ships the ports,
// pure application and fail-closed defaults only.
class NamespaceRepository {
public:
  virtual ~NamespaceRepository() = default;
  // Expired rows are lazily treated as absent by the implementation in the same read.
  virtual Outcome<std::optional<NamespaceValue>> get(const NamespaceScope& scope,
                                                     const std::string& key, TimePoint now) = 0;
  // Conditional create/update; conflict when the current row no longer matches `expected`.
  virtual Outcome<ResourceVersion> compare_exchange(const NamespaceScope& scope,
                                                    const std::string& key,
                                                    const CompareCondition& expected,
                                                    const NamespaceValue& value, TimePoint now) = 0;
  // Conditional remove; Conflict when the row is absent or its version no longer matches.
  virtual Status compare_remove(const NamespaceScope& scope, const std::string& key,
                                const CompareCondition& expected, TimePoint now) = 0;
};

class SharedNamespaceRepository {
public:
  virtual ~SharedNamespaceRepository() = default;
  virtual Outcome<std::optional<SharedNamespaceRecord>> find(const ResourceId& namespace_id) = 0;
  // At most one shared namespace per installation (partial unique index in phase 2 PG).
  virtual Outcome<std::optional<SharedNamespaceRecord>>
  by_installation(const InstallationId& installation) = 0;
  // null expected = create only; updates match the current namespace version.
  virtual Status save(const SharedNamespaceRecord& record,
                      std::optional<ResourceVersion> expected) = 0;
  virtual Outcome<std::vector<SharedMemberRecord>> members(const ResourceId& namespace_id) = 0;
  // Replaces the full member list atomically with the namespace version condition.
  virtual Status save_members(const ResourceId& namespace_id, ResourceVersion expected,
                              const std::vector<SharedMember>& members,
                              ResourceVersion next_version, TimePoint now) = 0;
};

// Persistent quota counters. Admission is authoritative: implementations lock/recheck the same
// namespace and installation counters inside the UoW so concurrent instances cannot bypass the
// aggregate 8192 keys / 8 MiB limit. Deltas are signed (negative on delete).
struct NamespaceQuotaChange {
  std::int64_t keys = 0;
  std::int64_t bytes = 0;
};
class NamespaceQuotaRepository {
public:
  virtual ~NamespaceQuotaRepository() = default;
  virtual Outcome<NamespaceQuota> usage(const NamespaceScope& scope,
                                        const InstallationId& installation) = 0;
  // Returns RateLimited when the resulting namespace/installation usage would exceed limits.
  virtual Status admit(const NamespaceScope& scope, const InstallationId& installation,
                       NamespaceQuotaChange change) = 0;
};

// Storage/management receipts share the generic command receipt durability rule (7 days, never
// evict valid rows) but keep their own typed result/projection. Phase 2 maps this port onto the
// existing command_receipts store; no new physical receipt table is introduced.
class StorageReceiptRepository {
public:
  virtual ~StorageReceiptRepository() = default;
  virtual Outcome<std::optional<NamespaceReceipt>> find(const ReceiptKey& key) = 0;
  // Same key + fingerprint returns the ORIGINAL receipt (no TTL/changes); changed fingerprint is
  // Conflict. Capacity is per instance and per installation; reject new rows at capacity.
  virtual Outcome<NamespaceReceiptWrite> insert(const NamespaceReceipt& receipt,
                                                NamespaceReceiptCapacity capacity) = 0;
  virtual Outcome<std::size_t> prune_expired(TimePoint now, std::size_t limit) = 0;
};

namespace namespace_ports_detail {
inline Failure unsupported_namespace_port() {
  return {ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::Diagnose,
          std::nullopt, std::nullopt};
}
// Fail-closed phase-1 defaults: a Runtime adapter that has not implemented P09 must never report a
// fake empty namespace. Phase 2 replaces these by overriding the corresponding UoW accessors.
class UnsupportedNamespaceRepository final : public NamespaceRepository {
public:
  Outcome<std::optional<NamespaceValue>> get(const NamespaceScope&, const std::string&,
                                             TimePoint) override {
    return unsupported_namespace_port();
  }
  Outcome<ResourceVersion> compare_exchange(const NamespaceScope&, const std::string&,
                                            const CompareCondition&, const NamespaceValue&,
                                            TimePoint) override {
    return unsupported_namespace_port();
  }
  Status compare_remove(const NamespaceScope&, const std::string&, const CompareCondition&,
                        TimePoint) override {
    return unsupported_namespace_port();
  }
};
class UnsupportedSharedNamespaceRepository final : public SharedNamespaceRepository {
public:
  Outcome<std::optional<SharedNamespaceRecord>> find(const ResourceId&) override {
    return unsupported_namespace_port();
  }
  Outcome<std::optional<SharedNamespaceRecord>> by_installation(const InstallationId&) override {
    return unsupported_namespace_port();
  }
  Status save(const SharedNamespaceRecord&, std::optional<ResourceVersion>) override {
    return unsupported_namespace_port();
  }
  Outcome<std::vector<SharedMemberRecord>> members(const ResourceId&) override {
    return unsupported_namespace_port();
  }
  Status save_members(const ResourceId&, ResourceVersion, const std::vector<SharedMember>&,
                      ResourceVersion, TimePoint) override {
    return unsupported_namespace_port();
  }
};
class UnsupportedNamespaceQuotaRepository final : public NamespaceQuotaRepository {
public:
  Outcome<NamespaceQuota> usage(const NamespaceScope&, const InstallationId&) override {
    return unsupported_namespace_port();
  }
  Status admit(const NamespaceScope&, const InstallationId&, NamespaceQuotaChange) override {
    return unsupported_namespace_port();
  }
};
class UnsupportedStorageReceiptRepository final : public StorageReceiptRepository {
public:
  Outcome<std::optional<NamespaceReceipt>> find(const ReceiptKey&) override {
    return unsupported_namespace_port();
  }
  Outcome<NamespaceReceiptWrite> insert(const NamespaceReceipt&,
                                        NamespaceReceiptCapacity) override {
    return unsupported_namespace_port();
  }
  Outcome<std::size_t> prune_expired(TimePoint, std::size_t) override {
    return unsupported_namespace_port();
  }
};
inline NamespaceRepository& unsupported_namespaces() {
  static UnsupportedNamespaceRepository repository;
  return repository;
}
inline SharedNamespaceRepository& unsupported_shared_namespaces() {
  static UnsupportedSharedNamespaceRepository repository;
  return repository;
}
inline NamespaceQuotaRepository& unsupported_namespace_quota() {
  static UnsupportedNamespaceQuotaRepository repository;
  return repository;
}
inline StorageReceiptRepository& unsupported_storage_receipts() {
  static UnsupportedStorageReceiptRepository repository;
  return repository;
}
} // namespace namespace_ports_detail

} // namespace kiarabot::core::target
