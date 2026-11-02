#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/context.hpp"

namespace kiarabot::core::target {

enum class PrincipalKind { Human, PlatformService, Adapter, Plugin };
enum class FixedRole { Admin, Operator, Viewer };
struct Principal {
  SubjectId id;
  PrincipalKind kind;
  std::optional<ResourceRef> binding;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
};
struct LoginSession {
  SessionId id;
  SubjectId subject;
  ResourceVersion version;
  std::uint64_t rotation;
  TimePoint expires_at;
  bool revoked = false;
};
// Immutable public projection, including creation time. Safe in an ordinary success receipt;
// contains neither token verifiers nor secret key references.
struct SessionMetadata {
  SessionId id;
  SubjectId subject;
  TimePoint created_at;
  TimePoint expires_at;
  std::uint64_t rotation;
  ResourceVersion version;
  bool revoked;
};
struct ResourceGrant {
  AssignmentId id;
  SubjectId subject;
  std::optional<FixedRole> role;
  std::vector<ResourceScope> scopes;
  ResourceVersion version;
};
struct AuthorizationState {
  SubjectId subject;
  ResourceVersion version;
  // Durable coalescing marker: ordinary outbox exhaustion must not prevent revocation.
  bool invalidation_dirty = false;
};
struct BotAccount {
  ResourceId id;
  std::string platform;
  std::string platform_account_id;
  std::string display_name;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
};
struct PluginInstallation {
  InstallationId id;
  ResourceId plugin_type;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
  // Canonical serialized plugin manifest accepted at installation time; Core never parses plugin
  // JSON/protobuf. Rejection/validation of manifest schema stays at the trusted boundary.
  std::vector<std::byte> manifest{};
};
// The wire TaskRecord.target carries only conversation id/type; adapter/account are legacy
// internal storage fields, blank for Scheduler-created tasks until P10b resolves the target from
// the owner instance's Runtime binding.
struct ConversationKey {
  ResourceId adapter;
  ResourceId account;
  std::string conversation_id;
  enum class Kind { Unspecified, Private, Group } kind;
  bool operator==(const ConversationKey&) const = default;
};

struct TaskKey {
  InstanceId owner;
  std::string task_id;
  bool operator==(const TaskKey&) const = default;
};
enum class TaskState { Pending, Running, Retrying, Completed, Failed, Cancelled };
struct ScheduledTask {
  TaskKey key;
  InstallationId installation;
  ConversationKey target;
  std::vector<std::byte> payload;
  TimePoint run_at;
  TaskState state = TaskState::Pending;
  ResourceVersion version;
  // Owned by the one-shot task and fixed at creation; the occurrence row appears only when the
  // due sweep durably enqueues the TimerTriggered handoff.
  MessageId occurrence;
  bool cancel_requested = false;
  std::optional<ErrorCode> reason;
  TimePoint created_at;
  TimePoint updated_at;
};
struct Occurrence {
  InstanceId owner;
  InstallationId installation;
  std::string task_id;
  ResourceVersion task_version;
  MessageId occurrence_id;
  TimePoint due_at;
  HandoffContext handoff;
  ResourceVersion version;
  OperationState state = OperationState::Accepted;
  std::optional<std::string> accepted_event_id;
  std::optional<TimePoint> acknowledged_at;
  std::uint32_t attempts = 0;
  std::optional<ErrorCode> last_error;
  TimePoint created_at;
  TimePoint updated_at;
};
enum class ModelCallState { Started, Succeeded, Failed, Unknown };
// P11 UNKNOWN resolution outcome. Unspecified while the call is unresolved; only the terminal
// resolutions below are accepted by ResolveModelCall. Stored as an integer column.
enum class ModelCallDisposition {
  Unspecified,
  NotExecuted,
  ExecutedNotBilled,
  ExecutedBilled,
  Abandoned
};
struct ModelIntent {
  InstanceId owner;
  std::string logical_call_id;
  ResourceId model;
  ResourceVersion config_version;
  Sha256Digest request_hash;
  ModelCallState state;
  ResourceVersion version;
  TimePoint created_at;
  TimePoint observed_at;
  // P11a durable call facts. Defaulted so P03 aggregate initialization stays source-compatible;
  // the P11 dedicated repository always writes every field.
  ResourceVersion secret_version{0};
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
  bool usage_known = false;
  bool may_have_billed = false;
  std::string provider_request_id;
  std::uint32_t latency_ms = 0;
  std::optional<ErrorCode> error;
  ModelCallDisposition disposition = ModelCallDisposition::Unspecified;
  std::optional<TimePoint> resolved_at;
  std::string resolved_by;
};

struct ModelCallKey {
  InstanceId owner;
  std::string logical_call_id;
  bool operator==(const ModelCallKey&) const = default;
};

} // namespace kiarabot::core::target
