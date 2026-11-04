#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <pqxx/pqxx>

#include "kiarabot/core/ports/target/unit_of_work.hpp"
#include "kiarabot/infrastructure/postgres/database.hpp"

namespace kiarabot::infrastructure::postgres::detail {

// Infrastructure-only borrow. The callback must contain local SQL/codec work only: no remote
// calls, crypto, user callbacks, commit or rollback. Failure poisons the owning common UoW.
class SqlWork {
public:
  virtual ~SqlWork() = default;
  virtual core::target::Status
  run(const std::function<core::target::Status(pqxx::transaction_base&)>& function) = 0;
};
struct CommonWorkAccess {
  std::unique_ptr<core::target::UnitOfWork> work;
  // Non-owning; valid only while work is alive. Terminal work rejects all further access.
  SqlWork* sql;
};
struct DatabaseAccess {
  static core::target::Outcome<CommonWorkAccess> begin(Database& database,
                                                       bool resource_snapshot = false);
};

// Closed infrastructure dispatch to concrete relation tables. Not a Core port or a blob store.
// Values encode the ReplayResult variant index for live-relation dispatch; new kinds are appended
// and existing values never change (they are also stored as replay_results.kind snapshots).
enum class RecordKind {
  Principal = 3,
  Session = 4,
  Grant = 5,
  Account = 6,
  Installation = 7,
  Instance = 8,
  Task = 9,
  Occurrence = 10,
  ModelIntent = 11,
  Adapter = 15,
  AdapterBinding = 16,
  Host = 17,
  InstanceConfig = 18
};
core::target::Outcome<std::optional<core::target::ReplayResult>>
find_record(core::target::UnitOfWork& work, RecordKind kind, std::span<const std::string> key);
core::target::Status save_record(core::target::UnitOfWork& work, RecordKind kind,
                                 const core::target::ReplayResult& value,
                                 std::optional<core::target::ResourceVersion> expected);
// Owner-ordered bounded projection for typed List queries. Trusted application limit only; the
// repository never applies authorization. Faults are Failure, never an empty page.
core::target::Outcome<std::vector<core::target::ReplayResult>>
list_records(core::target::UnitOfWork& work, RecordKind kind, std::size_t limit,
             const std::string& after);
// One-time external-bridge bootstrap credential persistence. Runtime stores only the SHA-256
// digest; unsalted/short digests are rejected by the table CHECK and by these helpers.
core::target::Outcome<std::size_t> active_binding_count(core::target::UnitOfWork& work,
                                                        const std::string& adapter_instance);
core::target::Outcome<bool> bridge_credential_active(core::target::UnitOfWork& work,
                                                     const std::string& resource_id);
core::target::Outcome<std::optional<core::target::BridgeCredentialRecord>>
bridge_credential_by_digest(core::target::UnitOfWork& work,
                            const core::target::Sha256Digest& digest);
core::target::Status insert_bridge_credential(core::target::UnitOfWork& work,
                                              const core::target::BridgeCredentialRecord& record);
core::target::Status revoke_bridge_credentials(core::target::UnitOfWork& work,
                                               const std::string& resource_id,
                                               core::target::TimePoint now);
// Resolve the stable adapter resource id from an assigned adapter instance identity (unique key).
core::target::Outcome<std::optional<core::target::ResourceId>>
find_adapter_id(core::target::UnitOfWork& work, const core::target::InstanceId& instance);
core::target::Outcome<std::optional<core::target::AuthorizationState>>
find_authorization(core::target::UnitOfWork& work, const core::target::SubjectId& subject);
core::target::Status save_authorization(core::target::UnitOfWork& work,
                                        const core::target::AuthorizationState& state,
                                        std::optional<core::target::ResourceVersion> expected);
} // namespace kiarabot::infrastructure::postgres::detail
