#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/domain/target/namespace_rules.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// Plugin storage commands carry the trusted, delegation-derived instance identity. The wire scope
// is only a request hint; installation_shared/installation_shared_id are resolved against this
// authenticated instance, never trusted as an owner claim.
enum class RequestedScopeKind { DefaultPrivate, InstallationShared, InstallationSharedId };
struct StorageCommandContext {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string method;
  std::string idempotency_key;
  Sha256Digest fingerprint;
  TimePoint deadline;
  InstanceId instance;
  RequestedScopeKind requested = RequestedScopeKind::DefaultPrivate;
  ResourceId shared_namespace_id;
  // P09b: a PluginBridge frame carries an authorization decision produced by the trusted P06
  // bridge session (authentication + ConnectionAuthorization + granted manifest permission).
  // Wire frames can never populate this; only the bridge transport sets it after verifying the
  // registered plugin profile. When present the application validates the decision binding and
  // skips the extra Identity round-trip for that frame.
  std::optional<RuntimeAuthorizationDecision> bridge_authorization;
};
struct StorageGetRequest {
  StorageCommandContext context;
  std::string key;
};
struct StorageSetRequest {
  StorageCommandContext context;
  std::string key;
  std::vector<std::byte> value;
  std::optional<TimePoint> expires_at;
  // Private Set must omit it; shared Set requires it (absent=create, version=update).
  std::optional<CompareCondition> condition;
};
struct StorageDeleteRequest {
  StorageCommandContext context;
  std::string key;
  // Private delete may omit it (LWW); shared delete requires the current version.
  std::optional<CompareCondition> condition;
};
struct StorageCompareAndSetRequest {
  StorageCommandContext context;
  std::string key;
  std::vector<std::byte> value;
  CompareCondition condition;
  std::optional<TimePoint> expires_at;
};

struct StorageEntryResult {
  NamespaceScope scope;
  NamespaceValue value;
};
struct StorageDeleteResult {
  NamespaceScope scope;
  std::string key;
  bool deleted = false;
  std::optional<ResourceVersion> version;
};

struct CreateSharedNamespaceRequest {
  CommandContext context;
  InstallationId installation;
  // Absent on first creation; present for an idempotent same-version retry.
  std::optional<ResourceVersion> expected_version;
};
struct GetSharedNamespaceQuery {
  CommandContext context;
  ResourceId namespace_id;
};
struct SetSharedMembersRequest {
  CommandContext context;
  ResourceId namespace_id;
  InstallationId installation;
  std::vector<SharedMember> members;
  std::optional<ResourceVersion> expected_version;
};
struct DisableSharedNamespaceRequest {
  CommandContext context;
  ResourceId namespace_id;
  std::optional<ResourceVersion> expected_version;
};
struct NamespaceView {
  SharedNamespaceRecord namespace_record;
  std::vector<SharedMember> members;
  NamespaceQuota quota;
};
struct NamespaceMutation {
  SharedNamespaceRecord namespace_record;
  std::optional<OperationRef> operation;
};
struct SharedMembersMutation {
  ResourceVersion version;
  OperationRef operation;
};

// Runtime storage/namespace application: each call owns its short UoWs, performs current online
// authorization outside a UoW, then revalidates membership/version in the write UoW and commits the
// value change, quota counters, typed receipt and audit atomically. No network I/O inside a UoW.
class RuntimeStorageApplication {
public:
  virtual ~RuntimeStorageApplication() = default;

  virtual Outcome<StorageEntryResult> get_storage(const StorageGetRequest& request) = 0;
  virtual Outcome<StorageEntryResult> set_storage(const StorageSetRequest& request) = 0;
  virtual Outcome<StorageDeleteResult> delete_storage(const StorageDeleteRequest& request) = 0;
  virtual Outcome<StorageEntryResult>
  compare_and_set_storage(const StorageCompareAndSetRequest& request) = 0;

  virtual Outcome<NamespaceMutation>
  create_shared_namespace(const CreateSharedNamespaceRequest& request) = 0;
  virtual Outcome<NamespaceView> get_shared_namespace(const GetSharedNamespaceQuery& request) = 0;
  virtual Outcome<SharedMembersMutation>
  set_shared_members(const SetSharedMembersRequest& request) = 0;
  virtual Outcome<SharedMembersMutation>
  disable_shared_namespace(const DisableSharedNamespaceRequest& request) = 0;
};

struct RuntimeStorageDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeAuthorizationPort& authorization;
};

Outcome<std::unique_ptr<RuntimeStorageApplication>>
make_runtime_storage_application(RuntimeStorageDependencies dependencies);

} // namespace kiarabot::core::target
