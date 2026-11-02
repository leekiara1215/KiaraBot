#pragma once

#include "kiarabot/core/ports/target/business_ports.hpp"
#include "record_access.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// PG implementation of the four P07 Runtime Action ports. Each class borrows the owning Runtime
// UoW (SqlWork) so all reads/writes participate in the caller's transaction; implementations never
// open a connection, commit, roll back another UoW, or perform network calls. Temporal predicates
// are rechecked against database clock_timestamp() inside the conditional SQL. Authorization and
// policy decisions stay in the pure application; these adapters only adjudicate durable row state.
class ActionRecords final : public core::target::ActionRepository {
public:
  explicit ActionRecords(SqlWork& sql) : sql_(sql) {}

  core::target::Outcome<std::optional<core::target::ActionRecord>>
  find_by_id(const core::target::ActionId& action_id) override;
  core::target::Outcome<std::optional<core::target::ActionRecord>>
  find_by_owner_key(const core::target::InstanceId& owner,
                    std::string_view idempotency_key) override;
  core::target::Outcome<core::target::ActionRecord>
  insert(const core::target::ActionRecord& record, core::target::ActionCapacity capacity) override;
  core::target::Status mark_dispatched(const core::target::ActionId& action_id,
                                       const core::target::AttemptId& attempt_id,
                                       const core::target::InstanceId& adapter_instance,
                                       core::target::TimePoint now) override;
  core::target::Outcome<core::target::ActionTerminalReceipt>
  record_terminal(const core::target::ActionRecord& current,
                  const core::target::ActionTerminalResult& result,
                  std::optional<core::target::AttemptId> attempt_id,
                  std::optional<core::target::InstanceId> adapter_instance,
                  core::target::TimePoint now) override;
  core::target::Status cancel(const core::target::ActionId& action_id,
                              core::target::TimePoint now) override;
  core::target::Outcome<std::vector<core::target::ActionRecord>>
  expire_due(std::size_t limit, core::target::TimePoint now) override;
  core::target::Outcome<std::vector<core::target::ActionRecord>>
  list_owner(const core::target::InstanceId& owner,
             const core::target::ActionPageRequest& page) override;
  core::target::Outcome<std::vector<core::target::ActionRecord>>
  list_control(const core::target::ActionControlQuery& query) override;
  core::target::Outcome<std::vector<core::target::ActionRecord>>
  list_due(const core::target::ResourceId& adapter_id, std::size_t limit,
           core::target::TimePoint now) override;
  core::target::Outcome<std::size_t> prune_terminal(core::target::TimePoint now,
                                                    std::size_t limit) override;

private:
  SqlWork& sql_;
};

class ActionQuotaRecords final : public core::target::ActionQuotaRepository {
public:
  explicit ActionQuotaRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Status admit(const core::target::ActionId& action_id, core::target::ActionKind kind,
                             std::span<const core::target::ActionScope> scopes,
                             const core::target::ActionQuotaLimits& limits,
                             core::target::TimePoint now) override;
  core::target::Outcome<std::size_t> prune_windows(core::target::TimePoint now,
                                                   std::size_t limit) override;

private:
  SqlWork& sql_;
};

class ActionPolicyRecords final : public core::target::ActionPolicyRepository {
public:
  explicit ActionPolicyRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Outcome<std::optional<core::target::ActionPolicy>>
  get(const core::target::ResourceId& account_id) override;
  core::target::Status upsert(const core::target::ActionPolicy& policy,
                              std::optional<core::target::ResourceVersion> expected) override;

private:
  SqlWork& sql_;
};

class ActionEvidenceRecords final : public core::target::ActionEvidenceRepository {
public:
  explicit ActionEvidenceRecords(SqlWork& sql) : sql_(sql) {}
  core::target::Status append(const core::target::ActionEvidenceRecord& evidence) override;
  core::target::Outcome<std::vector<core::target::ActionEvidenceRecord>>
  list(const core::target::ActionId& action_id) override;
  core::target::Outcome<std::size_t> prune(core::target::TimePoint now, std::size_t limit) override;

private:
  SqlWork& sql_;
};

} // namespace kiarabot::infrastructure::postgres::detail
