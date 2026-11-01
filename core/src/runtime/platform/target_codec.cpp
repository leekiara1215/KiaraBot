#include "kiarabot/core/runtime/platform/target_codec.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace kiarabot::core::platform::target_codec {
namespace {

target::Failure invalid() {
  return {target::ErrorCode::InvalidArgument, target::Acceptance::NotAccepted,
          target::Recovery::CorrectInput, std::nullopt, std::nullopt};
}

template <typename Internal, typename Wire, std::size_t Size>
target::Outcome<Wire> encode(Internal value,
                             const std::array<std::pair<Internal, Wire>, Size>& table) {
  for (const auto& [internal, wire] : table) {
    if (value == internal) {
      return wire;
    }
  }
  return invalid();
}

template <typename Internal, typename Wire, std::size_t Size>
target::Outcome<Internal> decode(Wire value,
                                 const std::array<std::pair<Internal, Wire>, Size>& table) {
  for (const auto& [internal, wire] : table) {
    if (value == wire) {
      return internal;
    }
  }
  return invalid();
}

constexpr std::array kResources{
    std::pair{target::ResourceKind::Account, v1::RESOURCE_ACCOUNT},
    std::pair{target::ResourceKind::Adapter, v1::RESOURCE_ADAPTER},
    std::pair{target::ResourceKind::PluginType, v1::RESOURCE_PLUGIN_TYPE},
    std::pair{target::ResourceKind::Installation, v1::RESOURCE_INSTALLATION},
    std::pair{target::ResourceKind::Instance, v1::RESOURCE_INSTANCE},
    std::pair{target::ResourceKind::Host, v1::RESOURCE_HOST},
    std::pair{target::ResourceKind::Namespace, v1::RESOURCE_NAMESPACE},
    std::pair{target::ResourceKind::Task, v1::RESOURCE_TASK},
    std::pair{target::ResourceKind::Model, v1::RESOURCE_MODEL},
    std::pair{target::ResourceKind::Principal, v1::RESOURCE_PRINCIPAL},
    std::pair{target::ResourceKind::Session, v1::RESOURCE_SESSION},
    std::pair{target::ResourceKind::Action, v1::RESOURCE_ACTION},
    std::pair{target::ResourceKind::Operation, v1::RESOURCE_OPERATION},
    std::pair{target::ResourceKind::Credential, v1::RESOURCE_CREDENTIAL},
    std::pair{target::ResourceKind::Assignment, v1::RESOURCE_ASSIGNMENT},
    std::pair{target::ResourceKind::Catalog, v1::RESOURCE_CATALOG}};

constexpr std::array kPermissions{
    std::pair{target::Permission::AccountManage, v1::PERMISSION_ACCOUNT_MANAGE},
    std::pair{target::Permission::AccountPolicyManage, v1::PERMISSION_ACCOUNT_POLICY_MANAGE},
    std::pair{target::Permission::AccountPolicyRead, v1::PERMISSION_ACCOUNT_POLICY_READ},
    std::pair{target::Permission::ActionExecute, v1::PERMISSION_ACTION_EXECUTE},
    std::pair{target::Permission::ActionInspect, v1::PERMISSION_ACTION_INSPECT},
    std::pair{target::Permission::AdapterConnect, v1::PERMISSION_ADAPTER_CONNECT},
    std::pair{target::Permission::DeliveryManage, v1::PERMISSION_DELIVERY_MANAGE},
    std::pair{target::Permission::DeliveryProcess, v1::PERMISSION_DELIVERY_PROCESS},
    std::pair{target::Permission::HostExecute, v1::PERMISSION_HOST_EXECUTE},
    std::pair{target::Permission::HostRead, v1::PERMISSION_HOST_READ},
    std::pair{target::Permission::HostRegister, v1::PERMISSION_HOST_REGISTER},
    std::pair{target::Permission::IdentityAssignmentManage,
              v1::PERMISSION_IDENTITY_ASSIGNMENT_MANAGE},
    std::pair{target::Permission::IdentityAssignmentRead, v1::PERMISSION_IDENTITY_ASSIGNMENT_READ},
    std::pair{target::Permission::IdentityAuthorizationCheck,
              v1::PERMISSION_IDENTITY_AUTHORIZATION_CHECK},
    std::pair{target::Permission::IdentityDelegationIssue,
              v1::PERMISSION_IDENTITY_DELEGATION_ISSUE},
    std::pair{target::Permission::IdentityInvalidationRead,
              v1::PERMISSION_IDENTITY_INVALIDATION_READ},
    std::pair{target::Permission::IdentityKeysRead, v1::PERMISSION_IDENTITY_KEYS_READ},
    std::pair{target::Permission::IdentityPrincipalManage,
              v1::PERMISSION_IDENTITY_PRINCIPAL_MANAGE},
    std::pair{target::Permission::IdentityPrincipalRead, v1::PERMISSION_IDENTITY_PRINCIPAL_READ},
    std::pair{target::Permission::IdentitySessionCreate, v1::PERMISSION_IDENTITY_SESSION_CREATE},
    std::pair{target::Permission::IdentitySessionRead, v1::PERMISSION_IDENTITY_SESSION_READ},
    std::pair{target::Permission::IdentitySessionRevoke, v1::PERMISSION_IDENTITY_SESSION_REVOKE},
    std::pair{target::Permission::ModelCallInspect, v1::PERMISSION_MODEL_CALL_INSPECT},
    std::pair{target::Permission::ModelChat, v1::PERMISSION_MODEL_CHAT},
    std::pair{target::Permission::ModelManage, v1::PERMISSION_MODEL_MANAGE},
    std::pair{target::Permission::ModelRead, v1::PERMISSION_MODEL_READ},
    std::pair{target::Permission::NamespaceManage, v1::PERMISSION_NAMESPACE_MANAGE},
    std::pair{target::Permission::PluginConnect, v1::PERMISSION_PLUGIN_CONNECT},
    std::pair{target::Permission::PluginManage, v1::PERMISSION_PLUGIN_MANAGE},
    std::pair{target::Permission::ResourceManage, v1::PERMISSION_RESOURCE_MANAGE},
    std::pair{target::Permission::ResourceRead, v1::PERMISSION_RESOURCE_READ},
    std::pair{target::Permission::StorageNamespace, v1::PERMISSION_STORAGE_NAMESPACE},
    std::pair{target::Permission::TaskRead, v1::PERMISSION_TASK_READ},
    std::pair{target::Permission::TaskSchedule, v1::PERMISSION_TASK_SCHEDULE},
    std::pair{target::Permission::TimerAccept, v1::PERMISSION_TIMER_ACCEPT},
    std::pair{target::Permission::AccountRead, v1::PERMISSION_ACCOUNT_READ},
    std::pair{target::Permission::PluginRead, v1::PERMISSION_PLUGIN_READ},
    std::pair{target::Permission::AdapterRead, v1::PERMISSION_ADAPTER_READ},
    std::pair{target::Permission::PluginConfigRead, v1::PERMISSION_PLUGIN_CONFIG_READ},
    std::pair{target::Permission::MessageRead, v1::PERMISSION_MESSAGE_READ}};

struct ErrorMapping {
  target::ErrorCode internal;
  v1::ErrorCode wire;
  grpc::StatusCode grpc;
  int http;
};
constexpr std::array kErrors{
    ErrorMapping{target::ErrorCode::InvalidArgument, v1::INVALID_ARGUMENT,
                 grpc::StatusCode::INVALID_ARGUMENT, 400},
    ErrorMapping{target::ErrorCode::Unauthenticated, v1::UNAUTHENTICATED,
                 grpc::StatusCode::UNAUTHENTICATED, 401},
    ErrorMapping{target::ErrorCode::PermissionDenied, v1::PERMISSION_DENIED,
                 grpc::StatusCode::PERMISSION_DENIED, 403},
    ErrorMapping{target::ErrorCode::NotFound, v1::NOT_FOUND, grpc::StatusCode::NOT_FOUND, 404},
    ErrorMapping{target::ErrorCode::Conflict, v1::CONFLICT, grpc::StatusCode::ABORTED, 409},
    ErrorMapping{target::ErrorCode::RateLimited, v1::RATE_LIMITED,
                 grpc::StatusCode::RESOURCE_EXHAUSTED, 429},
    ErrorMapping{target::ErrorCode::Timeout, v1::TIMEOUT, grpc::StatusCode::DEADLINE_EXCEEDED, 504},
    ErrorMapping{target::ErrorCode::UpstreamUnavailable, v1::UPSTREAM_UNAVAILABLE,
                 grpc::StatusCode::UNAVAILABLE, 503},
    ErrorMapping{target::ErrorCode::PluginUnavailable, v1::PLUGIN_UNAVAILABLE,
                 grpc::StatusCode::UNAVAILABLE, 503},
    ErrorMapping{target::ErrorCode::CapabilityNotSupported, v1::CAPABILITY_NOT_SUPPORTED,
                 grpc::StatusCode::UNIMPLEMENTED, 503},
    ErrorMapping{target::ErrorCode::Internal, v1::INTERNAL, grpc::StatusCode::INTERNAL, 500}};

bool correlation_id(std::string_view value) {
  if (value.empty() || value.size() > 128) {
    return false;
  }
  for (char character : value) {
    if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '.' || character == '_' ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

bool uuid_v4(std::string_view value) {
  if (value.size() != 36 || value[14] != '4' ||
      (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b')) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool resource_id(target::ResourceKind kind, std::string_view value) {
  if (kind == target::ResourceKind::Action) {
    return correlation_id(value);
  }
  if (kind == target::ResourceKind::Task) {
    if (value.empty() || value.size() > 256) {
      return false;
    }
    for (char character : value) {
      if (character < '!' || character > '~') {
        return false;
      }
    }
    return true;
  }
  return uuid_v4(value);
}

bool valid_error(const v1::Error& error) {
  return std::holds_alternative<target::ErrorCode>(from_wire(error.code())) &&
         correlation_id(error.request_id()) && correlation_id(error.trace_id()) &&
         !error.message().empty() && error.details().empty();
}
} // namespace

target::Outcome<google::protobuf::Timestamp> to_wire(target::TimePoint value) noexcept try {
  constexpr std::int64_t kBillion = 1'000'000'000;
  const auto count = value.time_since_epoch().count();
  auto seconds = count / kBillion;
  auto nanos = count % kBillion;
  if (nanos < 0) {
    --seconds;
    nanos += kBillion;
  }
  google::protobuf::Timestamp result;
  result.set_seconds(seconds);
  result.set_nanos(static_cast<std::int32_t>(nanos));
  return result;
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<target::TimePoint> from_wire(const google::protobuf::Timestamp& value) noexcept
    try {
  constexpr std::int64_t kBillion = 1'000'000'000;
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMinSeconds = kMin / kBillion - 1;
  constexpr auto kMinNanos = kMin % kBillion + kBillion;
  const auto seconds = value.seconds();
  const auto nanos = value.nanos();
  if (seconds < -62'135'596'800LL || seconds > 253'402'300'799LL || nanos < 0 ||
      nanos >= kBillion || seconds < kMinSeconds || seconds > kMax / kBillion ||
      (seconds == kMinSeconds && nanos < kMinNanos) ||
      (seconds == kMax / kBillion && nanos > kMax % kBillion)) {
    return invalid();
  }
  const auto count =
      seconds == kMinSeconds ? kMin + (nanos - kMinNanos) : seconds * kBillion + nanos;
  return target::TimePoint{std::chrono::nanoseconds{count}};
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<v1::ResourceKind> to_wire(target::ResourceKind value) noexcept try {
  return encode(value, kResources);
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}
target::Outcome<target::ResourceKind> from_wire(v1::ResourceKind value) noexcept try {
  return decode(value, kResources);
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}
target::Outcome<v1::Permission> to_wire(target::Permission value) noexcept try {
  return encode(value, kPermissions);
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}
target::Outcome<target::Permission> from_wire(v1::Permission value) noexcept try {
  return decode(value, kPermissions);
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<v1::ResourceRef> to_wire(const target::ResourceRef& value) noexcept try {
  const auto kind = to_wire(value.kind);
  if (std::holds_alternative<target::Failure>(kind) || !resource_id(value.kind, value.id.value)) {
    return invalid();
  }
  v1::ResourceRef result;
  result.set_kind(std::get<v1::ResourceKind>(kind));
  result.set_id(value.id.value);
  return result;
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<target::ResourceRef> from_wire(const v1::ResourceRef& value) noexcept try {
  const auto kind = from_wire(value.kind());
  if (std::holds_alternative<target::Failure>(kind)) {
    return invalid();
  }
  const auto internal = std::get<target::ResourceKind>(kind);
  if (!resource_id(internal, value.id())) {
    return invalid();
  }
  return target::ResourceRef{internal, target::ResourceId{value.id()}};
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<v1::ErrorCode> to_wire(target::ErrorCode value) noexcept try {
  for (const auto& mapping : kErrors) {
    if (mapping.internal == value) {
      return mapping.wire;
    }
  }
  return invalid();
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}
target::Outcome<target::ErrorCode> from_wire(v1::ErrorCode value) noexcept try {
  for (const auto& mapping : kErrors) {
    if (mapping.wire == value) {
      return mapping.internal;
    }
  }
  return invalid();
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}
target::Outcome<int> http_status(target::ErrorCode value) noexcept try {
  for (const auto& mapping : kErrors) {
    if (mapping.internal == value) {
      return mapping.http;
    }
  }
  return invalid();
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}
target::Outcome<grpc::StatusCode> grpc_status(target::ErrorCode value) noexcept try {
  for (const auto& mapping : kErrors) {
    if (mapping.internal == value) {
      return mapping.grpc;
    }
  }
  return invalid();
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<v1::Error> make_error(target::ErrorCode code, std::string_view request_id,
                                      std::string_view trace_id, bool retryable) noexcept try {
  const auto wire = to_wire(code);
  if (std::holds_alternative<target::Failure>(wire) || !correlation_id(request_id) ||
      !correlation_id(trace_id)) {
    return invalid();
  }
  v1::Error error;
  error.set_code(std::get<v1::ErrorCode>(wire));
  error.set_message(v1::ErrorCode_Name(error.code()));
  error.set_request_id(std::string(request_id));
  error.set_trace_id(std::string(trace_id));
  error.set_retryable(retryable);
  return error;
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<grpc::Status> to_grpc_error(const v1::Error& error) noexcept try {
  if (!valid_error(error)) {
    return invalid();
  }
  const auto code = std::get<target::ErrorCode>(from_wire(error.code()));
  std::string details;
  if (!error.SerializeToString(&details)) {
    return invalid();
  }
  return grpc::Status{std::get<grpc::StatusCode>(grpc_status(code)),
                      v1::ErrorCode_Name(error.code()), details};
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<v1::Error> from_grpc_error(const grpc::Status& status) noexcept try {
  v1::Error error;
  if (status.ok() || status.error_details().empty() ||
      !error.ParseFromString(status.error_details()) || !valid_error(error)) {
    return invalid();
  }
  const auto code = std::get<target::ErrorCode>(from_wire(error.code()));
  if (status.error_code() != std::get<grpc::StatusCode>(grpc_status(code))) {
    return invalid();
  }
  return error;
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

target::Outcome<v1::Error> from_http_error(int status, const v1::Error& error) noexcept try {
  if (!valid_error(error)) {
    return invalid();
  }
  const auto code = std::get<target::ErrorCode>(from_wire(error.code()));
  if (status != std::get<int>(http_status(code))) {
    return invalid();
  }
  return error;
} catch (...) {
  return target::Failure{target::ErrorCode::Internal, target::Acceptance::NotAccepted,
                         target::Recovery::Diagnose, std::nullopt, std::nullopt};
}

} // namespace kiarabot::core::platform::target_codec
