#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "kiarabot/core/runtime/platform/identity_rpc.hpp"
#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/infrastructure/identity_crypto/peer_certificate.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace p = kiarabot::core::platform;
namespace w = kiarabot::v1;
constexpr char kSubject[] = "11111111-1111-4111-8111-111111111111";
constexpr char kSession[] = "22222222-2222-4222-8222-222222222222";
constexpr char kPeer[] = "33333333-3333-4333-8333-333333333333";
constexpr char kCredential[] = "44444444-4444-4444-8444-444444444444";
constexpr char kAssignment[] = "55555555-5555-4555-8555-555555555555";
constexpr char kOther[] = "66666666-6666-4666-8666-666666666666";
const t::TimePoint kChecked{std::chrono::nanoseconds{1'793'491'200'123'456'789LL}};
std::atomic<unsigned int> failed_line{0};
void require(bool condition,
             const std::source_location& location = std::source_location::current()) {
  if (!condition) {
    failed_line.store(location.line());
    throw std::runtime_error("Identity RPC fixture assertion failed");
  }
}

std::string read(const char* path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>{stream}, {}};
}
// Insecure allocation is a TEST secret buffer only, never the production crypto adapter.
class FixtureSecret final : public t::SecretBuffer {
public:
  explicit FixtureSecret(std::string value) : bytes_(value.size()) {
    for (std::size_t i = 0; i < value.size(); ++i) {
      bytes_[i] = static_cast<std::byte>(value[i]);
    }
  }
  std::span<const std::byte> bytes() const noexcept override {
    return bytes_;
  }

private:
  std::vector<std::byte> bytes_;
};
bool same_secret(t::SecretView value, std::string_view expected) {
  return value.size() == expected.size() &&
         std::equal(value.begin(), value.end(),
                    std::as_bytes(std::span{expected.data(), expected.size()}).begin());
}
t::SessionMetadata metadata(bool revoked = false) {
  return {{kSession},
          {kSubject},
          kChecked - std::chrono::minutes{1},
          kChecked + std::chrono::minutes{15},
          std::numeric_limits<std::uint64_t>::max(),
          {std::numeric_limits<std::uint64_t>::max()},
          revoked};
}
t::Principal human() {
  return {{kSubject},
          t::PrincipalKind::Human,
          t::ResourceRef{t::ResourceKind::Principal, {kSubject}},
          {std::numeric_limits<std::uint64_t>::max()},
          t::LifecycleState::Active};
}
std::vector<t::ExactAssignment> assignments(int mode) {
  const t::ResourceRef catalog{t::ResourceKind::Catalog, {kOther}};
  const t::ResourceRef account{t::ResourceKind::Account, {kSubject}};
  t::ExactAssignment role{{kAssignment}, {kSubject}, {{t::FixedRole::Viewer, catalog}}, {}, {1}};
  t::ExactAssignment explicit_grant{
      {kOther}, {kSubject}, {}, {{t::Permission::MessageRead, account}}, {2}};
  if (mode == 21) {
    role.roles.push_back({t::FixedRole::Admin, account});
  }
  if (mode == 22) {
    explicit_grant.explicit_permissions.push_back({t::Permission::ActionInspect, catalog});
  }
  if (mode == 23) {
    explicit_grant.subject.value = kPeer;
  }
  return {role, explicit_grant};
}
class ApplicationFixture final : public t::IdentitySecurityApplication {
public:
  std::atomic<int> mode{0};
  std::atomic<int> calls{0};
  std::atomic<std::int64_t> last_deadline{0};
  std::atomic<int> code_index{0};
  std::atomic<int> recovery_index{5};
  t::ServiceReadiness readiness() const override {
    return {false, {{t::Dependency::BusinessImplementation, t::DependencyState::NotImplemented}}};
  }
  t::Failure configured_failure() const {
    constexpr std::array codes{
        t::ErrorCode::InvalidArgument,   t::ErrorCode::Unauthenticated,
        t::ErrorCode::PermissionDenied,  t::ErrorCode::NotFound,
        t::ErrorCode::Conflict,          t::ErrorCode::RateLimited,
        t::ErrorCode::Timeout,           t::ErrorCode::UpstreamUnavailable,
        t::ErrorCode::PluginUnavailable, t::ErrorCode::CapabilityNotSupported,
        t::ErrorCode::Internal};
    constexpr std::array recoveries{t::Recovery::CorrectInput, t::Recovery::Authenticate,
                                    t::Recovery::ReadCurrent,  t::Recovery::QuerySameIdentity,
                                    t::Recovery::Backoff,      t::Recovery::Diagnose};
    return {codes.at(static_cast<std::size_t>(code_index.load())), t::Acceptance::NotAccepted,
            recoveries.at(static_cast<std::size_t>(recovery_index.load())), std::nullopt,
            std::chrono::milliseconds{123}};
  }
  t::Failure unknown_failure() const {
    return {t::ErrorCode::Timeout, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity,
            t::OperationRef{t::Service::Model, {kOther}, {t::ResourceKind::Model, {kOther}}},
            std::chrono::milliseconds{321}};
  }
  template <typename Request> void observed(const Request& request) {
    ++calls;
    last_deadline.store(request.context.deadline.time_since_epoch().count());
    require(request.context.request_id == kSubject && request.context.trace_id == kOther);
    if (mode.load() == 12) {
      throw std::runtime_error("never-return-exception-secret");
    }
  }
  t::Outcome<t::IdentityTokenReply> tokens() {
    if (mode.load() == 1) {
      return configured_failure();
    }
    if (mode.load() == 14) {
      auto error = unknown_failure();
      error.acceptance = t::Acceptance::Accepted;
      error.recovery = t::Recovery::ReadCurrent;
      return error;
    }
    if (mode.load() == 2 || mode.load() == 13) {
      auto error = unknown_failure();
      if (mode.load() == 13) {
        error.recovery = t::Recovery::CorrectInput;
      }
      return error;
    }
    t::IdentityTokenReply result{{{kSession},
                                  std::make_unique<FixtureSecret>("access-fixture-secret"),
                                  std::make_unique<FixtureSecret>("refresh-fixture-secret"),
                                  kChecked + std::chrono::minutes{15},
                                  kChecked + std::chrono::hours{24},
                                  std::numeric_limits<std::uint64_t>::max()},
                                 kChecked};
    if (mode.load() == 31) {
      result.checked_at += std::chrono::seconds{901};
    }
    if (mode.load() == 20) {
      result.tokens.access.reset();
    }
    return result;
  }
  t::Outcome<t::IdentityTokenReply> login(const t::LoginRequest& request) override {
    observed(request);
    require(same_secret(request.password, "password-fixture-secret"));
    require(request.username == "human");
    const auto* peer = std::get_if<t::PlatformPeer>(&request.control_peer.binding.peer);
    require(peer && peer->subject.value == kPeer);
    return tokens();
  }
  t::Outcome<t::IdentityTokenReply>
  refresh_session(const t::RefreshSessionRequest& request) override {
    observed(request);
    require(same_secret(request.refresh_token, "refresh-fixture-secret"));
    return tokens();
  }
  t::Outcome<t::ValidatedSession>
  validate_session(const t::AccessSessionRequest& request) override {
    observed(request);
    require(same_secret(request.access_token, "access-fixture-secret"));
    if (mode.load() == 1) {
      return configured_failure();
    }
    if (mode.load() == 2) {
      return unknown_failure();
    }
    t::ValidatedSession result{human(),
                               metadata(),
                               assignments(mode.load()),
                               {std::numeric_limits<std::uint64_t>::max()},
                               kChecked};
    if (mode.load() == 28) {
      result.principal.binding.reset();
    }
    if (mode.load() == 24) {
      result.session.created_at = {};
    }
    if (mode.load() == 25) {
      result.authorization_version.value = 0;
    }
    if (mode.load() == 26) {
      result.checked_at = {};
    }
    if (mode.load() == 27) {
      result.session.expires_at = kChecked;
    }
    return result;
  }
  t::Outcome<t::CurrentPrincipal> get_principal(const t::AccessSessionRequest& request) override {
    observed(request);
    require(same_secret(request.access_token, "access-fixture-secret"));
    if (mode.load() == 1) {
      return configured_failure();
    }
    if (mode.load() == 2) {
      return unknown_failure();
    }
    return t::CurrentPrincipal{
        human(), assignments(mode.load()), {std::numeric_limits<std::uint64_t>::max()}, kChecked};
  }
  t::Outcome<t::RevokeSessionReply>
  revoke_session(const t::RevokeSessionRequest& request) override {
    observed(request);
    require(request.session.value == kSession &&
            same_secret(request.actor.delegation, "delegation-fixture-secret"));
    if (mode.load() == 1) {
      return configured_failure();
    }
    if (mode.load() == 2) {
      return unknown_failure();
    }
    return t::RevokeSessionReply{metadata(true), kChecked};
  }
  t::Outcome<t::SessionPageReply> list_sessions(const t::ListSessionsRequest& request) override {
    observed(request);
    require(same_secret(request.actor.delegation, "delegation-fixture-secret"));
    if (request.subject.value != kSubject) {
      return t::Failure{t::ErrorCode::PermissionDenied, t::Acceptance::NotAccepted,
                        t::Recovery::Diagnose, std::nullopt, std::nullopt};
    }
    if (mode.load() == 1) {
      return configured_failure();
    }
    if (mode.load() == 2) {
      return unknown_failure();
    }
    const auto size = request.page.page_size == 0 ? 20 : request.page.page_size;
    return t::SessionPageReply{{metadata()},
                               size,
                               request.page.snapshot_id.empty() ? kOther : request.page.snapshot_id,
                               "next-fixture",
                               kChecked + std::chrono::minutes{10},
                               {1},
                               kChecked};
  }
};
void request_context(w::RequestContext* context) {
  context->set_request_id(kSubject);
  context->set_trace_id(kOther);
  context->set_idempotency_key(kAssignment);
  context->set_delegation("delegation-fixture-secret");
  const t::TimePoint deadline{std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::system_clock::now() + std::chrono::seconds{30}).time_since_epoch())};
  *context->mutable_deadline() =
      std::get<google::protobuf::Timestamp>(p::target_codec::to_wire(deadline));
}
template <typename Request, typename Response>
grpc::Status call(w::IdentityService::Stub& stub,
                  grpc::Status (w::IdentityService::Stub::*method)(grpc::ClientContext*,
                                                                   const Request&, Response*),
                  const Request& request, Response* response,
                  std::chrono::milliseconds budget = std::chrono::seconds{10}) {
  grpc::ClientContext context;
  if (budget.count() > 0) {
    context.set_deadline(std::chrono::system_clock::now() + budget);
  }
  return (stub.*method)(&context, request, response);
}
w::RpcFailure rejected(const grpc::Status& status, w::ErrorCode expected) {
  require(!status.ok());
  auto decoded = p::target_codec::from_grpc_failure(status);
  require(std::holds_alternative<w::RpcFailure>(decoded));
  const auto& failure = std::get<w::RpcFailure>(decoded);
  require(failure.error().code() == expected && failure.has_context());
  require(failure.error().message().find("secret") == std::string::npos);
  return failure;
}
void check_principal(const w::PrincipalRecord& principal,
                     const google::protobuf::RepeatedPtrField<w::AssignmentRecord>& grants) {
  require(principal.version() == std::numeric_limits<std::uint64_t>::max());
  require(principal.roles_size() == 1 && principal.roles(0) == w::ROLE_VIEWER);
  require(grants.size() == 2 && grants[0].scope().resources(0).kind() == w::RESOURCE_CATALOG);
  require(grants[0].permissions().empty() && grants[0].role() == w::ROLE_VIEWER);
  require(grants[1].permissions_size() == 1 &&
          grants[1].permissions(0) == w::PERMISSION_MESSAGE_READ);
  bool message = false;
  for (auto permission : principal.display_permissions()) {
    require(permission != w::PERMISSION_ACTION_INSPECT);
    if (permission == w::PERMISSION_MESSAGE_READ) {
      message = true;
    }
  }
  require(message);
}
void exercise(w::IdentityService::Stub& stub, ApplicationFixture& app,
              std::atomic<int>& auth_mode) {
  w::IdentityLoginRequest login;
  request_context(login.mutable_context());
  login.set_password("password-fixture-secret");
  login.set_username("human");
  w::IdentityLoginResponse login_reply;
  require(call(stub, &w::IdentityService::Stub::Login, login, &login_reply).ok());
  require(login_reply.tokens().access_token() == "access-fixture-secret" &&
          login_reply.checked_at().nanos() == 123456789);
  require(login_reply.tokens().rotation() == std::numeric_limits<std::uint64_t>::max());
  for (int code = 0; code < 11; ++code) {
    app.mode.store(1);
    app.code_index.store(code);
    constexpr std::array codes{w::INVALID_ARGUMENT,
                               w::UNAUTHENTICATED,
                               w::PERMISSION_DENIED,
                               w::NOT_FOUND,
                               w::CONFLICT,
                               w::RATE_LIMITED,
                               w::TIMEOUT,
                               w::UPSTREAM_UNAVAILABLE,
                               w::PLUGIN_UNAVAILABLE,
                               w::CAPABILITY_NOT_SUPPORTED,
                               w::INTERNAL};
    const auto denied = rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply),
                                 codes.at(static_cast<std::size_t>(code)));
    require(denied.context().acceptance() == w::NOT_ACCEPTED &&
            denied.context().retry_after_ms() == 123);
  }
  app.mode.store(2);
  const auto login_unknown =
      rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::TIMEOUT);
  require(login_unknown.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          login_unknown.context().recovery() == w::QUERY_SAME_INTENT &&
          login_unknown.context().operation().owner_service() == "model" &&
          login_unknown.context().retry_after_ms() == 321);
  app.mode.store(0);
  auth_mode.store(1);
  rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::UNAUTHENTICATED);
  auth_mode.store(2);
  rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply),
           w::UPSTREAM_UNAVAILABLE);
  auth_mode.store(0);
  auto login_expired = login;
  login_expired.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::Login, login_expired, &login_reply), w::TIMEOUT);
  w::IdentityRefreshSessionRequest refreshsession;
  request_context(refreshsession.mutable_context());
  refreshsession.set_refresh_token("refresh-fixture-secret");
  w::IdentityRefreshSessionResponse refreshsession_reply;
  require(
      call(stub, &w::IdentityService::Stub::RefreshSession, refreshsession, &refreshsession_reply)
          .ok());
  require(refreshsession_reply.tokens().access_token() == "access-fixture-secret" &&
          refreshsession_reply.checked_at().nanos() == 123456789);
  require(refreshsession_reply.tokens().rotation() == std::numeric_limits<std::uint64_t>::max());
  for (int code = 0; code < 11; ++code) {
    app.mode.store(1);
    app.code_index.store(code);
    constexpr std::array codes{w::INVALID_ARGUMENT,
                               w::UNAUTHENTICATED,
                               w::PERMISSION_DENIED,
                               w::NOT_FOUND,
                               w::CONFLICT,
                               w::RATE_LIMITED,
                               w::TIMEOUT,
                               w::UPSTREAM_UNAVAILABLE,
                               w::PLUGIN_UNAVAILABLE,
                               w::CAPABILITY_NOT_SUPPORTED,
                               w::INTERNAL};
    const auto denied = rejected(call(stub, &w::IdentityService::Stub::RefreshSession,
                                      refreshsession, &refreshsession_reply),
                                 codes.at(static_cast<std::size_t>(code)));
    require(denied.context().acceptance() == w::NOT_ACCEPTED &&
            denied.context().retry_after_ms() == 123);
  }
  app.mode.store(2);
  const auto refreshsession_unknown = rejected(
      call(stub, &w::IdentityService::Stub::RefreshSession, refreshsession, &refreshsession_reply),
      w::TIMEOUT);
  require(refreshsession_unknown.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          refreshsession_unknown.context().recovery() == w::QUERY_SAME_INTENT &&
          refreshsession_unknown.context().operation().owner_service() == "model" &&
          refreshsession_unknown.context().retry_after_ms() == 321);
  app.mode.store(0);
  auth_mode.store(1);
  rejected(
      call(stub, &w::IdentityService::Stub::RefreshSession, refreshsession, &refreshsession_reply),
      w::UNAUTHENTICATED);
  auth_mode.store(2);
  rejected(
      call(stub, &w::IdentityService::Stub::RefreshSession, refreshsession, &refreshsession_reply),
      w::UPSTREAM_UNAVAILABLE);
  auth_mode.store(0);
  auto refreshsession_expired = refreshsession;
  refreshsession_expired.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::RefreshSession, refreshsession_expired,
                &refreshsession_reply),
           w::TIMEOUT);
  w::IdentityValidateSessionRequest validatesession;
  request_context(validatesession.mutable_context());
  validatesession.set_access_token("access-fixture-secret");
  w::IdentityValidateSessionResponse validatesession_reply;
  require(call(stub, &w::IdentityService::Stub::ValidateSession, validatesession,
               &validatesession_reply)
              .ok());
  check_principal(validatesession_reply.principal(), validatesession_reply.assignments());
  require(validatesession_reply.session().created_at().nanos() == 123456789 &&
          validatesession_reply.session().rotation() == std::numeric_limits<std::uint64_t>::max());
  for (int code = 0; code < 11; ++code) {
    app.mode.store(1);
    app.code_index.store(code);
    constexpr std::array codes{w::INVALID_ARGUMENT,
                               w::UNAUTHENTICATED,
                               w::PERMISSION_DENIED,
                               w::NOT_FOUND,
                               w::CONFLICT,
                               w::RATE_LIMITED,
                               w::TIMEOUT,
                               w::UPSTREAM_UNAVAILABLE,
                               w::PLUGIN_UNAVAILABLE,
                               w::CAPABILITY_NOT_SUPPORTED,
                               w::INTERNAL};
    const auto denied = rejected(call(stub, &w::IdentityService::Stub::ValidateSession,
                                      validatesession, &validatesession_reply),
                                 codes.at(static_cast<std::size_t>(code)));
    require(denied.context().acceptance() == w::NOT_ACCEPTED &&
            denied.context().retry_after_ms() == 123);
  }
  app.mode.store(2);
  const auto validatesession_unknown =
      rejected(call(stub, &w::IdentityService::Stub::ValidateSession, validatesession,
                    &validatesession_reply),
               w::TIMEOUT);
  require(validatesession_unknown.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          validatesession_unknown.context().recovery() == w::QUERY_SAME_INTENT &&
          validatesession_unknown.context().operation().owner_service() == "model" &&
          validatesession_unknown.context().retry_after_ms() == 321);
  app.mode.store(0);
  auth_mode.store(1);
  rejected(call(stub, &w::IdentityService::Stub::ValidateSession, validatesession,
                &validatesession_reply),
           w::UNAUTHENTICATED);
  auth_mode.store(2);
  rejected(call(stub, &w::IdentityService::Stub::ValidateSession, validatesession,
                &validatesession_reply),
           w::UPSTREAM_UNAVAILABLE);
  auth_mode.store(0);
  auto validatesession_expired = validatesession;
  validatesession_expired.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::ValidateSession, validatesession_expired,
                &validatesession_reply),
           w::TIMEOUT);
  w::IdentityGetPrincipalRequest getprincipal;
  request_context(getprincipal.mutable_context());
  getprincipal.set_access_token("access-fixture-secret");
  w::IdentityGetPrincipalResponse getprincipal_reply;
  require(
      call(stub, &w::IdentityService::Stub::GetPrincipal, getprincipal, &getprincipal_reply).ok());
  check_principal(getprincipal_reply.principal(), getprincipal_reply.assignments());
  for (int code = 0; code < 11; ++code) {
    app.mode.store(1);
    app.code_index.store(code);
    constexpr std::array codes{w::INVALID_ARGUMENT,
                               w::UNAUTHENTICATED,
                               w::PERMISSION_DENIED,
                               w::NOT_FOUND,
                               w::CONFLICT,
                               w::RATE_LIMITED,
                               w::TIMEOUT,
                               w::UPSTREAM_UNAVAILABLE,
                               w::PLUGIN_UNAVAILABLE,
                               w::CAPABILITY_NOT_SUPPORTED,
                               w::INTERNAL};
    const auto denied = rejected(
        call(stub, &w::IdentityService::Stub::GetPrincipal, getprincipal, &getprincipal_reply),
        codes.at(static_cast<std::size_t>(code)));
    require(denied.context().acceptance() == w::NOT_ACCEPTED &&
            denied.context().retry_after_ms() == 123);
  }
  app.mode.store(2);
  const auto getprincipal_unknown = rejected(
      call(stub, &w::IdentityService::Stub::GetPrincipal, getprincipal, &getprincipal_reply),
      w::TIMEOUT);
  require(getprincipal_unknown.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          getprincipal_unknown.context().recovery() == w::QUERY_SAME_INTENT &&
          getprincipal_unknown.context().operation().owner_service() == "model" &&
          getprincipal_unknown.context().retry_after_ms() == 321);
  app.mode.store(0);
  auth_mode.store(1);
  rejected(call(stub, &w::IdentityService::Stub::GetPrincipal, getprincipal, &getprincipal_reply),
           w::UNAUTHENTICATED);
  auth_mode.store(2);
  rejected(call(stub, &w::IdentityService::Stub::GetPrincipal, getprincipal, &getprincipal_reply),
           w::UPSTREAM_UNAVAILABLE);
  auth_mode.store(0);
  auto getprincipal_expired = getprincipal;
  getprincipal_expired.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::GetPrincipal, getprincipal_expired,
                &getprincipal_reply),
           w::TIMEOUT);
  w::IdentityRevokeSessionRequest revokesession;
  request_context(revokesession.mutable_context());
  revokesession.set_session_id("22222222-2222-4222-8222-222222222222");
  w::IdentityRevokeSessionResponse revokesession_reply;
  require(call(stub, &w::IdentityService::Stub::RevokeSession, revokesession, &revokesession_reply)
              .ok());
  require(revokesession_reply.session().revoked());
  for (int code = 0; code < 11; ++code) {
    app.mode.store(1);
    app.code_index.store(code);
    constexpr std::array codes{w::INVALID_ARGUMENT,
                               w::UNAUTHENTICATED,
                               w::PERMISSION_DENIED,
                               w::NOT_FOUND,
                               w::CONFLICT,
                               w::RATE_LIMITED,
                               w::TIMEOUT,
                               w::UPSTREAM_UNAVAILABLE,
                               w::PLUGIN_UNAVAILABLE,
                               w::CAPABILITY_NOT_SUPPORTED,
                               w::INTERNAL};
    const auto denied = rejected(
        call(stub, &w::IdentityService::Stub::RevokeSession, revokesession, &revokesession_reply),
        codes.at(static_cast<std::size_t>(code)));
    require(denied.context().acceptance() == w::NOT_ACCEPTED &&
            denied.context().retry_after_ms() == 123);
  }
  app.mode.store(2);
  const auto revokesession_unknown = rejected(
      call(stub, &w::IdentityService::Stub::RevokeSession, revokesession, &revokesession_reply),
      w::TIMEOUT);
  require(revokesession_unknown.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          revokesession_unknown.context().recovery() == w::QUERY_SAME_INTENT &&
          revokesession_unknown.context().operation().owner_service() == "model" &&
          revokesession_unknown.context().retry_after_ms() == 321);
  app.mode.store(0);
  auth_mode.store(1);
  rejected(
      call(stub, &w::IdentityService::Stub::RevokeSession, revokesession, &revokesession_reply),
      w::UNAUTHENTICATED);
  auth_mode.store(2);
  rejected(
      call(stub, &w::IdentityService::Stub::RevokeSession, revokesession, &revokesession_reply),
      w::UPSTREAM_UNAVAILABLE);
  auth_mode.store(0);
  auto revokesession_expired = revokesession;
  revokesession_expired.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::RevokeSession, revokesession_expired,
                &revokesession_reply),
           w::TIMEOUT);
  w::IdentityListSessionsRequest listsessions;
  request_context(listsessions.mutable_context());
  listsessions.set_principal_id("11111111-1111-4111-8111-111111111111");
  w::IdentityListSessionsResponse listsessions_reply;
  require(
      call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply).ok());
  require(listsessions_reply.page().snapshot_id() == kOther &&
          listsessions_reply.page().page_size() == 20);
  for (int code = 0; code < 11; ++code) {
    app.mode.store(1);
    app.code_index.store(code);
    constexpr std::array codes{w::INVALID_ARGUMENT,
                               w::UNAUTHENTICATED,
                               w::PERMISSION_DENIED,
                               w::NOT_FOUND,
                               w::CONFLICT,
                               w::RATE_LIMITED,
                               w::TIMEOUT,
                               w::UPSTREAM_UNAVAILABLE,
                               w::PLUGIN_UNAVAILABLE,
                               w::CAPABILITY_NOT_SUPPORTED,
                               w::INTERNAL};
    const auto denied = rejected(
        call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply),
        codes.at(static_cast<std::size_t>(code)));
    require(denied.context().acceptance() == w::NOT_ACCEPTED &&
            denied.context().retry_after_ms() == 123);
  }
  app.mode.store(2);
  const auto listsessions_unknown = rejected(
      call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply),
      w::TIMEOUT);
  require(listsessions_unknown.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          listsessions_unknown.context().recovery() == w::QUERY_SAME_INTENT &&
          listsessions_unknown.context().operation().owner_service() == "model" &&
          listsessions_unknown.context().retry_after_ms() == 321);
  app.mode.store(0);
  auth_mode.store(1);
  rejected(call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply),
           w::UNAUTHENTICATED);
  auth_mode.store(2);
  rejected(call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply),
           w::UPSTREAM_UNAVAILABLE);
  auth_mode.store(0);
  auto listsessions_expired = listsessions;
  listsessions_expired.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::ListSessions, listsessions_expired,
                &listsessions_reply),
           w::TIMEOUT);
  // Input deadlines and pagination are rejected before pure application calls.
  const auto calls_before = app.calls.load();
  auto invalid_login = login;
  invalid_login.mutable_context()->set_request_id("not-a-uuid");
  const auto bad_id =
      rejected(call(stub, &w::IdentityService::Stub::Login, invalid_login, &login_reply),
               w::INVALID_ARGUMENT);
  require(bad_id.error().request_id().size() == 36 && bad_id.error().trace_id() == kOther);
  invalid_login = login;
  invalid_login.mutable_context()->set_idempotency_key("illegal key");
  rejected(call(stub, &w::IdentityService::Stub::Login, invalid_login, &login_reply),
           w::INVALID_ARGUMENT);
  invalid_login = login;
  invalid_login.mutable_context()->mutable_deadline()->set_nanos(-1);
  rejected(call(stub, &w::IdentityService::Stub::Login, invalid_login, &login_reply),
           w::INVALID_ARGUMENT);
  invalid_login = login;
  invalid_login.mutable_context()->mutable_deadline()->set_seconds(1);
  rejected(call(stub, &w::IdentityService::Stub::Login, invalid_login, &login_reply), w::TIMEOUT);
  for (int mode = 0; mode < 4; ++mode) {
    auto bad = listsessions;
    if (mode == 0) {
      bad.mutable_page()->mutable_expires_at()->set_seconds(1);
    }
    if (mode == 1) {
      bad.mutable_page()->set_next_page_token("output-only");
    }
    if (mode == 2) {
      bad.mutable_page()->set_page_size(101);
    }
    if (mode == 3) {
      bad.mutable_page()->set_page_token("opaque-token");
      bad.mutable_page()->set_snapshot_id("invalid-snapshot");
    }
    rejected(call(stub, &w::IdentityService::Stub::ListSessions, bad, &listsessions_reply),
             w::INVALID_ARGUMENT);
  }
  require(app.calls.load() == calls_before);
  listsessions.mutable_page()->set_snapshot_id(kOther);
  listsessions.mutable_page()->set_page_token("cursor-fixture");
  require(
      call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply).ok());
  // Same committed pair after access expiry is recoverable while refresh remains valid.
  app.mode.store(31);
  require(call(stub, &w::IdentityService::Stub::Login, login, &login_reply).ok());
  require(login_reply.checked_at().seconds() ==
          kChecked.time_since_epoch().count() / 1'000'000'000 + 901);
  require(login_reply.tokens().access_expires_at().seconds() + 1 ==
          login_reply.checked_at().seconds());
  require(login_reply.tokens().refresh_expires_at().seconds() > login_reply.checked_at().seconds());
  require(
      call(stub, &w::IdentityService::Stub::RefreshSession, refreshsession, &refreshsession_reply)
          .ok());
  require(refreshsession_reply.tokens().access_expires_at().SerializeAsString() ==
          login_reply.tokens().access_expires_at().SerializeAsString());
  app.mode.store(0);
  // REST uses one opaque token; the optional snapshot field is not required to resume.
  listsessions.mutable_page()->clear_snapshot_id();
  listsessions.mutable_page()->set_page_token("opaque-next-fixture");
  require(
      call(stub, &w::IdentityService::Stub::ListSessions, listsessions, &listsessions_reply).ok());
  for (int mode : {21, 22, 23, 24, 25, 26, 27, 28}) {
    app.mode.store(mode);
    rejected(call(stub, &w::IdentityService::Stub::ValidateSession, validatesession,
                  &validatesession_reply),
             w::INTERNAL);
  }
  app.mode.store(20);
  rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::INTERNAL);
  app.mode.store(12);
  const auto exception =
      rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::INTERNAL);
  require(exception.context().acceptance() == w::ACCEPTANCE_UNKNOWN);
  app.mode.store(13);
  const auto dangerous =
      rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::INTERNAL);
  require(dangerous.context().acceptance() == w::ACCEPTANCE_UNKNOWN &&
          dangerous.context().recovery() == w::QUERY_SAME_INTENT);
  app.mode.store(0);
  auth_mode.store(3);
  rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::UNAUTHENTICATED);
  auth_mode.store(0);
  const auto before = std::chrono::system_clock::now();
  require(call(stub, &w::IdentityService::Stub::Login, login, &login_reply).ok());
  require(app.last_deadline.load() <= std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          (before + std::chrono::seconds{6}).time_since_epoch())
                                          .count());
  require(call(stub, &w::IdentityService::Stub::Login, login, &login_reply,
               std::chrono::milliseconds{0})
              .ok());
  require(app.last_deadline.load() <=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              (std::chrono::system_clock::now() + std::chrono::seconds{2}).time_since_epoch())
              .count());
  // The earlier body deadline and real gRPC deadline both constrain the pure app.
  const auto client_started = std::chrono::system_clock::now();
  require(call(stub, &w::IdentityService::Stub::Login, login, &login_reply,
               std::chrono::milliseconds{300})
              .ok());
  require(app.last_deadline.load() <=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              (client_started + std::chrono::milliseconds{400}).time_since_epoch())
              .count());
  auto earlier = login;
  const t::TimePoint earlier_deadline{std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::system_clock::now() + std::chrono::seconds{1}).time_since_epoch())};
  *earlier.mutable_context()->mutable_deadline() =
      std::get<google::protobuf::Timestamp>(p::target_codec::to_wire(earlier_deadline));
  require(call(stub, &w::IdentityService::Stub::Login, earlier, &login_reply).ok());
  require(app.last_deadline.load() == earlier_deadline.time_since_epoch().count());
  const auto before_delay = app.calls.load();
  const t::TimePoint imminent{std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::system_clock::now() + std::chrono::milliseconds{10}).time_since_epoch())};
  *earlier.mutable_context()->mutable_deadline() =
      std::get<google::protobuf::Timestamp>(p::target_codec::to_wire(imminent));
  auth_mode.store(4);
  rejected(call(stub, &w::IdentityService::Stub::Login, earlier, &login_reply), w::TIMEOUT);
  require(app.calls.load() == before_delay);
  auth_mode.store(5);
  const auto peer_exception =
      rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::INTERNAL);
  require(peer_exception.context().acceptance() == w::NOT_ACCEPTED);
  auth_mode.store(0);
  w::IdentityGetCredentialStatusRequest unsupported;
  request_context(unsupported.mutable_context());
  unsupported.set_credential_id(kCredential);
  w::IdentityGetCredentialStatusResponse unsupported_reply;
  rejected(
      call(stub, &w::IdentityService::Stub::GetCredentialStatus, unsupported, &unsupported_reply),
      w::CAPABILITY_NOT_SUPPORTED);
  auto other_subject = listsessions;
  other_subject.set_principal_id(kOther);
  rejected(call(stub, &w::IdentityService::Stub::ListSessions, other_subject, &listsessions_reply),
           w::PERMISSION_DENIED);
  app.mode.store(1);
  app.code_index.store(4);
  constexpr std::array recoveries{w::CORRECT_INPUT,     w::REAUTHENTICATE, w::RESYNC_SNAPSHOT,
                                  w::QUERY_SAME_INTENT, w::RETRY_SAME_ID,  w::MANUAL_REVIEW};
  for (std::size_t i = 0; i < recoveries.size(); ++i) {
    app.recovery_index.store(static_cast<int>(i));
    const auto recovery =
        rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::CONFLICT);
    require(recovery.context().recovery() == recoveries[i]);
  }
  app.recovery_index.store(5);
  app.mode.store(14);
  const auto accepted =
      rejected(call(stub, &w::IdentityService::Stub::Login, login, &login_reply), w::TIMEOUT);
  require(accepted.context().acceptance() == w::DURABLY_ACCEPTED &&
          accepted.context().has_operation());
  app.mode.store(0);
  const auto before_unsupported = app.calls.load();
  w::IdentityRegisterPrincipalRequest unsupported_registerprincipal;
  request_context(unsupported_registerprincipal.mutable_context());
  w::IdentityRegisterPrincipalResponse unsupported_registerprincipal_reply;
  rejected(call(stub, &w::IdentityService::Stub::RegisterPrincipal, unsupported_registerprincipal,
                &unsupported_registerprincipal_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityRotateCredentialRequest unsupported_rotatecredential;
  request_context(unsupported_rotatecredential.mutable_context());
  w::IdentityRotateCredentialResponse unsupported_rotatecredential_reply;
  rejected(call(stub, &w::IdentityService::Stub::RotateCredential, unsupported_rotatecredential,
                &unsupported_rotatecredential_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityRevokeCredentialRequest unsupported_revokecredential;
  request_context(unsupported_revokecredential.mutable_context());
  w::IdentityRevokeCredentialResponse unsupported_revokecredential_reply;
  rejected(call(stub, &w::IdentityService::Stub::RevokeCredential, unsupported_revokecredential,
                &unsupported_revokecredential_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityGetPrincipalStatusRequest unsupported_getprincipalstatus;
  request_context(unsupported_getprincipalstatus.mutable_context());
  w::IdentityGetPrincipalStatusResponse unsupported_getprincipalstatus_reply;
  rejected(call(stub, &w::IdentityService::Stub::GetPrincipalStatus, unsupported_getprincipalstatus,
                &unsupported_getprincipalstatus_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityIssueDelegationRequest unsupported_issuedelegation;
  request_context(unsupported_issuedelegation.mutable_context());
  w::IdentityIssueDelegationResponse unsupported_issuedelegation_reply;
  rejected(call(stub, &w::IdentityService::Stub::IssueDelegation, unsupported_issuedelegation,
                &unsupported_issuedelegation_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityExchangeDelegationRequest unsupported_exchangedelegation;
  request_context(unsupported_exchangedelegation.mutable_context());
  w::IdentityExchangeDelegationResponse unsupported_exchangedelegation_reply;
  rejected(call(stub, &w::IdentityService::Stub::ExchangeDelegation, unsupported_exchangedelegation,
                &unsupported_exchangedelegation_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityCheckAuthorizationRequest unsupported_checkauthorization;
  request_context(unsupported_checkauthorization.mutable_context());
  w::IdentityCheckAuthorizationResponse unsupported_checkauthorization_reply;
  rejected(call(stub, &w::IdentityService::Stub::CheckAuthorization, unsupported_checkauthorization,
                &unsupported_checkauthorization_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityListAssignmentsRequest unsupported_listassignments;
  request_context(unsupported_listassignments.mutable_context());
  w::IdentityListAssignmentsResponse unsupported_listassignments_reply;
  rejected(call(stub, &w::IdentityService::Stub::ListAssignments, unsupported_listassignments,
                &unsupported_listassignments_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentitySetAssignmentRequest unsupported_setassignment;
  request_context(unsupported_setassignment.mutable_context());
  w::IdentitySetAssignmentResponse unsupported_setassignment_reply;
  rejected(call(stub, &w::IdentityService::Stub::SetAssignment, unsupported_setassignment,
                &unsupported_setassignment_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityRevokeAssignmentRequest unsupported_revokeassignment;
  request_context(unsupported_revokeassignment.mutable_context());
  w::IdentityRevokeAssignmentResponse unsupported_revokeassignment_reply;
  rejected(call(stub, &w::IdentityService::Stub::RevokeAssignment, unsupported_revokeassignment,
                &unsupported_revokeassignment_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityWatchInvalidationsRequest unsupported_watchinvalidations;
  request_context(unsupported_watchinvalidations.mutable_context());
  w::IdentityWatchInvalidationsResponse unsupported_watchinvalidations_reply;
  grpc::ClientContext unsupported_watchinvalidations_context;
  unsupported_watchinvalidations_context.set_deadline(std::chrono::system_clock::now() +
                                                      std::chrono::seconds{2});
  auto unsupported_watchinvalidations_stream = stub.WatchInvalidations(
      &unsupported_watchinvalidations_context, unsupported_watchinvalidations);
  require(!unsupported_watchinvalidations_stream->Read(&unsupported_watchinvalidations_reply));
  rejected(unsupported_watchinvalidations_stream->Finish(), w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityGetAuthorizationSnapshotRequest unsupported_getauthorizationsnapshot;
  request_context(unsupported_getauthorizationsnapshot.mutable_context());
  w::IdentityGetAuthorizationSnapshotResponse unsupported_getauthorizationsnapshot_reply;
  rejected(call(stub, &w::IdentityService::Stub::GetAuthorizationSnapshot,
                unsupported_getauthorizationsnapshot, &unsupported_getauthorizationsnapshot_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  w::IdentityGetVerificationKeysRequest unsupported_getverificationkeys;
  request_context(unsupported_getverificationkeys.mutable_context());
  w::IdentityGetVerificationKeysResponse unsupported_getverificationkeys_reply;
  rejected(call(stub, &w::IdentityService::Stub::GetVerificationKeys,
                unsupported_getverificationkeys, &unsupported_getverificationkeys_reply),
           w::CAPABILITY_NOT_SUPPORTED);
  require(app.calls.load() == before_unsupported);
}
// Read-only repository fixture for the production authenticator. Certificate parsing/verification
// below uses the real OpenSSL adapter; this fixture does not claim PostgreSQL persistence proof.
struct RegistryFixture {
  t::Principal principal;
  t::CredentialVersionRecord credential;
  std::atomic<int> mode{0};
  std::atomic<int> active{0};
  std::atomic<int> reads{0};
};
t::Failure registry_unavailable() {
  return {
      t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
class PrincipalRepositoryFixture final : public t::Repository<t::SubjectId, t::Principal> {
public:
  explicit PrincipalRepositoryFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::optional<t::Principal>> find(const t::SubjectId& subject) override {
    if (subject != registry_.principal.id) {
      return std::optional<t::Principal>{};
    }
    auto principal = registry_.principal;
    if (registry_.mode.load() == 3) {
      principal.state = t::LifecycleState::Disabled;
    }
    return std::optional<t::Principal>{principal};
  }
  t::Status save(const t::Principal&, std::optional<t::ResourceVersion>) override {
    throw std::logic_error("unused write");
  }

private:
  RegistryFixture& registry_;
};
class CredentialRepositoryFixture final : public t::IdentityCredentialRepository {
public:
  explicit CredentialRepositoryFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  current(const t::CredentialId& id) override {
    if (id != registry_.credential.ref.id) {
      return std::optional<t::CredentialVersionRecord>{};
    }
    auto credential = registry_.credential;
    if (registry_.mode.load() == 4) {
      ++credential.ref.version.value;
    }
    return std::optional<t::CredentialVersionRecord>{credential};
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  version(const t::CredentialVersionRef&) override {
    throw std::logic_error("unused historical lookup");
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest& fingerprint) override {
    ++registry_.reads;
    if (registry_.mode.load() == 1) {
      return registry_unavailable();
    }
    const auto& cert = std::get<t::CertificateBinding>(registry_.credential.material);
    if (registry_.mode.load() == 2 || fingerprint != cert.fingerprint) {
      return std::optional<t::CredentialVersionRecord>{};
    }
    auto credential = registry_.credential;
    if (registry_.mode.load() == 5) {
      std::get<t::CertificateBinding>(credential.material).registered_san = "wrong.test";
    }
    return std::optional<t::CredentialVersionRecord>{credential};
  }
  t::Outcome<std::optional<t::LoginIdentity>> login(const std::string&) override {
    throw std::logic_error("unused login lookup");
  }
  t::Status save_login(const t::LoginIdentity&, std::optional<t::ResourceVersion>) override {
    throw std::logic_error("unused login write");
  }
  t::Status save(const t::CredentialVersionRecord&, std::optional<t::ResourceVersion>) override {
    throw std::logic_error("unused credential write");
  }

private:
  RegistryFixture& registry_;
};
class RegistryWorkFixture final : public t::IdentitySecurityUnitOfWork {
public:
  explicit RegistryWorkFixture(RegistryFixture& registry)
      : registry_(registry), principals_(registry), credentials_(registry) {
    ++registry_.active;
  }
  ~RegistryWorkFixture() override {
    rollback();
  }
  t::Outcome<t::TimePoint> current_time() override {
    return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
  }
  t::Repository<t::SubjectId, t::Principal>& principals() override {
    return principals_;
  }
  t::IdentityCredentialRepository& credentials() override {
    return credentials_;
  }
  t::CommitResult commit() override {
    throw std::logic_error("authentication must not write");
  }
  void rollback() noexcept override {
    if (active_) {
      --registry_.active;
      active_ = false;
    }
  }
  t::ReceiptRepository& receipts() override {
    throw std::logic_error("unused receipt");
  }
  t::JournalRepository& journal() override {
    throw std::logic_error("unused journal");
  }
  t::Repository<t::OperationId, t::Operation>& operations() override {
    throw std::logic_error("unused operation");
  }
  t::Repository<t::SessionId, t::LoginSession>& sessions() override {
    throw std::logic_error("unused session");
  }
  t::Repository<t::AssignmentId, t::ResourceGrant>& grants() override {
    throw std::logic_error("authentication must not grant");
  }
  t::Repository<t::SubjectId, t::AuthorizationState>& authorizations() override {
    throw std::logic_error("unused grants");
  }
  t::IdentitySessionSecurityRepository& session_security() override {
    throw std::logic_error("unused secret session");
  }
  t::IdentityExactAssignmentRepository& exact_assignments() override {
    throw std::logic_error("unused authorization");
  }
  t::IdentitySecretReceiptRepository& secret_receipts() override {
    throw std::logic_error("unused secret receipt");
  }
  t::IdentityBootstrapRepository& bootstrap() override {
    throw std::logic_error("unused bootstrap");
  }

private:
  RegistryFixture& registry_;
  PrincipalRepositoryFixture principals_;
  CredentialRepositoryFixture credentials_;
  bool active_ = true;
};
class RegistryFactoryFixture final : public t::IdentitySecurityUnitOfWorkFactory {
public:
  explicit RegistryFactoryFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::unique_ptr<t::IdentitySecurityUnitOfWork>> begin() override {
    std::unique_ptr<t::IdentitySecurityUnitOfWork> result =
        std::make_unique<RegistryWorkFixture>(registry_);
    return result;
  }

private:
  RegistryFixture& registry_;
};
class ClockFixture final : public t::SecurityClockPort {
public:
  explicit ClockFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<t::SecurityClockSample> sample() override {
    require(registry_.active.load() == 0);
    auto utc = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
    if (registry_.mode.load() == 8) {
      utc += std::chrono::seconds{3};
    }
    return t::SecurityClockSample{utc, std::chrono::steady_clock::now(),
                                  std::chrono::milliseconds{0}, registry_.mode.load() != 7};
  }

private:
  RegistryFixture& registry_;
};
t::TimePoint cert_time(const ASN1_TIME* value) {
  std::tm decoded{};
  require(ASN1_TIME_to_tm(value, &decoded) == 1);
  google::protobuf::Timestamp timestamp;
  timestamp.set_seconds(timegm(&decoded));
  return std::get<t::TimePoint>(p::target_codec::from_wire(timestamp));
}
t::CertificateBinding certificate_binding(const std::string& leaf) {
  std::unique_ptr<BIO, decltype(&BIO_free)> input(
      BIO_new_mem_buf(leaf.data(), static_cast<int>(leaf.size())), BIO_free);
  std::unique_ptr<X509, decltype(&X509_free)> certificate(
      PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
  require(certificate != nullptr);
  t::Sha256Digest digest;
  unsigned int size = 0;
  require(X509_digest(certificate.get(), EVP_sha256(),
                      reinterpret_cast<unsigned char*>(digest.bytes.data()), &size) == 1 &&
          size == 32);
  return {{kPeer},
          digest,
          "localhost",
          t::Service::Control,
          cert_time(X509_get0_notBefore(certificate.get())),
          cert_time(X509_get0_notAfter(certificate.get()))};
}
class CertificateSpy final : public t::PeerCertificatePort {
public:
  CertificateSpy(RegistryFixture& registry, t::PeerCertificatePort& actual)
      : registry_(registry), actual_(actual) {}
  t::Outcome<t::VerifiedPeer> verify(std::span<const std::byte> chain,
                                     const t::RegisteredPeer& expected, t::TimePoint now) override {
    require(registry_.active.load() == 0);
    auto result = actual_.verify(chain, expected, now);
    if (registry_.mode.load() == 6) {
      registry_.credential.state = t::CredentialState::Revoked;
      registry_.credential.revoked_at = now;
    }
    return result;
  }

private:
  RegistryFixture& registry_;
  t::PeerCertificatePort& actual_;
};
void exercise_registered_peer(const std::string& leaf, const std::string& ca,
                              const std::string& key) {
  auto certificate = certificate_binding(leaf);
  const t::ResourceRef resource{t::ResourceKind::Principal, {kPeer}};
  RegistryFixture registry{
      t::Principal{
          {kPeer}, t::PrincipalKind::PlatformService, resource, {1}, t::LifecycleState::Active},
      t::CredentialVersionRecord{{{kCredential}, {1}},
                                 {kPeer},
                                 certificate,
                                 t::CredentialState::Active,
                                 certificate.not_before,
                                 std::nullopt,
                                 resource,
                                 certificate.not_after}};
  RegistryFactoryFixture transactions{registry};
  ClockFixture clock{registry};
  namespace crypto = kiarabot::infrastructure::identity_crypto;
  auto actual = std::get<std::unique_ptr<t::PeerCertificatePort>>(
      crypto::make_peer_certificate_crypto({{ca}, crypto::PeerSanKind::Dns}));
  CertificateSpy verified_certificates{registry, *actual};
  auto authenticator = std::get<p::PeerAuthenticator>(
      p::make_identity_peer_authenticator(transactions, verified_certificates, clock));
  ApplicationFixture app;
  auto service = std::get<std::unique_ptr<w::IdentityService::Service>>(
      p::make_identity_rpc(app, authenticator));
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = ca;
  options.pem_key_cert_pairs.push_back({key, leaf});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.RegisterService(service.get());
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  auto server = builder.BuildAndStart();
  require(server && port > 0);
  grpc::SslCredentialsOptions client;
  client.pem_root_certs = ca;
  client.pem_cert_chain = leaf;
  client.pem_private_key = key;
  auto stub = w::IdentityService::NewStub(
      grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(client)));
  w::IdentityLoginRequest request;
  request_context(request.mutable_context());
  request.set_username("human");
  request.set_password("password-fixture-secret");
  w::IdentityLoginResponse response;
  require(call(*stub, &w::IdentityService::Stub::Login, request, &response).ok());
  require(registry.reads.load() == 2 && registry.active.load() == 0 && app.calls.load() == 1);
  const auto before = app.calls.load();
  for (const auto mode : {1, 2, 3, 4, 7, 8, 6}) {
    registry.mode.store(mode);
    rejected(call(*stub, &w::IdentityService::Stub::Login, request, &response),
             mode == 1 || mode == 7 || mode == 8 ? w::UPSTREAM_UNAVAILABLE : w::UNAUTHENTICATED);
    require(app.calls.load() == before && registry.active.load() == 0);
  }
  registry.mode.store(0);
  registry.credential.state = t::CredentialState::Active;
  registry.credential.revoked_at.reset();
  std::get<t::CertificateBinding>(registry.credential.material).registered_san = "wrong.test";
  rejected(call(*stub, &w::IdentityService::Stub::Login, request, &response), w::UNAUTHENTICATED);
  std::get<t::CertificateBinding>(registry.credential.material).registered_san = "localhost";
  require(call(*stub, &w::IdentityService::Stub::Login, request, &response).ok());
  server->Shutdown();
  server->Wait();
}
struct GrpcLifetime {
  GrpcLifetime() {
    grpc_init();
  }
  ~GrpcLifetime() {
    grpc_shutdown();
  }
};
} // namespace
int main(int argc, char* argv[]) {
  try {
    if (argc != 4) {
      return 2;
    }
    const GrpcLifetime lifetime;
    const auto leaf = read(argv[1]);
    const auto ca = read(argv[2]);
    const auto key = read(argv[3]);
    ApplicationFixture app;
    require(std::holds_alternative<t::Failure>(p::make_identity_rpc(app, {})));
    std::atomic<int> auth_mode{0};
    std::atomic<int> auth_calls{0};
    // Explicit verifier FIXTURE: its immutable registry binds the one test leaf to Control.
    // This does not implement production current-registration/credential validation.
    const auto verifier = [&](grpc::ServerContext& context) -> t::Outcome<t::VerifiedPeer> {
      ++auth_calls;
      auto evidence = p::extract_peer_certificate(*context.auth_context());
      const auto* certificate = std::get_if<p::PeerCertificateEvidence>(&evidence);
      require(certificate && !certificate->der_chain.empty());
      if (auth_mode.load() == 1) {
        return t::Failure{t::ErrorCode::Unauthenticated, t::Acceptance::NotAccepted,
                          t::Recovery::Authenticate, std::nullopt, std::nullopt};
      }
      if (auth_mode.load() == 2) {
        return t::Failure{t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted,
                          t::Recovery::Diagnose, std::nullopt, std::nullopt};
      }
      t::VerifiedPeer peer{{t::PlatformPeer{t::Service::Control, {kPeer}},
                            {{kPeer},
                             certificate->leaf_fingerprint,
                             "localhost",
                             t::Service::Control,
                             kChecked - std::chrono::hours{24},
                             kChecked + std::chrono::hours{24}},
                            {{kCredential}, {1}}},
                           kChecked};
      if (auth_mode.load() == 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
      }
      if (auth_mode.load() == 5) {
        throw std::runtime_error("never-return-peer-secret");
      }
      if (auth_mode.load() == 3) {
        peer.binding.certificate.fingerprint.bytes[0] ^= std::byte{1};
      }
      return peer;
    };
    auto service =
        std::get<std::unique_ptr<w::IdentityService::Service>>(p::make_identity_rpc(app, verifier));
    // A fabricated callback cannot bypass a missing real AuthContext.
    grpc::ServerContext no_peer;
    w::IdentityLoginRequest no_peer_request;
    request_context(no_peer_request.mutable_context());
    w::IdentityLoginResponse no_peer_reply;
    rejected(service->Login(&no_peer, &no_peer_request, &no_peer_reply), w::UNAUTHENTICATED);
    require(auth_calls.load() == 0);
    grpc::SslServerCredentialsOptions options;
    options.pem_root_certs = ca;
    options.pem_key_cert_pairs.push_back({key, leaf});
    options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.RegisterService(service.get());
    builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
    auto server = builder.BuildAndStart();
    require(server && port > 0);
    grpc::SslCredentialsOptions client;
    client.pem_root_certs = ca;
    client.pem_cert_chain = leaf;
    client.pem_private_key = key;
    auto stub = w::IdentityService::NewStub(
        grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(client)));
    exercise(*stub, app, auth_mode);
    const auto before = auth_calls.load();
    grpc::SslCredentialsOptions missing;
    missing.pem_root_certs = ca;
    auto missing_stub = w::IdentityService::NewStub(
        grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(missing)));
    grpc::ClientContext bad_client;
    bad_client.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds{200});
    require(!missing_stub->Login(&bad_client, no_peer_request, &no_peer_reply).ok());
    require(auth_calls.load() == before);
    server->Shutdown();
    server->Wait();
    exercise_registered_peer(leaf, ca, key);
    std::cout << "Six Identity RPCs over real mTLS: boundary projections, stable failures, unknown "
                 "commit, peer/deadline rejection passed (fixture app only)\n";
    return 0;
  } catch (...) {
    std::cerr << "Identity RPC fixture assertion line " << failed_line.load()
              << "; diagnostics redacted\n";
    return 1;
  }
}
