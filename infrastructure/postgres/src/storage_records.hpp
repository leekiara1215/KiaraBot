#pragma once

#include "kiarabot/core/ports/target/namespace_ports.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// PG implementation of the four P09 Runtime namespace/storage ports. Each class borrows the owning
// Runtime UoW (SqlWork), never commits/opens another transaction or performs network calls. Private
// namespace identity is the stable authenticated instance (created lazily on first write); shared
// namespaces are the installation-owned kind=1 row managed through the admin ports. Quota counters
// are updated in the same UoW as the value row under the fixed order namespace -> members -> key ->
// installation aggregate; all temporal predicates recheck database UTC.
class NamespaceRecords final : public core::target::NamespaceRepository {
public:
  explicit NamespaceRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Outcome<std::optional<core::target::NamespaceValue>>
  get(const core::target::NamespaceScope& scope, const std::string& key,
      core::target::TimePoint now) override;
  core::target::Outcome<core::target::ResourceVersion>
  compare_exchange(const core::target::NamespaceScope& scope, const std::string& key,
                   const core::target::CompareCondition& expected,
                   const core::target::NamespaceValue& value, core::target::TimePoint now) override;
  core::target::Status compare_remove(const core::target::NamespaceScope& scope,
                                      const std::string& key,
                                      const core::target::CompareCondition& expected,
                                      core::target::TimePoint now) override;

private:
  SqlWork& sql_;
};

class SharedNamespaceRecords final : public core::target::SharedNamespaceRepository {
public:
  explicit SharedNamespaceRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Outcome<std::optional<core::target::SharedNamespaceRecord>>
  find(const core::target::ResourceId& namespace_id) override;
  core::target::Outcome<std::optional<core::target::SharedNamespaceRecord>>
  by_installation(const core::target::InstallationId& installation) override;
  core::target::Status save(const core::target::SharedNamespaceRecord& record,
                            std::optional<core::target::ResourceVersion> expected) override;
  core::target::Outcome<std::vector<core::target::SharedMemberRecord>>
  members(const core::target::ResourceId& namespace_id) override;
  core::target::Status save_members(const core::target::ResourceId& namespace_id,
                                    core::target::ResourceVersion expected,
                                    const std::vector<core::target::SharedMember>& members,
                                    core::target::ResourceVersion next_version,
                                    core::target::TimePoint now) override;

private:
  SqlWork& sql_;
};

class NamespaceQuotaRecords final : public core::target::NamespaceQuotaRepository {
public:
  explicit NamespaceQuotaRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Outcome<core::target::NamespaceQuota>
  usage(const core::target::NamespaceScope& scope,
        const core::target::InstallationId& installation) override;
  core::target::Status admit(const core::target::NamespaceScope& scope,
                             const core::target::InstallationId& installation,
                             core::target::NamespaceQuotaChange change) override;

private:
  SqlWork& sql_;
};

class StorageReceiptRecords final : public core::target::StorageReceiptRepository {
public:
  explicit StorageReceiptRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Outcome<std::optional<core::target::NamespaceReceipt>>
  find(const core::target::ReceiptKey& key) override;
  core::target::Outcome<core::target::NamespaceReceiptWrite>
  insert(const core::target::NamespaceReceipt& receipt,
         core::target::NamespaceReceiptCapacity capacity) override;
  core::target::Outcome<std::size_t> prune_expired(core::target::TimePoint now,
                                                   std::size_t limit) override;

private:
  SqlWork& sql_;
};

} // namespace kiarabot::infrastructure::postgres::detail
