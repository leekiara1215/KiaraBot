#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace kiarabot::core::target {

// Same value-or-error convention as core::Result, without the legacy store dependency.
template <typename Value, typename Error> using Result = std::variant<Value, Error>;
using TimePoint = std::chrono::system_clock::time_point;
enum class Service { Identity, Runtime, Host, Scheduler, Model, Control };
enum class ResourceKind {
  Account,
  Adapter,
  PluginType,
  Installation,
  Instance,
  Host,
  Namespace,
  Task,
  Model,
  Principal,
  Session,
  Action,
  Operation,
  Credential,
  Assignment
};
struct Sha256Digest {
  std::array<std::byte, 32> bytes{};
  bool operator==(const Sha256Digest&) const = default;
};

template <typename Tag> struct Identifier {
  std::string value;
  bool operator==(const Identifier&) const = default;
};
struct ResourceTag;
struct InstallationTag;
struct InstanceTag;
struct ExecutionTag;
struct ConnectionTag;
struct OperationTag;
struct MessageTag;
struct AttemptTag;
struct RuntimeEpochTag;
struct SubjectTag;
struct SessionTag;
struct AssignmentTag;
struct ClaimTag;
using ResourceId = Identifier<ResourceTag>;
using InstallationId = Identifier<InstallationTag>;
using InstanceId = Identifier<InstanceTag>;
using ExecutionId = Identifier<ExecutionTag>;
using ConnectionId = Identifier<ConnectionTag>;
using OperationId = Identifier<OperationTag>;
using MessageId = Identifier<MessageTag>;
using AttemptId = Identifier<AttemptTag>;
using RuntimeEpoch = Identifier<RuntimeEpochTag>;
using SubjectId = Identifier<SubjectTag>;
using SessionId = Identifier<SessionTag>;
using AssignmentId = Identifier<AssignmentTag>;
using ClaimToken = Identifier<ClaimTag>;
struct ResourceRef {
  ResourceKind kind;
  ResourceId id;
  bool operator==(const ResourceRef&) const = default;
};
struct OperationRef {
  Service owner;
  OperationId id;
  ResourceRef resource;
  bool operator==(const OperationRef&) const = default;
};

struct ResourceVersion {
  std::uint64_t value = 0;
  bool operator==(const ResourceVersion&) const = default;
};
struct ExecutionGeneration {
  std::uint64_t value = 0;
  bool operator==(const ExecutionGeneration&) const = default;
};
struct ConnectionGeneration {
  std::uint64_t value = 0;
  bool operator==(const ConnectionGeneration&) const = default;
};
// Runtime resolves execution/connection IDs from its authenticated registration state after
// matching wire instance, epoch and generations. Never synthesize IDs from wire generations
// or substitute the current generation for an absent/stale one; missing bindings must reject.
struct Fence {
  InstanceId instance;
  RuntimeEpoch runtime_epoch;
  ExecutionId execution;
  ExecutionGeneration execution_generation;
  ConnectionId connection;
  ConnectionGeneration connection_generation;
  bool operator==(const Fence&) const = default;
};

enum class ErrorCode {
  InvalidArgument,
  Unauthenticated,
  PermissionDenied,
  NotFound,
  Conflict,
  RateLimited,
  Timeout,
  UpstreamUnavailable,
  PluginUnavailable,
  CapabilityNotSupported,
  Internal
};
enum class Acceptance { NotAccepted, Accepted, Unknown };
enum class Recovery {
  CorrectInput,
  Authenticate,
  ReadCurrent,
  QuerySameIdentity,
  Backoff,
  Diagnose
};
struct Failure {
  ErrorCode code;
  Acceptance acceptance = Acceptance::NotAccepted;
  Recovery recovery = Recovery::Diagnose;
  std::optional<OperationRef> operation;
  std::optional<std::chrono::milliseconds> retry_after;
};
template <typename Value> using Outcome = Result<Value, Failure>;
using Status = Outcome<std::monostate>;

enum class OperationState { Accepted, Running, Blocked, Succeeded, Failed, Cancelled, Expired };
enum class ExecutionOutcome { NotStarted, KnownSucceeded, KnownFailed, Unknown };
struct Operation {
  OperationRef ref;
  ResourceVersion version;
  OperationState state = OperationState::Accepted;
  ExecutionOutcome execution_outcome = ExecutionOutcome::NotStarted;
  std::optional<ErrorCode> blocked_reason;
  std::optional<TimePoint> next_attempt;
};
enum class LifecycleState { Active, Disabled, Deleting, Deleted, Restoring, Purged };

struct PluginInstance {
  InstanceId id;
  InstallationId installation;
  ResourceId plugin_type;
  ResourceVersion desired_version;
  ResourceVersion applied_config_version;
  std::optional<Fence> current;
  ResourceVersion version;
  LifecycleState state = LifecycleState::Active;
  std::optional<ResourceId> host;
};

} // namespace kiarabot::core::target
