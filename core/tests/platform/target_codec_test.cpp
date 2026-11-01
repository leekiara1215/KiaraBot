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
    wire::RpcFailure failure;
    *failure.mutable_error() = error;
    failure.mutable_context()->set_acceptance(wire::NOT_ACCEPTED);
    failure.mutable_context()->set_recovery(wire::CORRECT_INPUT);
    const auto status = std::get<grpc::Status>(codec::to_grpc_failure(failure));
    expect(status.error_code() == test.grpc);
    wire::RpcFailure detail;
    expect(detail.ParseFromString(status.error_details()));
    expect(detail.error().code() == test.external && detail.error().request_id() == "request-1" &&
           detail.error().trace_id() == "trace_2" && detail.error().retryable());
    expect(std::get<wire::RpcFailure>(codec::from_grpc_failure(status)).SerializeAsString() ==
           failure.SerializeAsString());
    expect(std::get<wire::RpcFailure>(codec::from_http_failure(test.http, failure))
               .SerializeAsString() == failure.SerializeAsString());
    expect(rejected(codec::from_http_failure(200, failure)));
    expect(std::get<wire::Error>(codec::from_http_error(test.http, error)).code() == test.external);
    expect(rejected(codec::from_http_error(200, error)));
    expect(rejected(codec::from_grpc_failure(
        grpc::Status{grpc::StatusCode::UNKNOWN, "text", status.error_details()})));
    expect(rejected(codec::from_grpc_failure(grpc::Status{test.grpc, "no detail"})));
    expect(rejected(codec::from_grpc_failure(grpc::Status{test.grpc, "text", "malformed"})));
    expect(rejected(codec::from_grpc_failure(
        grpc::Status{test.grpc, "reference Error-only", error.SerializeAsString()})));
    expect(std::get<wire::RpcFailure>(
               codec::from_grpc_failure(grpc::Status{test.grpc, "ignored", status.error_details()}))
               .error()
               .code() == test.external);
  }
  expect(rejected(codec::from_wire(wire::ERROR_CODE_UNSPECIFIED)));
  expect(rejected(codec::from_wire(static_cast<wire::ErrorCode>(unknown_value()))));
  expect(rejected(codec::to_wire(static_cast<target::ErrorCode>(unknown_value()))));
  expect(rejected(codec::http_status(static_cast<target::ErrorCode>(unknown_value()))));
  expect(rejected(codec::grpc_status(static_cast<target::ErrorCode>(unknown_value()))));
  expect(rejected(codec::from_grpc_failure(grpc::Status::OK)));
  expect(rejected(codec::make_error(target::ErrorCode::Internal, "", "trace", false)));
  expect(
      rejected(codec::make_error(target::ErrorCode::Internal, "request", "invalid trace", false)));
  auto error = std::get<wire::Error>(
      codec::make_error(target::ErrorCode::Internal, "request", "trace", false));
  (*error.mutable_details())["private"] = "forbidden";
  expect(rejected(codec::from_http_error(500, error)));
  error.clear_details();
  error.clear_request_id();
  expect(rejected(codec::from_http_error(500, error)));
}
void test_failure_context() {
  wire::RpcFailure failure;
  *failure.mutable_error() = std::get<wire::Error>(
      codec::make_error(target::ErrorCode::Timeout, "request", "trace", true));
  auto* context = failure.mutable_context();
  context->set_acceptance(wire::ACCEPTANCE_UNKNOWN);
  context->set_recovery(wire::QUERY_SAME_INTENT);
  context->set_retry_after_ms(1234);
  context->set_current_version(std::numeric_limits<std::uint64_t>::max());
  auto* operation = context->mutable_operation();
  operation->set_owner_service("model");
  operation->set_operation_id("11111111-1111-4111-8111-111111111111");
  operation->mutable_resource()->set_kind(wire::RESOURCE_MODEL);
  operation->mutable_resource()->set_id("22222222-2222-4222-8222-222222222222");
  const auto verify = [](const wire::RpcFailure& input) {
    const auto status = std::get<grpc::Status>(codec::to_grpc_failure(input));
    expect(std::get<wire::RpcFailure>(codec::from_grpc_failure(status)).SerializeAsString() ==
           input.SerializeAsString());
    expect(std::get<wire::RpcFailure>(codec::from_http_failure(504, input)).SerializeAsString() ==
           input.SerializeAsString());
  };
  verify(failure);
  for (const auto acceptance :
       {wire::NOT_ACCEPTED, wire::DURABLY_ACCEPTED, wire::ACCEPTANCE_UNKNOWN}) {
    context->set_acceptance(acceptance);
    for (const auto recovery :
         {wire::CORRECT_INPUT, wire::REAUTHENTICATE, wire::DO_NOT_RETRY, wire::QUERY_SAME_INTENT,
          wire::RETRY_SAME_ID, wire::RESYNC_SNAPSHOT, wire::MANUAL_REVIEW}) {
      context->set_recovery(recovery);
      verify(failure);
    }
  }
  const auto reject = [](const wire::RpcFailure& input) {
    expect(rejected(codec::to_grpc_failure(input)));
    expect(rejected(codec::from_grpc_failure(
        grpc::Status{grpc::StatusCode::DEADLINE_EXCEEDED, "ignored", input.SerializeAsString()})));
    expect(rejected(codec::from_http_failure(504, input)));
  };
  auto bad = failure;
  bad.clear_context();
  reject(bad);
  bad = failure;
  bad.clear_error();
  reject(bad);
  bad = failure;
  bad.mutable_context()->set_acceptance(wire::ACCEPTANCE_UNSPECIFIED);
  reject(bad);
  bad = failure;
  bad.mutable_context()->set_acceptance(static_cast<wire::AcceptanceState>(unknown_value()));
  reject(bad);
  bad = failure;
  bad.mutable_context()->set_recovery(wire::RECOVERY_UNSPECIFIED);
  reject(bad);
  bad = failure;
  bad.mutable_context()->set_recovery(static_cast<wire::RecoveryAction>(unknown_value()));
  reject(bad);
  bad = failure;
  bad.mutable_error()->set_code(static_cast<wire::ErrorCode>(unknown_value()));
  reject(bad);
  bad = failure;
  bad.mutable_context()->set_current_version(0);
  reject(bad);
  bad = failure;
  bad.mutable_context()->mutable_operation()->set_owner_service("unknown");
  reject(bad);
  bad = failure;
  bad.mutable_context()->mutable_operation()->set_operation_id("not-uuid");
  reject(bad);
  bad = failure;
  bad.mutable_context()->mutable_operation()->clear_resource();
  reject(bad);
  bad = failure;
  bad.mutable_context()->mutable_operation()->mutable_resource()->set_kind(
      wire::RESOURCE_KIND_UNSPECIFIED);
  reject(bad);
  bad = failure;
  bad.mutable_context()->mutable_operation()->mutable_resource()->clear_id();
  reject(bad);
  // Login/refresh unknown outcome can legitimately have no operation reference or version.
  failure.mutable_context()->clear_operation();
  failure.mutable_context()->clear_current_version();
  failure.mutable_context()->set_acceptance(wire::ACCEPTANCE_UNKNOWN);
  failure.mutable_context()->set_recovery(wire::QUERY_SAME_INTENT);
  verify(failure);
}

} // namespace
int main() {
  expect(unknown_value() == 999);
  test_permissions();
  test_resources();
  test_time();
  test_errors();
  test_failure_context();
  if (failures != 0) {
    return 1;
  }
  std::cout << "Target codec: all enum mappings, invalid values, UUID/source IDs, timestamp "
               "boundaries and stable binary errors passed\n";
  return 0;
}
