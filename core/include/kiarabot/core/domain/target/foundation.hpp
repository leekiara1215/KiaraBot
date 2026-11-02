#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kiarabot::core::target {

// Same value-or-error convention as core::Result, without the legacy store dependency.
template <typename Value, typename Error> using Result = std::variant<Value, Error>;
// Canonical signed int64 epoch nanoseconds on all supported platforms (including macOS).
using TimePoint = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;
static_assert(std::numeric_limits<TimePoint::duration::rep>::is_signed &&
              std::numeric_limits<TimePoint::duration::rep>::digits == 63);
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
  Assignment,
  Catalog = 15
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
// P13a owner-local participant progress. Participants never claim a global completion they cannot
// observe: a pending/unknown participant keeps the operation non-terminal and P13a maps it to the
// frozen OperationProgress wire projection (participant/state/reason/next_attempt_at only).
struct OperationParticipant {
  std::string participant;
  OperationState state = OperationState::Accepted;
  std::optional<ErrorCode> reason;
  std::optional<TimePoint> next_attempt;
};
struct Operation {
  OperationRef ref;
  ResourceVersion version;
  OperationState state = OperationState::Accepted;
  ExecutionOutcome execution_outcome = ExecutionOutcome::NotStarted;
  std::optional<ErrorCode> blocked_reason;
  std::optional<TimePoint> next_attempt;
  // P13a progress rows and retention metadata. These fields are owner-internal and deliberately
  // absent from the wire Operation; the boundary renders only the frozen participant projection.
  std::vector<OperationParticipant> participants;
  TimePoint created_at{};
  TimePoint updated_at{};
  std::optional<TimePoint> retain_until;
};
enum class LifecycleState { Active, Disabled, Deleting, Deleted, Restoring, Purged };
// P13a durable deletion intent/tombstone. The resource row keeps the stable id and state; this
// relation records when the reversible delete happened, when restore stops being allowed and when
// the minimal tombstone may be purged. All time values are database-UTC samples.
struct LifecycleTombstone {
  ResourceRef resource;
  std::string operation_id;
  LifecycleState state = LifecycleState::Deleting;
  TimePoint deleted_at{};
  TimePoint restore_until{};
  TimePoint purge_after{};
};
// Durable management intent, distinct from lifecycle and observed process state. A stopped plugin
// can remain registered/connected; an offline host never proves its process was stopped.
enum class DesiredState { Running, Stopped };

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
  DesiredState desired = DesiredState::Running;
};

} // namespace kiarabot::core::target
