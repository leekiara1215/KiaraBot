#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/action_rules.hpp"
#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/ports/target/action_ports.hpp"
#include "kiarabot/core/ports/target/business_ports.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/core/ports/target/runtime_secrets.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// Trusted identity of one externally authenticated plugin action submission. The bridge transport
// resolves the instance from the registered connection; the wire target_source is only a request
// hint and is validated against the authoritative account binding and adapter record below.
struct ActionSubmitRequest {
  InstanceId owner_instance;
  std::string request_id;
  std::string trace_id;
  // Manifest permissions granted to the authenticated plugin instance (declaration vocabulary;
  // per-kind authorization is checked against the frozen P07 permission mapping).
  std::vector<std::string> owner_permissions;
  std::vector<std::string> enabled_features;
  ActionId action_id;
  ResourceId account_id;
  ResourceId adapter_id;
  ActionKind kind = ActionKind::SendMessage;
  std::string idempotency_key;
  std::string serialized_envelope;
  std::string target_conversation;
  std::uint8_t target_conversation_kind = 0;
  TimePoint deadline;
};

struct ActionSubmitOutcome {
  ActionRecord record;
  bool duplicate = false;
};

struct ActionOwnerQuery {
  InstanceId owner_instance;
  std::string request_id;
  std::string trace_id;
  std::optional<ActionId> action_id;
  std::optional<std::string> idempotency_key;
};

struct ActionCancelRequest {
  InstanceId owner_instance;
  std::string request_id;
  std::string trace_id;
  ActionId action_id;
};

struct ActionControlResultQuery {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string method;
  TimePoint deadline;
  ResourceId account_id;
  std::optional<ActionId> action_id;
  std::optional<std::string> idempotency_key;
  std::optional<InstanceId> owner_instance;
};

struct ActionMetadataQuery {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string method;
  TimePoint deadline;
  ResourceId account_id;
  std::optional<ActionState> state;
  std::optional<ActionKind> kind;
  std::size_t limit = 20;
  std::string after_action_id;
};

struct ActionMetadataPage {
  std::vector<ActionRecord> records;
  std::string next_action_id;
};

struct ActionPolicyQuery {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string method;
  TimePoint deadline;
  ResourceId account_id;
};

struct ActionPolicyUpdate {
  AuthenticationContext authentication;
  std::string request_id;
  std::string trace_id;
  std::string method;
  TimePoint deadline;
  ActionPolicy policy;
  std::optional<ResourceVersion> expected_version;
};

struct ActionSweepResult {
  std::size_t expired = 0;
  std::size_t quota_pruned = 0;
  std::size_t evidence_pruned = 0;
  std::size_t records_pruned = 0;
};

// Runtime Action application: every call owns short Runtime UoWs and commits the authoritative
// Action row, its persistent quota/budget admission, adapter evidence and audit entry atomically.
// Adapter frames are written by the bridge transport after commit; no network I/O happens here.
// The application also implements the trusted dispatch/evidence ports consumed by the bridge.
class RuntimeActionApplication : public ActionDispatchPort, public ActionEvidenceSink {
public:
  ~RuntimeActionApplication() override = default;

  virtual Outcome<ActionSubmitOutcome> submit_action(const ActionSubmitRequest& request) = 0;
  virtual Outcome<ActionRecord> query_owner_action(const ActionOwnerQuery& query) = 0;
  virtual Outcome<ActionRecord> cancel_action(const ActionCancelRequest& request) = 0;
  // Control-scoped query: requires an online action.inspect grant for exactly the requested
  // account. By idempotency_key the owner_instance_id is mandatory; there is no admin override.
  virtual Outcome<ActionRecord> query_control_action(const ActionControlResultQuery& query) = 0;
  virtual Outcome<ActionMetadataPage> list_metadata(const ActionMetadataQuery& query) = 0;
  virtual Outcome<ActionPolicy> get_policy(const ActionPolicyQuery& query) = 0;
  virtual Outcome<ActionPolicy> update_policy(const ActionPolicyUpdate& request) = 0;
  // Maintenance: expire accepted actions past deadline, prune quota windows and retained evidence
  // and terminal records in bounded batches. Called by the composition dependency hook.
  virtual Outcome<ActionSweepResult> maintenance() = 0;
};

struct RuntimeActionDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeIdPort& ids;
  RuntimeSecretPort& secrets;
  // Null means Control-scoped query/update fail closed as Unavailable; plugin-owner operations do
  // not need the online Identity authorization chain for their declared-action path.
  RuntimeAuthorizationPort* authorization = nullptr;
  ActionCapacity capacity;
  ActionQuotaLimits quota;
};

Outcome<std::unique_ptr<RuntimeActionApplication>>
make_runtime_action_application(RuntimeActionDependencies dependencies);

} // namespace kiarabot::core::target
