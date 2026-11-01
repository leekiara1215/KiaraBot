#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace kiarabot::core::target {

// Same value-or-error convention as core::Result, without the legacy store dependency.
template <typename Value, typename Error> using Result = std::variant<Value, Error>;
using TimePoint = std::chrono::system_clock::time_point;

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
struct SubjectTag;
using ResourceId = Identifier<ResourceTag>;
using InstallationId = Identifier<InstallationTag>;
using InstanceId = Identifier<InstanceTag>;
using ExecutionId = Identifier<ExecutionTag>;
using ConnectionId = Identifier<ConnectionTag>;
using OperationId = Identifier<OperationTag>;
using MessageId = Identifier<MessageTag>;
using AttemptId = Identifier<AttemptTag>;
using SubjectId = Identifier<SubjectTag>;

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
struct Fence {
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
  std::optional<OperationId> operation;
  std::optional<std::chrono::milliseconds> retry_after;
};
template <typename Value> using Outcome = Result<Value, Failure>;
using Status = Outcome<std::monostate>;

enum class OperationState { Accepted, Running, Blocked, Succeeded, Failed, Cancelled, Expired };
enum class ExecutionOutcome { NotStarted, Known, Unknown };
struct Operation {
  OperationId id;
  ResourceId resource;
  ResourceVersion version;
  OperationState state = OperationState::Accepted;
  ExecutionOutcome execution_outcome = ExecutionOutcome::NotStarted;
  std::optional<ErrorCode> blocked_reason;
  std::optional<TimePoint> next_attempt;
};
struct PluginInstance {
  InstanceId id;
  InstallationId installation;
  ResourceId plugin_type;
  ResourceVersion desired_version;
  ResourceVersion applied_config_version;
  std::optional<Fence> current;
};

} // namespace kiarabot::core::target
