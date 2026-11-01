#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <variant>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace codec = kiarabot::core::platform::target_codec;
namespace target = kiarabot::core::target;
namespace wire = kiarabot::v1;
namespace {
static_assert(noexcept(codec::to_wire(std::declval<target::TimePoint>())));
static_assert(noexcept(codec::from_wire(std::declval<const wire::ResourceRef&>())));
int failures = 0;
void expect(bool condition) {
  if (!condition) {
    ++failures;
    std::cerr << "codec expectation failed\n";
  }
}
template <typename T> bool rejected(const target::Outcome<T>& result) {
  const auto* failure = std::get_if<target::Failure>(&result);
  return failure != nullptr && failure->code == target::ErrorCode::InvalidArgument;
}
// Read a future enum value through the actual generated parser, rather than suppressing
// EnumCastOutOfRange diagnostics for intentional negative fixtures.
int unknown_value() {
  wire::ResourceRef resource;
  const std::string bytes{"\x08\xe7\x07", 3};
  expect(resource.ParseFromString(bytes));
  return static_cast<int>(resource.kind());
}
void test_permissions() {
  const std::array permissions{
      std::pair{target::Permission::AccountManage, wire::PERMISSION_ACCOUNT_MANAGE},
      std::pair{target::Permission::AccountPolicyManage, wire::PERMISSION_ACCOUNT_POLICY_MANAGE},
      std::pair{target::Permission::AccountPolicyRead, wire::PERMISSION_ACCOUNT_POLICY_READ},
      std::pair{target::Permission::ActionExecute, wire::PERMISSION_ACTION_EXECUTE},
      std::pair{target::Permission::ActionInspect, wire::PERMISSION_ACTION_INSPECT},
      std::pair{target::Permission::AdapterConnect, wire::PERMISSION_ADAPTER_CONNECT},
      std::pair{target::Permission::DeliveryManage, wire::PERMISSION_DELIVERY_MANAGE},
      std::pair{target::Permission::DeliveryProcess, wire::PERMISSION_DELIVERY_PROCESS},
      std::pair{target::Permission::HostExecute, wire::PERMISSION_HOST_EXECUTE},
      std::pair{target::Permission::HostRead, wire::PERMISSION_HOST_READ},
      std::pair{target::Permission::HostRegister, wire::PERMISSION_HOST_REGISTER},
      std::pair{target::Permission::IdentityAssignmentManage,
                wire::PERMISSION_IDENTITY_ASSIGNMENT_MANAGE},
      std::pair{target::Permission::IdentityAssignmentRead,
                wire::PERMISSION_IDENTITY_ASSIGNMENT_READ},
      std::pair{target::Permission::IdentityAuthorizationCheck,
                wire::PERMISSION_IDENTITY_AUTHORIZATION_CHECK},
      std::pair{target::Permission::IdentityDelegationIssue,
                wire::PERMISSION_IDENTITY_DELEGATION_ISSUE},
      std::pair{target::Permission::IdentityInvalidationRead,
                wire::PERMISSION_IDENTITY_INVALIDATION_READ},
      std::pair{target::Permission::IdentityKeysRead, wire::PERMISSION_IDENTITY_KEYS_READ},
      std::pair{target::Permission::IdentityPrincipalManage,
                wire::PERMISSION_IDENTITY_PRINCIPAL_MANAGE},
      std::pair{target::Permission::IdentityPrincipalRead,
                wire::PERMISSION_IDENTITY_PRINCIPAL_READ},
      std::pair{target::Permission::IdentitySessionCreate,
                wire::PERMISSION_IDENTITY_SESSION_CREATE},
      std::pair{target::Permission::IdentitySessionRead, wire::PERMISSION_IDENTITY_SESSION_READ},
      std::pair{target::Permission::IdentitySessionRevoke,
                wire::PERMISSION_IDENTITY_SESSION_REVOKE},
      std::pair{target::Permission::ModelCallInspect, wire::PERMISSION_MODEL_CALL_INSPECT},
      std::pair{target::Permission::ModelChat, wire::PERMISSION_MODEL_CHAT},
      std::pair{target::Permission::ModelManage, wire::PERMISSION_MODEL_MANAGE},
      std::pair{target::Permission::ModelRead, wire::PERMISSION_MODEL_READ},
      std::pair{target::Permission::NamespaceManage, wire::PERMISSION_NAMESPACE_MANAGE},
      std::pair{target::Permission::PluginConnect, wire::PERMISSION_PLUGIN_CONNECT},
      std::pair{target::Permission::PluginManage, wire::PERMISSION_PLUGIN_MANAGE},
      std::pair{target::Permission::ResourceManage, wire::PERMISSION_RESOURCE_MANAGE},
      std::pair{target::Permission::ResourceRead, wire::PERMISSION_RESOURCE_READ},
      std::pair{target::Permission::StorageNamespace, wire::PERMISSION_STORAGE_NAMESPACE},
      std::pair{target::Permission::TaskRead, wire::PERMISSION_TASK_READ},
      std::pair{target::Permission::TaskSchedule, wire::PERMISSION_TASK_SCHEDULE},
      std::pair{target::Permission::TimerAccept, wire::PERMISSION_TIMER_ACCEPT},
      std::pair{target::Permission::AccountRead, wire::PERMISSION_ACCOUNT_READ},
      std::pair{target::Permission::PluginRead, wire::PERMISSION_PLUGIN_READ},
      std::pair{target::Permission::AdapterRead, wire::PERMISSION_ADAPTER_READ},
      std::pair{target::Permission::PluginConfigRead, wire::PERMISSION_PLUGIN_CONFIG_READ},
      std::pair{target::Permission::MessageRead, wire::PERMISSION_MESSAGE_READ}};
  expect(permissions.size() ==
         static_cast<std::size_t>(wire::Permission_descriptor()->value_count() - 1));
  for (const auto& [internal, external] : permissions) {
    expect(std::get<wire::Permission>(codec::to_wire(internal)) == external);
    expect(std::get<target::Permission>(codec::from_wire(external)) == internal);
  }
  expect(rejected(codec::to_wire(target::Permission::Unspecified)));
  expect(rejected(codec::from_wire(wire::PERMISSION_UNSPECIFIED)));
  expect(rejected(codec::to_wire(static_cast<target::Permission>(unknown_value()))));
  expect(rejected(codec::from_wire(static_cast<wire::Permission>(unknown_value()))));
}
void test_resources() {
  constexpr std::array kinds{
      std::pair{target::ResourceKind::Account, wire::RESOURCE_ACCOUNT},
      std::pair{target::ResourceKind::Adapter, wire::RESOURCE_ADAPTER},
      std::pair{target::ResourceKind::PluginType, wire::RESOURCE_PLUGIN_TYPE},
      std::pair{target::ResourceKind::Installation, wire::RESOURCE_INSTALLATION},
      std::pair{target::ResourceKind::Instance, wire::RESOURCE_INSTANCE},
      std::pair{target::ResourceKind::Host, wire::RESOURCE_HOST},
      std::pair{target::ResourceKind::Namespace, wire::RESOURCE_NAMESPACE},
      std::pair{target::ResourceKind::Task, wire::RESOURCE_TASK},
      std::pair{target::ResourceKind::Model, wire::RESOURCE_MODEL},
      std::pair{target::ResourceKind::Principal, wire::RESOURCE_PRINCIPAL},
      std::pair{target::ResourceKind::Session, wire::RESOURCE_SESSION},
      std::pair{target::ResourceKind::Action, wire::RESOURCE_ACTION},
      std::pair{target::ResourceKind::Operation, wire::RESOURCE_OPERATION},
      std::pair{target::ResourceKind::Credential, wire::RESOURCE_CREDENTIAL},
      std::pair{target::ResourceKind::Assignment, wire::RESOURCE_ASSIGNMENT},
      std::pair{target::ResourceKind::Catalog, wire::RESOURCE_CATALOG}};
  expect(kinds.size() ==
         static_cast<std::size_t>(wire::ResourceKind_descriptor()->value_count() - 1));
  for (const auto& [internal, external] : kinds) {
    expect(std::get<wire::ResourceKind>(codec::to_wire(internal)) == external);
    expect(std::get<target::ResourceKind>(codec::from_wire(external)) == internal);
    target::ResourceRef resource{internal,
                                 target::ResourceId{"11111111-1111-4111-8111-111111111111"}};
    const auto encoded = std::get<wire::ResourceRef>(codec::to_wire(resource));
    expect(std::get<target::ResourceRef>(codec::from_wire(encoded)) == resource);
    resource.id.value.clear();
    expect(rejected(codec::to_wire(resource)));
  }
  expect(static_cast<int>(target::ResourceKind::Catalog) == 15);
  expect(wire::RESOURCE_CATALOG == 16);
  expect(rejected(codec::from_wire(wire::RESOURCE_KIND_UNSPECIFIED)));
  expect(rejected(codec::from_wire(static_cast<wire::ResourceKind>(unknown_value()))));
  expect(rejected(codec::to_wire(static_cast<target::ResourceKind>(unknown_value()))));
  expect(rejected(codec::from_wire(wire::ResourceRef{})));
  for (const auto* bad :
       {"all", "11111111-1111-1111-8111-111111111111", "11111111-1111-4111-7111-111111111111",
        "AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA", "11111111-1111-4111-8111-11111111111Z"}) {
    expect(rejected(codec::to_wire(
        target::ResourceRef{target::ResourceKind::Catalog, target::ResourceId{bad}})));
  }
  expect(!rejected(codec::to_wire(
      target::ResourceRef{target::ResourceKind::Action, target::ResourceId{"action_1-2.3"}})));
  expect(rejected(codec::to_wire(
      target::ResourceRef{target::ResourceKind::Action, target::ResourceId{"action:bad"}})));
  expect(rejected(codec::to_wire(target::ResourceRef{target::ResourceKind::Action,
                                                     target::ResourceId{std::string(129, 'a')}})));
  expect(!rejected(codec::to_wire(
      target::ResourceRef{target::ResourceKind::Task, target::ResourceId{"task:source/1"}})));
  expect(rejected(codec::to_wire(
      target::ResourceRef{target::ResourceKind::Task, target::ResourceId{"task space"}})));
  expect(rejected(codec::to_wire(
      target::ResourceRef{target::ResourceKind::Task, target::ResourceId{std::string(257, 'a')}})));
}
void test_time() {
  for (const std::int64_t count :
       {std::numeric_limits<std::int64_t>::min(), -1'000'000'001LL, -1LL, 0LL, 1LL, 1'000'000'001LL,
        std::numeric_limits<std::int64_t>::max()}) {
    const target::TimePoint time{std::chrono::nanoseconds{count}};
    const auto encoded = std::get<google::protobuf::Timestamp>(codec::to_wire(time));
    expect(encoded.nanos() >= 0 && encoded.nanos() < 1'000'000'000);
    expect(std::get<target::TimePoint>(codec::from_wire(encoded)) == time);
  }
  google::protobuf::Timestamp timestamp;
  timestamp.set_seconds(-1);
  timestamp.set_nanos(999'999'999);
  expect(std::get<target::TimePoint>(codec::from_wire(timestamp)).time_since_epoch().count() == -1);
  for (const auto nanos : {-1, 1'000'000'000}) {
    timestamp.set_nanos(nanos);
    expect(rejected(codec::from_wire(timestamp)));
  }
  timestamp.set_seconds(-9'223'372'037LL);
  timestamp.set_nanos(145'224'191);
  expect(rejected(codec::from_wire(timestamp)));
  timestamp.set_seconds(9'223'372'036LL);
  timestamp.set_nanos(854'775'808);
  expect(rejected(codec::from_wire(timestamp)));
  for (const auto seconds :
       {-62'135'596'801LL, -62'135'596'800LL, 253'402'300'799LL, 253'402'300'800LL}) {
    timestamp.set_seconds(seconds);
    timestamp.set_nanos(0);
    expect(rejected(codec::from_wire(timestamp)));
  }
}
void test_errors() {
  struct ErrorCase {
    target::ErrorCode internal;
    wire::ErrorCode external;
    grpc::StatusCode grpc;
    int http;
  };
  constexpr std::array cases{
      ErrorCase{target::ErrorCode::InvalidArgument, wire::INVALID_ARGUMENT,
                grpc::StatusCode::INVALID_ARGUMENT, 400},
      ErrorCase{target::ErrorCode::Unauthenticated, wire::UNAUTHENTICATED,
                grpc::StatusCode::UNAUTHENTICATED, 401},
      ErrorCase{target::ErrorCode::PermissionDenied, wire::PERMISSION_DENIED,
                grpc::StatusCode::PERMISSION_DENIED, 403},
      ErrorCase{target::ErrorCode::NotFound, wire::NOT_FOUND, grpc::StatusCode::NOT_FOUND, 404},
      ErrorCase{target::ErrorCode::Conflict, wire::CONFLICT, grpc::StatusCode::ABORTED, 409},
      ErrorCase{target::ErrorCode::RateLimited, wire::RATE_LIMITED,
                grpc::StatusCode::RESOURCE_EXHAUSTED, 429},
      ErrorCase{target::ErrorCode::Timeout, wire::TIMEOUT, grpc::StatusCode::DEADLINE_EXCEEDED,
                504},
      ErrorCase{target::ErrorCode::UpstreamUnavailable, wire::UPSTREAM_UNAVAILABLE,
                grpc::StatusCode::UNAVAILABLE, 503},
      ErrorCase{target::ErrorCode::PluginUnavailable, wire::PLUGIN_UNAVAILABLE,
                grpc::StatusCode::UNAVAILABLE, 503},
      ErrorCase{target::ErrorCode::CapabilityNotSupported, wire::CAPABILITY_NOT_SUPPORTED,
                grpc::StatusCode::UNIMPLEMENTED, 503},
      ErrorCase{target::ErrorCode::Internal, wire::INTERNAL, grpc::StatusCode::INTERNAL, 500}};
  expect(cases.size() == static_cast<std::size_t>(wire::ErrorCode_descriptor()->value_count() - 1));
  for (const auto& test : cases) {
    expect(std::get<wire::ErrorCode>(codec::to_wire(test.internal)) == test.external);
    expect(std::get<target::ErrorCode>(codec::from_wire(test.external)) == test.internal);
    expect(std::get<grpc::StatusCode>(codec::grpc_status(test.internal)) == test.grpc);
    expect(std::get<int>(codec::http_status(test.internal)) == test.http);
    const auto error =
        std::get<wire::Error>(codec::make_error(test.internal, "request-1", "trace_2", true));
    const auto status = std::get<grpc::Status>(codec::to_grpc_error(error));
    expect(status.error_code() == test.grpc);
    wire::Error detail;
    expect(detail.ParseFromString(status.error_details()));
    expect(detail.code() == test.external && detail.request_id() == "request-1" &&
           detail.trace_id() == "trace_2" && detail.retryable());
    expect(std::get<wire::Error>(codec::from_grpc_error(status)).SerializeAsString() ==
           error.SerializeAsString());
    expect(std::get<wire::Error>(codec::from_http_error(test.http, error)).code() == test.external);
    expect(rejected(codec::from_http_error(200, error)));
    expect(rejected(codec::from_grpc_error(
        grpc::Status{grpc::StatusCode::UNKNOWN, "free text", status.error_details()})));
    expect(rejected(
        codec::from_grpc_error(grpc::Status{test.grpc, "free text without stable details"})));
    expect(rejected(codec::from_grpc_error(grpc::Status{test.grpc, "free text", "malformed"})));
    expect(std::get<wire::Error>(codec::from_grpc_error(grpc::Status{test.grpc, "ignored text",
                                                                     status.error_details()}))
               .code() == test.external);
  }
  expect(rejected(codec::from_wire(wire::ERROR_CODE_UNSPECIFIED)));
  expect(rejected(codec::from_wire(static_cast<wire::ErrorCode>(unknown_value()))));
  expect(rejected(codec::to_wire(static_cast<target::ErrorCode>(unknown_value()))));
  expect(rejected(codec::http_status(static_cast<target::ErrorCode>(unknown_value()))));
  expect(rejected(codec::grpc_status(static_cast<target::ErrorCode>(unknown_value()))));
  expect(rejected(codec::from_grpc_error(grpc::Status::OK)));
  expect(rejected(codec::make_error(target::ErrorCode::Internal, "", "trace", false)));
  expect(
      rejected(codec::make_error(target::ErrorCode::Internal, "request", "invalid trace", false)));
  auto error = std::get<wire::Error>(
      codec::make_error(target::ErrorCode::Internal, "request", "trace", false));
  (*error.mutable_details())["private"] = "forbidden";
  expect(rejected(codec::to_grpc_error(error)));
  error.clear_details();
  error.clear_request_id();
  expect(rejected(codec::to_grpc_error(error)));
}
} // namespace
int main() {
  expect(unknown_value() == 999);
  test_permissions();
  test_resources();
  test_time();
  test_errors();
  if (failures != 0) {
    return 1;
  }
  std::cout << "Target codec: all enum mappings, invalid values, UUID/source IDs, timestamp "
               "boundaries and stable binary errors passed\n";
  return 0;
}
