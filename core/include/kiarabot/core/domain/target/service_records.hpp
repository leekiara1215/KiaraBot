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
};
struct ConversationKey {
  ResourceId adapter;
  ResourceId account;
  std::string conversation_id;
  enum class Kind { Unspecified, Private, Group } kind;
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
  MessageId occurrence;
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
};
enum class ModelCallState { Started, Succeeded, Failed, Unknown };
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
};

struct ModelCallKey {
  InstanceId owner;
  std::string logical_call_id;
  bool operator==(const ModelCallKey&) const = default;
};

} // namespace kiarabot::core::target
