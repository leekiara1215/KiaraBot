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

#include <google/protobuf/descriptor.h>
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
  std::optional<t::ValidatedSession> issued_source{};
  std::atomic<int> mode{0};
  std::atomic<int> calls{0};
  std::atomic<std::int64_t> last_deadline{0};
  std::atomic<std::int64_t> last_peer_expiry{0};
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
    if constexpr (requires { request.control_peer; }) {
      last_peer_expiry.store(
          request.control_peer.binding.certificate.not_after.time_since_epoch().count());
    }
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
    if (!same_secret(request.access_token, "access-fixture-secret")) {
      return t::Failure{t::ErrorCode::Unauthenticated,
                        t::Acceptance::NotAccepted,
                        t::Recovery::Authenticate,
                        {},
                        {}};
    }
    if (mode.load() == 1) {
      return configured_failure();
    }
    if (mode.load() == 2) {
      return unknown_failure();
    }
    if (issued_source) {
      if (issued_source->session.revoked) {
        return t::Failure{t::ErrorCode::Unauthenticated,
                          t::Acceptance::NotAccepted,
                          t::Recovery::Authenticate,
                          {},
                          {}};
      }
      auto result = *issued_source;
      result.checked_at = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
      return result;
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
void exercise_invocation_deadline(w::IdentityService::Stub& stub, ApplicationFixture& app,
                                  std::atomic<int>& mode,
                                  std::atomic<std::int64_t>& supplied_limit) {
  w::IdentityLoginRequest request;
  request_context(request.mutable_context());
  request.set_username("human");
  request.set_password("password-fixture-secret");
  w::IdentityLoginResponse response;
  const auto before = app.calls.load();
  mode.store(1);
  const auto expired =
      rejected(call(stub, &w::IdentityService::Stub::Login, request, &response), w::TIMEOUT);
  require(expired.context().acceptance() == w::NOT_ACCEPTED);
  mode.store(3);
  const auto unavailable = rejected(
      call(stub, &w::IdentityService::Stub::Login, request, &response), w::UPSTREAM_UNAVAILABLE);
  require(std::get<int>(p::target_codec::http_status(t::ErrorCode::UpstreamUnavailable)) == 503);
  require(unavailable.context().acceptance() == w::NOT_ACCEPTED &&
          unavailable.context().retry_after_ms() == 123);
  mode.store(4);
  const auto exception =
      rejected(call(stub, &w::IdentityService::Stub::Login, request, &response), w::INTERNAL);
  require(exception.context().acceptance() == w::NOT_ACCEPTED);
  mode.store(5);
  rejected(call(stub, &w::IdentityService::Stub::Login, request, &response),
           w::UPSTREAM_UNAVAILABLE);
  require(app.calls.load() == before);
  const auto now = std::chrono::system_clock::now();
  const auto cap = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       (now + std::chrono::seconds{1}).time_since_epoch())
                       .count();
  supplied_limit.store(cap);
  mode.store(2);
  require(call(stub, &w::IdentityService::Stub::Login, request, &response).ok());
  require(app.last_deadline.load() == cap);
  require(app.last_peer_expiry.load() ==
          (kChecked + std::chrono::hours{24}).time_since_epoch().count());
  // A future own-TLS limit cannot extend a shorter body or transport budget.
  mode.store(0);
  auto shorter = request;
  const t::TimePoint body_cap{std::chrono::duration_cast<std::chrono::nanoseconds>(
      (std::chrono::system_clock::now() + std::chrono::seconds{1}).time_since_epoch())};
  *shorter.mutable_context()->mutable_deadline() =
      std::get<google::protobuf::Timestamp>(p::target_codec::to_wire(body_cap));
  require(call(stub, &w::IdentityService::Stub::Login, shorter, &response).ok());
  require(app.last_deadline.load() == body_cap.time_since_epoch().count());
  const auto transport_start = std::chrono::system_clock::now();
  require(call(stub, &w::IdentityService::Stub::Login, request, &response,
               std::chrono::milliseconds{300})
              .ok());
  require(app.last_deadline.load() <=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              (transport_start + std::chrono::milliseconds{400}).time_since_epoch())
              .count());
}
struct RegistryFixture {
  t::Principal principal;
  t::CredentialVersionRecord credential;
  std::optional<t::Principal> human{};
  std::optional<t::CredentialVersionRecord> password{};
  std::optional<t::SessionSecurity> session{};
  std::vector<t::ExactAssignment> grants{};
  std::optional<t::Principal> runtime{};
  std::optional<t::CredentialVersionRecord> runtime_credential{};
  bool runtime_capability = true;
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
    if (registry_.runtime && subject == registry_.runtime->id) {
      return registry_.runtime;
    }
    if (registry_.human && subject == registry_.human->id) {
      return registry_.human;
    }
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
    if (registry_.runtime_credential && id == registry_.runtime_credential->ref.id) {
      return registry_.runtime_credential;
    }
    if (registry_.password && id == registry_.password->ref.id) {
      return registry_.password;
    }
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
  by_bootstrap(const t::TokenVerifier&) override {
    throw std::logic_error("unused bootstrap proof lookup");
  }
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_certificate(const t::Sha256Digest& fingerprint) override {
    ++registry_.reads;
    if (registry_.mode.load() == 1) {
      return registry_unavailable();
    }
    if (registry_.runtime_credential &&
        fingerprint ==
            std::get<t::CertificateBinding>(registry_.runtime_credential->material).fingerprint) {
      return registry_.runtime_credential;
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
  t::Outcome<std::optional<t::CredentialVersionRecord>>
  by_platform(const t::PlatformPeer&) override {
    throw std::logic_error("unexpected platform credential lookup");
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
class IssueSessionRepositoryFixture final : public t::IdentitySessionSecurityRepository {
public:
  explicit IssueSessionRepositoryFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::size_t> count_unexpired() override {
    throw std::logic_error("proof issuance does not count or create sessions");
  }
  t::Outcome<std::optional<t::SessionSecurity>> find(const t::SessionId& id) override {
    if (!registry_.session || registry_.session->id != id) {
      return std::optional<t::SessionSecurity>{};
    }
    return registry_.session;
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_access(const t::TokenVerifier&) override {
    throw std::logic_error("unused access index");
  }
  t::Outcome<std::optional<t::SessionSecurity>> by_refresh(const t::TokenVerifier&) override {
    throw std::logic_error("unused refresh index");
  }
  t::Outcome<std::optional<t::RefreshHandle>>
  resolve_refresh(const t::TokenVerifier&, const t::RefreshRecoveryLookup&, t::TimePoint) override {
    throw std::logic_error("unused refresh");
  }
  t::Status record_refresh_consumption(const t::ConsumedRefreshHandle&) override {
    throw std::logic_error("proof issuance cannot consume refresh");
  }
  t::Outcome<std::vector<t::SessionMetadata>> list_metadata(const t::SubjectId&,
                                                            std::size_t) override {
    throw std::logic_error("unused list");
  }
  t::Status save(const t::SessionSecurity&, std::optional<t::ResourceVersion>,
                 std::optional<std::uint64_t>) override {
    throw std::logic_error("proof issuance must not mutate session");
  }

private:
  RegistryFixture& registry_;
};
class IssueAuthorizationFixture final : public t::Repository<t::SubjectId, t::AuthorizationState> {
public:
  explicit IssueAuthorizationFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::optional<t::AuthorizationState>> find(const t::SubjectId& id) override {
    if (id == registry_.principal.id || (registry_.runtime && id == registry_.runtime->id)) {
      return std::optional<t::AuthorizationState>{t::AuthorizationState{id, {1}, false}};
    }
    if (!registry_.human || id != registry_.human->id) {
      return std::optional<t::AuthorizationState>{};
    }
    return std::optional<t::AuthorizationState>{t::AuthorizationState{{kSubject}, {1}, false}};
  }
  t::Status save(const t::AuthorizationState&, std::optional<t::ResourceVersion>) override {
    throw std::logic_error("proof cannot change grants");
  }

private:
  RegistryFixture& registry_;
};
class IssueGrantsFixture final : public t::IdentityExactAssignmentRepository {
public:
  explicit IssueGrantsFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::vector<t::ExactAssignment>> for_subject(const t::SubjectId& id) override {
    if (registry_.runtime && id == registry_.runtime->id && !registry_.runtime_capability) {
      return std::vector<t::ExactAssignment>{};
    }
    if (id == registry_.principal.id || (registry_.runtime && id == registry_.runtime->id)) {
      return std::vector<t::ExactAssignment>{t::ExactAssignment{
          {kCredential},
          id,
          {},
          {{t::Permission::IdentityDelegationIssue, {t::ResourceKind::Principal, {id.value}}}},
          {1}}};
    }
    if (id.value != kSubject) {
      return std::vector<t::ExactAssignment>{};
    }
    return registry_.grants;
  }
  t::Status save(const t::ExactAssignment&, std::optional<t::ResourceVersion>) override {
    throw std::logic_error("proof cannot grant");
  }

private:
  RegistryFixture& registry_;
};
class ExchangeSessionMetadataFixture final : public t::Repository<t::SessionId, t::LoginSession> {
public:
  explicit ExchangeSessionMetadataFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<std::optional<t::LoginSession>> find(const t::SessionId& id) override {
    if (!registry_.session || registry_.session->id != id) {
      return std::optional<t::LoginSession>{};
    }
    const auto& session = *registry_.session;
    return std::optional<t::LoginSession>{
        t::LoginSession{session.id, session.subject, session.version, session.rotation,
                        session.refresh_expires_at, session.revoked}};
  }
  t::Status save(const t::LoginSession&, std::optional<t::ResourceVersion>) override {
    throw std::logic_error("exchange must not mutate session metadata");
  }

private:
  RegistryFixture& registry_;
};
class RegistryWorkFixture final : public t::IdentitySecurityUnitOfWork {
public:
  explicit RegistryWorkFixture(RegistryFixture& registry)
      : registry_(registry), principals_(registry), credentials_(registry),
        issue_sessions_(registry), issue_authorizations_(registry), issue_grants_(registry),
        exchange_metadata_(registry) {
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
    return exchange_metadata_;
  }
  t::Repository<t::AssignmentId, t::ResourceGrant>& grants() override {
    throw std::logic_error("authentication must not grant");
  }
  t::Repository<t::SubjectId, t::AuthorizationState>& authorizations() override {
    return issue_authorizations_;
  }
  t::IdentitySessionSecurityRepository& session_security() override {
    return issue_sessions_;
  }
  t::IdentityExactAssignmentRepository& exact_assignments() override {
    return issue_grants_;
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
  IssueSessionRepositoryFixture issue_sessions_;
  IssueAuthorizationFixture issue_authorizations_;
  IssueGrantsFixture issue_grants_;
  ExchangeSessionMetadataFixture exchange_metadata_;
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
  t::IdentityAdminApplication administrator{transactions, verified_certificates};
  auto authenticator =
      std::get<p::PeerAuthenticator>(p::make_identity_peer_authenticator(administrator, clock));
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
class IssueRandomFixture final : public t::RandomPort {
public:
  t::Outcome<t::OwnedSecret> opaque_token() override {
    throw std::logic_error("proof issuance does not create bearer tokens");
  }
  t::Outcome<t::OwnedSecret> bytes(std::size_t count) override {
    // Deterministic TEST entropy; production Core uses its mature random adapter.
    t::OwnedSecret result =
        std::make_unique<FixtureSecret>(std::string(count, static_cast<char>('a' + calls++)));
    return result;
  }
  int calls = 0;
};
class IssueSignerFixture final : public t::DelegationCryptoPort {
public:
  t::Outcome<t::AuthorityVerifiedDelegation>
  verify_for_authorization(t::SecretView, const t::VerifiedPeer&,
                           const t::SecurityClockSample&) override {
    throw std::runtime_error("unexpected authorization crypto call");
  }
  t::Outcome<t::VerifiedDelegation> verify_exchange_parent(t::SecretView, const t::VerifiedPeer&,
                                                           const t::SecurityClockSample&) override {
    throw std::logic_error("proof issuance does not verify an exchange parent");
  }
  IssueSignerFixture(RegistryFixture& registry, ApplicationFixture& sessions)
      : registry_(registry), sessions_(sessions) {}
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims& claims,
                                  const t::CryptoKeyRef&) override {
    require(registry_.active.load() == 0);
    captured = claims;
    ++calls;
    if (mode == 1) {
      return registry_unavailable();
    }
    if (mode == 2) {
      sessions_.issued_source->session.revoked = true;
      registry_.session->revoked = true;
    }
    // TEST signer only: Core orchestration/claims are real, no production signature is claimed.
    t::OwnedSecret result = std::make_unique<FixtureSecret>("signed-proof-fixture");
    return result;
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView, t::Service, const t::VerifiedPeer&,
                                           const t::SecurityClockSample&) override {
    throw std::logic_error("Issue does not exchange a proof");
  }
  std::optional<t::DelegationClaims> captured{};
  int mode = 0;
  int calls = 0;

private:
  RegistryFixture& registry_;
  ApplicationFixture& sessions_;
};
class ExchangeCryptoFixture final : public t::DelegationCryptoPort {
public:
  t::Outcome<t::AuthorityVerifiedDelegation>
  verify_for_authorization(t::SecretView, const t::VerifiedPeer&,
                           const t::SecurityClockSample&) override {
    throw std::logic_error("exchange does not verify authorization queries");
  }
  explicit ExchangeCryptoFixture(RegistryFixture& registry) : registry_(registry) {}
  t::Outcome<t::VerifiedDelegation> verify_exchange_parent(t::SecretView proof,
                                                           const t::VerifiedPeer& recipient,
                                                           const t::SecurityClockSample&) override {
    require(registry_.active.load() == 0 && same_secret(proof, "parent-proof-fixture"));
    require(parent.client_certificate != recipient.binding.certificate.fingerprint);
    ++verified;
    if (mode == 1) {
      return t::Failure{t::ErrorCode::Unauthenticated,
                        t::Acceptance::NotAccepted,
                        t::Recovery::Authenticate,
                        {},
                        {}};
    }
    return t::VerifiedDelegation{parent, {"fixture-parent-key"}};
  }
  t::Outcome<t::VerifiedDelegation> verify(t::SecretView, t::Service, const t::VerifiedPeer&,
                                           const t::SecurityClockSample&) override {
    throw std::logic_error("exchange must use recipient-specific parent verification");
  }
  t::Outcome<t::OwnedSecret> sign(const t::DelegationClaims& claims,
                                  const t::CryptoKeyRef&) override {
    require(registry_.active.load() == 0);
    captured = claims;
    ++signed_count;
    if (mode == 2) {
      return registry_unavailable();
    }
    if (mode == 3) {
      registry_.runtime_capability = false;
    }
    t::OwnedSecret result = std::make_unique<FixtureSecret>("exchange-proof-fixture");
    return result;
  }
  t::DelegationClaims parent{};
  std::optional<t::DelegationClaims> captured{};
  int verified = 0;
  int signed_count = 0;
  int mode = 0;

private:
  RegistryFixture& registry_;
};
void exercise_exchange(const std::string& leaf, const std::string& ca, const std::string& key) {
  const auto certificate = certificate_binding(leaf);
  const t::ResourceRef control_resource{t::ResourceKind::Principal, {kPeer}};
  const auto current = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  RegistryFixture registry{t::Principal{{kPeer},
                                        t::PrincipalKind::PlatformService,
                                        control_resource,
                                        {1},
                                        t::LifecycleState::Active},
                           t::CredentialVersionRecord{{{kCredential}, {1}},
                                                      {kPeer},
                                                      certificate,
                                                      t::CredentialState::Active,
                                                      certificate.not_before,
                                                      std::nullopt,
                                                      control_resource,
                                                      certificate.not_after}};
  registry.human = t::Principal{{kSubject},
                                t::PrincipalKind::Human,
                                t::ResourceRef{t::ResourceKind::Principal, {kSubject}},
                                {1},
                                t::LifecycleState::Active};
  registry.password = t::CredentialVersionRecord{{{kAssignment}, {1}},
                                                 {kSubject},
                                                 t::PasswordVerifier{"fixture-verifier", {1}},
                                                 t::CredentialState::Active,
                                                 current - std::chrono::hours{1},
                                                 std::nullopt,
                                                 {t::ResourceKind::Principal, {kSubject}},
                                                 current + std::chrono::hours{24}};
  registry.session = t::SessionSecurity{{kSession},
                                        {kSubject},
                                        registry.password->ref,
                                        {t::TokenPurpose::Access, {"fixture-key"}, {}},
                                        {t::TokenPurpose::Refresh, {"fixture-key"}, {}},
                                        current - std::chrono::minutes{1},
                                        current + std::chrono::minutes{15},
                                        current + std::chrono::hours{24},
                                        1,
                                        {1},
                                        false};
  const t::ResourceRef first{t::ResourceKind::Account, {kSubject}};
  const t::ResourceRef second{t::ResourceKind::Account, {kOther}};
  registry.grants = {t::ExactAssignment{{kOther},
                                        {kSubject},
                                        {},
                                        {{t::Permission::MessageRead, first},
                                         {t::Permission::ResourceRead, first},
                                         {t::Permission::MessageRead, second},
                                         {t::Permission::ResourceRead, second}},
                                        {1}}};
  ApplicationFixture sessions;
  sessions.issued_source = t::ValidatedSession{*registry.human,
                                               {{kSession},
                                                {kSubject},
                                                registry.session->created_at,
                                                registry.session->refresh_expires_at,
                                                1,
                                                {1},
                                                false},
                                               registry.grants,
                                               {1},
                                               current};
  RegistryFactoryFixture transactions{registry};
  ClockFixture clock{registry};
  namespace crypto = kiarabot::infrastructure::identity_crypto;
  auto actual = std::get<std::unique_ptr<t::PeerCertificatePort>>(
      crypto::make_peer_certificate_crypto({{ca}, crypto::PeerSanKind::Dns}));
  t::IdentityAdminApplication admin{transactions, *actual};
  auto authenticated =
      std::get<p::PeerAuthenticator>(p::make_identity_peer_authenticator(admin, clock));
  IssueRandomFixture random;
  auto runtime_certificate = certificate;
  runtime_certificate.subject = {"88888888-8888-4888-8888-888888888888"};
  runtime_certificate.platform_service = t::Service::Runtime;
  registry.runtime =
      t::Principal{runtime_certificate.subject,
                   t::PrincipalKind::PlatformService,
                   t::ResourceRef{t::ResourceKind::Principal, {runtime_certificate.subject.value}},
                   {1},
                   t::LifecycleState::Active};
  registry.runtime_credential = registry.credential;
  registry.runtime_credential->ref.id = {"99999999-9999-4999-8999-999999999999"};
  registry.runtime_credential->subject = runtime_certificate.subject;
  registry.runtime_credential->material = runtime_certificate;
  registry.runtime_credential->binding = {t::ResourceKind::Principal,
                                          {runtime_certificate.subject.value}};
  // The parent carries the previous Control fingerprint, distinct from Runtime's TLS leaf.
  auto& control_certificate = std::get<t::CertificateBinding>(registry.credential.material);
  control_certificate.fingerprint.bytes.front() ^= std::byte{1};
  ExchangeCryptoFixture signer{registry};
  signer.parent = t::DelegationClaims{"issuer-fixture",
                                      t::Service::Runtime,
                                      {kSubject},
                                      {kPeer},
                                      {{kPeer}},
                                      registry.grants.front().explicit_permissions,
                                      {kOther},
                                      control_certificate.fingerprint,
                                      {1},
                                      current - std::chrono::seconds{1},
                                      current - std::chrono::seconds{1},
                                      current + std::chrono::seconds{55},
                                      1,
                                      registry.password->ref,
                                      t::SessionId{kSession}};
  t::IdentityExchangeApplication exchange{
      transactions, signer, random, clock, {"issuer-fixture", {"signing-fixture"}}};
  auto service = std::get<std::unique_ptr<w::IdentityService::Service>>(
      p::make_identity_rpc(sessions, authenticated, nullptr, {}, {.exchange = &exchange}));
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
  w::IdentityExchangeDelegationRequest request;
  request_context(request.mutable_context());
  request.mutable_context()->clear_idempotency_key();
  request.set_parent_proof("parent-proof-fixture");
  request.set_audience("model");
  request.set_client_certificate_sha256(
      std::string(reinterpret_cast<const char*>(certificate.fingerprint.bytes.data()), 32));
  auto* resource = request.mutable_scope()->add_resources();
  resource->set_kind(w::RESOURCE_ACCOUNT);
  resource->set_id(kSubject);
  request.add_permissions(w::PERMISSION_MESSAGE_READ);
  w::IdentityExchangeDelegationResponse response;
  for (const auto* target : {"model", "scheduler"}) {
    request.set_audience(target);
    require(call(*stub, &w::IdentityService::Stub::ExchangeDelegation, request, &response).ok());
    require(response.delegation().proof() == "exchange-proof-fixture" &&
            response.delegation().audience() == target &&
            response.delegation().business_hops() == 2);
    require(signer.captured && signer.captured->permissions.size() == 1 &&
            signer.captured->actor_chain ==
                std::vector<t::SubjectId>{{kPeer}, runtime_certificate.subject} &&
            signer.captured->client_certificate == certificate.fingerprint &&
            signer.captured->source_credential == signer.parent.source_credential &&
            signer.captured->source_session == signer.parent.source_session &&
            signer.captured->subject == signer.parent.subject &&
            signer.captured->expires_at <= signer.parent.expires_at);
  }
  const auto reject = [&](const w::IdentityExchangeDelegationRequest& input,
                          grpc::StatusCode code) {
    response.Clear();
    const auto status =
        call(*stub, &w::IdentityService::Stub::ExchangeDelegation, input, &response);
    require(status.error_code() == code && response.ByteSizeLong() == 0);
    w::RpcFailure detail;
    require(detail.ParseFromString(status.error_details()) && detail.has_error());
  };
  // Positive full budget, then 129 tuples rejected before any crypto/random call.
  auto bounded = request;
  bounded.mutable_scope()->clear_resources();
  bounded.add_permissions(w::PERMISSION_RESOURCE_READ);
  const auto original_parent = signer.parent;
  const auto original_grants = registry.grants;
  registry.grants.front().explicit_permissions.clear();
  for (int index = 0; index < 64; ++index) {
    const std::string id = "77777777-7777-4777-8777-00000" + std::to_string(1'000'000 + index);
    auto* item = bounded.mutable_scope()->add_resources();
    item->set_kind(w::RESOURCE_ACCOUNT);
    item->set_id(id);
    for (auto permission : {t::Permission::MessageRead, t::Permission::ResourceRead}) {
      registry.grants.front().explicit_permissions.push_back(
          {permission, {t::ResourceKind::Account, {id}}});
    }
  }
  signer.parent.permissions = registry.grants.front().explicit_permissions;
  require(call(*stub, &w::IdentityService::Stub::ExchangeDelegation, bounded, &response).ok());
  require(signer.captured->permissions.size() == 128);
  auto overflow = bounded;
  overflow.mutable_permissions()->RemoveLast();
  for (int index = 64; index < 129; ++index) {
    auto* item = overflow.mutable_scope()->add_resources();
    item->set_kind(w::RESOURCE_ACCOUNT);
    item->set_id("77777777-7777-4777-8777-00000" + std::to_string(1'000'000 + index));
  }
  const int verified_before_overflow = signer.verified;
  const int signed_before_overflow = signer.signed_count;
  const int random_before_overflow = random.calls;
  reject(overflow, grpc::StatusCode::INVALID_ARGUMENT);
  require(signer.verified == verified_before_overflow &&
          signer.signed_count == signed_before_overflow && random.calls == random_before_overflow);
  signer.parent = original_parent;
  registry.grants = original_grants;
  const int before_verify = signer.verified;
  for (const auto* target : {"identity", "runtime", "host", "control", "unknown"}) {
    auto invalid = request;
    invalid.set_audience(target);
    reject(invalid, grpc::StatusCode::INVALID_ARGUMENT);
  }
  for (const auto& proof :
       {std::string{}, std::string{"Bearer parent-proof-fixture"}, std::string(32769, 'x')}) {
    auto invalid = request;
    invalid.set_parent_proof(proof);
    reject(invalid, grpc::StatusCode::INVALID_ARGUMENT);
  }
  auto invalid = request;
  invalid.set_client_certificate_sha256(std::string(32, 'x'));
  reject(invalid, grpc::StatusCode::INVALID_ARGUMENT);
  invalid = request;
  invalid.add_permissions(w::PERMISSION_MESSAGE_READ);
  reject(invalid, grpc::StatusCode::INVALID_ARGUMENT);
  invalid = request;
  for (int index = 0; index < 128; ++index) {
    *invalid.mutable_scope()->add_resources() = request.scope().resources(0);
  }
  reject(invalid, grpc::StatusCode::INVALID_ARGUMENT);
  require(signer.verified == before_verify);
  auto widened = request;
  widened.mutable_scope()->mutable_resources(0)->set_id(kCredential);
  reject(widened, grpc::StatusCode::PERMISSION_DENIED);
  auto& runtime_material = std::get<t::CertificateBinding>(registry.runtime_credential->material);
  runtime_material.platform_service = t::Service::Control;
  reject(request, grpc::StatusCode::PERMISSION_DENIED);
  runtime_material.platform_service = t::Service::Runtime;
  signer.mode = 1;
  reject(request, grpc::StatusCode::UNAUTHENTICATED);
  signer.mode = 2;
  reject(request, grpc::StatusCode::UNAVAILABLE);
  signer.mode = 0;
  registry.runtime_capability = false;
  reject(request, grpc::StatusCode::PERMISSION_DENIED);
  registry.runtime_capability = true;
  signer.mode = 3;
  reject(request, grpc::StatusCode::PERMISSION_DENIED);
  registry.runtime_capability = true;
  signer.mode = 0;
  registry.session->revoked = true;
  reject(request, grpc::StatusCode::UNAUTHENTICATED);
  // Core also permits non-human provenance without a login session; the boundary
  // must preserve optional absence rather than imposing Issue's human-session shape.
  registry.human->kind = t::PrincipalKind::PlatformService;
  auto source_certificate = certificate;
  source_certificate.subject = {kSubject};
  registry.password->material = source_certificate;
  registry.password->created_at = certificate.not_before;
  registry.password->expires_at = certificate.not_after;
  signer.parent.source_session.reset();
  require(call(*stub, &w::IdentityService::Stub::ExchangeDelegation, request, &response).ok());
  require(signer.captured && !signer.captured->source_session);
  require(sessions.calls.load() == 0);
  server->Shutdown();
  server->Wait();
}
void exercise_issue(const std::string& leaf, const std::string& ca, const std::string& key) {
  const auto certificate = certificate_binding(leaf);
  const t::ResourceRef control_resource{t::ResourceKind::Principal, {kPeer}};
  const auto current = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  RegistryFixture registry{t::Principal{{kPeer},
                                        t::PrincipalKind::PlatformService,
                                        control_resource,
                                        {1},
                                        t::LifecycleState::Active},
                           t::CredentialVersionRecord{{{kCredential}, {1}},
                                                      {kPeer},
                                                      certificate,
                                                      t::CredentialState::Active,
                                                      certificate.not_before,
                                                      std::nullopt,
                                                      control_resource,
                                                      certificate.not_after}};
  registry.human = t::Principal{{kSubject},
                                t::PrincipalKind::Human,
                                t::ResourceRef{t::ResourceKind::Principal, {kSubject}},
                                {1},
                                t::LifecycleState::Active};
  registry.password = t::CredentialVersionRecord{{{kAssignment}, {1}},
                                                 {kSubject},
                                                 t::PasswordVerifier{"fixture-verifier", {1}},
                                                 t::CredentialState::Active,
                                                 current - std::chrono::hours{1},
                                                 std::nullopt,
                                                 {t::ResourceKind::Principal, {kSubject}},
                                                 current + std::chrono::hours{24}};
  registry.session = t::SessionSecurity{{kSession},
                                        {kSubject},
                                        registry.password->ref,
                                        {t::TokenPurpose::Access, {"fixture-key"}, {}},
                                        {t::TokenPurpose::Refresh, {"fixture-key"}, {}},
                                        current - std::chrono::minutes{1},
                                        current + std::chrono::minutes{15},
                                        current + std::chrono::hours{24},
                                        1,
                                        {1},
                                        false};
  const t::ResourceRef first{t::ResourceKind::Account, {kSubject}};
  const t::ResourceRef second{t::ResourceKind::Account, {kOther}};
  registry.grants = {t::ExactAssignment{{kOther},
                                        {kSubject},
                                        {},
                                        {{t::Permission::MessageRead, first},
                                         {t::Permission::ResourceRead, first},
                                         {t::Permission::MessageRead, second},
                                         {t::Permission::ResourceRead, second}},
                                        {1}}};
  ApplicationFixture sessions;
  sessions.issued_source = t::ValidatedSession{*registry.human,
                                               {{kSession},
                                                {kSubject},
                                                registry.session->created_at,
                                                registry.session->refresh_expires_at,
                                                1,
                                                {1},
                                                false},
                                               registry.grants,
                                               {1},
                                               current};
  RegistryFactoryFixture transactions{registry};
  ClockFixture clock{registry};
  namespace crypto = kiarabot::infrastructure::identity_crypto;
  auto actual = std::get<std::unique_ptr<t::PeerCertificatePort>>(
      crypto::make_peer_certificate_crypto({{ca}, crypto::PeerSanKind::Dns}));
  t::IdentityAdminApplication admin{transactions, *actual};
  auto authenticated =
      std::get<p::PeerAuthenticator>(p::make_identity_peer_authenticator(admin, clock));
  IssueRandomFixture random;
  IssueSignerFixture signer{registry, sessions};
  t::IdentityDelegationApplication issuer{
      sessions, transactions, signer, random, clock, {"issuer-fixture", {"signing-fixture"}}};
  auto service = std::get<std::unique_ptr<w::IdentityService::Service>>(
      p::make_identity_rpc(sessions, authenticated, &issuer));
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
  w::IdentityIssueDelegationRequest request;
  request_context(request.mutable_context());
  request.mutable_context()->clear_idempotency_key();
  request.set_subject_proof("access-fixture-secret");
  request.set_audience("identity");
  request.set_client_certificate_sha256(
      std::string(reinterpret_cast<const char*>(certificate.fingerprint.bytes.data()),
                  certificate.fingerprint.bytes.size()));
  for (const auto* id : {kSubject, kOther}) {
    auto* resource = request.mutable_scope()->add_resources();
    resource->set_kind(w::RESOURCE_ACCOUNT);
    resource->set_id(id);
  }
  request.add_permissions(w::PERMISSION_MESSAGE_READ);
  request.add_permissions(w::PERMISSION_RESOURCE_READ);
  w::IdentityIssueDelegationResponse response;
  require(call(*stub, &w::IdentityService::Stub::IssueDelegation, request, &response).ok());
  require(response.delegation().proof() == "signed-proof-fixture" &&
          response.delegation().audience() == "identity" &&
          response.delegation().business_hops() == 1);
  require(signer.captured && signer.captured->permissions.size() == 4 &&
          signer.captured->source_session->value == kSession &&
          signer.captured->source_credential == registry.password->ref);
  require(signer.captured->client_certificate == certificate.fingerprint &&
          signer.captured->actor_chain == std::vector<t::SubjectId>{{kPeer}});
  require(signer.captured->expires_at - signer.captured->issued_at <= std::chrono::seconds{56});
  const auto first_proof_id = signer.captured->proof_id;
  require(call(*stub, &w::IdentityService::Stub::IssueDelegation, request, &response).ok());
  require(signer.captured->proof_id !=
          first_proof_id); // reauthorized, not a secret success receipt.
  // 64 exact resources x 2 permissions = the complete 128-binding budget.
  auto bounded = request;
  bounded.mutable_scope()->clear_resources();
  const auto original_grants = registry.grants;
  auto& grant = registry.grants.front();
  grant.explicit_permissions.clear();
  for (int index = 0; index < 64; ++index) {
    std::string suffix = std::to_string(1'000'000 + index);
    const std::string id = "77777777-7777-4777-8777-00000" + suffix;
    auto* resource = bounded.mutable_scope()->add_resources();
    resource->set_kind(w::RESOURCE_ACCOUNT);
    resource->set_id(id);
    for (const auto permission : {t::Permission::MessageRead, t::Permission::ResourceRead}) {
      grant.explicit_permissions.push_back({permission, {t::ResourceKind::Account, {id}}});
    }
  }
  sessions.issued_source->assignments = registry.grants;
  require(call(*stub, &w::IdentityService::Stub::IssueDelegation, bounded, &response).ok());
  require(signer.captured->permissions.size() == 128);
  const auto before_source = sessions.calls.load();
  const auto before_sign = signer.calls;
  const auto before_random = random.calls;
  // 129 unique resources (one permission) reject before even decoding/allocating scopes.
  auto oversized = bounded;
  oversized.clear_permissions();
  oversized.add_permissions(w::PERMISSION_MESSAGE_READ);
  for (int index = 64; index < 129; ++index) {
    auto* resource = oversized.mutable_scope()->add_resources();
    resource->set_kind(w::RESOURCE_ACCOUNT);
    resource->set_id("77777777-7777-4777-8777-00000" + std::to_string(1'000'000 + index));
  }
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, oversized, &response),
           w::INVALID_ARGUMENT);
  // 43 resources x 3 distinct permissions = exactly 129 tuples, before any owner call.
  auto one_over = request;
  one_over.mutable_scope()->clear_resources();
  for (int index = 0; index < 43; ++index) {
    *one_over.mutable_scope()->add_resources() = bounded.scope().resources(index);
  }
  one_over.add_permissions(w::PERMISSION_ACTION_EXECUTE);
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, one_over, &response),
           w::INVALID_ARGUMENT);
  // Individually bounded input counts can still exceed the product budget: 65 x 2.
  auto product_overflow = bounded;
  auto* extra = product_overflow.mutable_scope()->add_resources();
  extra->set_kind(w::RESOURCE_ACCOUNT);
  extra->set_id(kOther);
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, product_overflow, &response),
           w::INVALID_ARGUMENT);
  auto too_many_permissions = request;
  while (too_many_permissions.permissions_size() <= 40) {
    too_many_permissions.add_permissions(w::PERMISSION_MESSAGE_READ);
  }
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, too_many_permissions, &response),
           w::INVALID_ARGUMENT);
  require(sessions.calls.load() == before_source && signer.calls == before_sign &&
          random.calls == before_random);
  registry.grants = original_grants;
  sessions.issued_source->assignments = registry.grants;
  const auto calls_before = signer.calls;
  for (int mode = 0; mode < 10; ++mode) {
    auto bad = request;
    if (mode == 0) {
      bad.set_subject_proof("Bearer access-fixture-secret");
    }
    if (mode == 1) {
      bad.set_audience("all");
    }
    if (mode == 2) {
      bad.clear_scope();
    }
    if (mode == 3) {
      bad.add_permissions(w::PERMISSION_UNSPECIFIED);
    }
    if (mode == 4) {
      bad.add_permissions(w::PERMISSION_MESSAGE_READ);
    }
    if (mode == 5) {
      *bad.mutable_scope()->add_resources() = bad.scope().resources(0);
    }
    if (mode == 6) {
      bad.set_client_certificate_sha256(std::string(32, 'x'));
    }
    if (mode == 7) {
      bad.mutable_scope()->mutable_resources(0)->set_kind(w::RESOURCE_KIND_UNSPECIFIED);
    }
    if (mode == 8) {
      bad.mutable_permissions()->Add(999);
    }
    if (mode == 9) {
      bad.clear_permissions();
    }
    rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, bad, &response),
             w::INVALID_ARGUMENT);
  }
  require(signer.calls == calls_before);
  auto denied = request;
  denied.add_permissions(w::PERMISSION_ACTION_EXECUTE);
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, denied, &response),
           w::PERMISSION_DENIED);
  require(signer.calls == calls_before);
  auto wrong_purpose = request;
  wrong_purpose.set_subject_proof("signed-proof-fixture");
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, wrong_purpose, &response),
           w::UNAUTHENTICATED);
  signer.mode = 1;
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, request, &response),
           w::UPSTREAM_UNAVAILABLE);
  signer.mode = 2;
  rejected(call(*stub, &w::IdentityService::Stub::IssueDelegation, request, &response),
           w::UNAUTHENTICATED);
  require(registry.active.load() == 0);
  server->Shutdown();
  server->Wait();
}
// Complete Core application TEST double: the real 151f31e application owns current
// platform/Catalog authorization. The transport must neither cache nor JWT-precheck it.
class VerificationKeysApplicationFixture final : public t::IdentityVerificationKeysApplication {
public:
  std::atomic<int> mode{0};
  std::atomic<int> calls{0};
  std::atomic<std::uint64_t> current_version{std::numeric_limits<std::uint64_t>::max()};
  std::atomic<std::uint64_t> last_known{0};
  t::Outcome<t::GetVerificationKeysReply>
  get_verification_keys(const t::GetVerificationKeysRequest& request) override {
    ++calls;
    const auto* platform = std::get_if<t::PlatformPeer>(&request.caller.binding.peer);
    require(platform && platform->service == t::Service::Runtime &&
            platform->subject.value == kPeer);
    require(request.context.request_id == kSubject && request.context.trace_id == kOther);
    last_known.store(request.known_version ? request.known_version->value : 0);
    // Explicit fake current-authority denial precedes the known-version comparison.
    if (mode.load() == 1) {
      return t::Failure{t::ErrorCode::PermissionDenied,
                        t::Acceptance::NotAccepted,
                        t::Recovery::Diagnose,
                        {},
                        {}};
    }
    if (mode.load() == 2 || mode.load() == 3) {
      return registry_unavailable();
    }
    if (mode.load() == 12) {
      throw std::runtime_error("never-export-private-key-path-or-exception");
    }
    if (request.known_version && request.known_version->value > current_version.load()) {
      return t::Failure{t::ErrorCode::InvalidArgument,
                        t::Acceptance::NotAccepted,
                        t::Recovery::CorrectInput,
                        {},
                        {}};
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    const auto checked =
        t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(seconds) +
                     std::chrono::nanoseconds{123456789}};
    t::PublicVerificationKey key{{"active-public-key"},
                                 {},
                                 {std::numeric_limits<std::uint64_t>::max() - 1},
                                 checked + std::chrono::hours{1}};
    for (std::size_t i = 0; i < key.ed25519_public_key.size(); ++i) {
      key.ed25519_public_key[i] = static_cast<std::byte>(i);
    }
    t::GetVerificationKeysReply result{
        {{key}, {current_version.load()}, {t::ResourceKind::Catalog, {kOther}}}, checked};
    if (mode.load() == 4) {
      result.keys.keys[0].version.value = 0;
    }
    if (mode.load() == 5) {
      result.keys.version.value = 0;
    }
    if (mode.load() == 6) {
      result.keys.keys[0].valid_until = t::TimePoint::max();
    }
    if (mode.load() == 7) {
      result.keys.keys.clear();
    }
    if (mode.load() == 8) {
      result.checked_at = {};
    }
    if (mode.load() == 9) {
      result.keys.keys[0].valid_until = checked;
    }
    if (mode.load() == 10) {
      result.keys.keys[0].reference.value.clear();
    }
    if (mode.load() == 11) {
      result.keys.keyset_resource.kind = t::ResourceKind::Account;
    }
    if (mode.load() == 13) {
      result.keys.keys.push_back(key);
    }
    return result;
  }
};
void exercise_verification_keys(const std::string& leaf, const std::string& ca,
                                const std::string& key,
                                p::PeerAuthenticator authenticated_fixture) {
  ApplicationFixture sessions;
  VerificationKeysApplicationFixture application;
  std::atomic<bool> lifecycle_unavailable{false};
  p::InvocationDeadline lifecycle = [&]() -> t::Outcome<t::TimePoint> {
    if (lifecycle_unavailable.load()) {
      return registry_unavailable();
    }
    return t::TimePoint::max();
  };
  // This explicit registry fixture binds Runtime to the real TLS leaf; it is not production
  // enrollment/SAN authority. It demonstrates this endpoint does not require a Control role/JWT.
  p::PeerAuthenticator runtime_fixture =
      [&](grpc::ServerContext& context) -> t::Outcome<t::VerifiedPeer> {
    auto verified = authenticated_fixture(context);
    if (const auto* error = std::get_if<t::Failure>(&verified)) {
      return *error;
    }
    auto peer = std::get<t::VerifiedPeer>(std::move(verified));
    std::get<t::PlatformPeer>(peer.binding.peer).service = t::Service::Runtime;
    peer.binding.certificate.platform_service = t::Service::Runtime;
    return peer;
  };
  auto service = std::get<std::unique_ptr<w::IdentityService::Service>>(
      p::make_identity_rpc(sessions, runtime_fixture, nullptr, lifecycle,
                           p::IdentityRpcOptions{.verification_keys = &application}));
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
  w::IdentityGetVerificationKeysRequest request;
  request_context(request.mutable_context());
  request.mutable_context()->clear_idempotency_key();
  request.mutable_context()->clear_delegation();
  w::IdentityGetVerificationKeysResponse response;
  require(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response).ok());
  require(application.last_known.load() == 0 &&
          response.version() == std::numeric_limits<std::uint64_t>::max());
  require(response.keys_size() == 1 &&
          response.keys(0).version() == std::numeric_limits<std::uint64_t>::max() - 1);
  require(response.keys(0).key_id() == "active-public-key" &&
          response.keys(0).ed25519_public_key().size() == 32);
  for (int i = 0; i < 32; ++i) {
    require(response.keys(0).ed25519_public_key()[i] == static_cast<char>(i));
  }
  require(response.keys(0).valid_until().nanos() == 123456789);
  request.set_known_key_version(std::numeric_limits<std::uint64_t>::max());
  // Unknown-kid proof must not trigger a JWT bootstrap loop in the transport.
  request.mutable_context()->set_delegation("unverifiable.unknown-kid.signature");
  require(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response).ok());
  require(application.last_known.load() == std::numeric_limits<std::uint64_t>::max() &&
          response.keys_size() == 1);
  application.current_version.store(9);
  for (const std::uint64_t known : {8, 9}) {
    request.set_known_key_version(known);
    require(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response).ok());
    require(response.version() == 9 && response.keys_size() == 1);
  }
  request.set_known_key_version(10);
  application.mode.store(1);
  rejected(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response),
           w::PERMISSION_DENIED);
  application.mode.store(0);
  rejected(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response),
           w::INVALID_ARGUMENT);
  const auto before_zero = application.calls.load();
  request.set_known_key_version(0);
  rejected(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response),
           w::INVALID_ARGUMENT);
  require(application.calls.load() == before_zero);
  request.clear_known_key_version();
  for (int mode : {2, 3}) {
    application.mode.store(mode);
    const auto unavailable =
        rejected(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response),
                 w::UPSTREAM_UNAVAILABLE);
    require(unavailable.context().acceptance() == w::NOT_ACCEPTED);
  }
  for (int mode : {4, 5, 6, 7, 8, 9, 10, 11, 12, 13}) {
    application.mode.store(mode);
    rejected(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response),
             w::INTERNAL);
  }
  application.mode.store(0);
  const auto before_lifecycle = application.calls.load();
  lifecycle_unavailable.store(true);
  const auto unavailable =
      rejected(call(*stub, &w::IdentityService::Stub::GetVerificationKeys, request, &response),
               w::UPSTREAM_UNAVAILABLE);
  require(unavailable.context().acceptance() == w::NOT_ACCEPTED &&
          application.calls.load() == before_lifecycle);
  const auto before_missing_peer = application.calls.load();
  grpc::SslCredentialsOptions missing;
  missing.pem_root_certs = ca;
  auto missing_stub = w::IdentityService::NewStub(
      grpc::CreateChannel("localhost:" + std::to_string(port), grpc::SslCredentials(missing)));
  grpc::ClientContext missing_context;
  missing_context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds{200});
  require(!missing_stub->GetVerificationKeys(&missing_context, request, &response).ok());
  require(application.calls.load() == before_missing_peer);
  require(sessions.calls.load() == 0); // no JWT/session or unused issuer preflight.
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
    const auto* descriptor = google::protobuf::DescriptorPool::generated_pool()->FindServiceByName(
        "kiarabot.v1.IdentityService");
    require(descriptor != nullptr);
    require(!descriptor->FindMethodByName("IssueDelegation")
                 ->options()
                 .GetExtension(w::requires_idempotency));
    require(!descriptor->FindMethodByName("ExchangeDelegation")
                 ->options()
                 .GetExtension(w::requires_idempotency));
    for (const auto* name : {"Login", "RefreshSession", "RevokeSession", "RotateCredential"}) {
      require(descriptor->FindMethodByName(name)->options().GetExtension(w::requires_idempotency));
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
    std::atomic<int> lifecycle_mode{0};
    std::atomic<std::int64_t> lifecycle_limit{0};
    // Lifecycle deadline TEST callback, not a certificate-expiry implementation/production clock.
    p::InvocationDeadline lifecycle = [&]() -> t::Outcome<t::TimePoint> {
      if (lifecycle_mode.load() == 1) {
        return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
            (std::chrono::system_clock::now() - std::chrono::seconds{1}).time_since_epoch())};
      }
      if (lifecycle_mode.load() == 2) {
        return t::TimePoint{std::chrono::nanoseconds{lifecycle_limit.load()}};
      }
      if (lifecycle_mode.load() == 3) {
        return t::Failure{t::ErrorCode::UpstreamUnavailable,
                          t::Acceptance::NotAccepted,
                          t::Recovery::Diagnose,
                          {},
                          std::chrono::milliseconds{123}};
      }
      if (lifecycle_mode.load() == 4) {
        throw std::runtime_error("never-return-own-TLS-secret");
      }
      if (lifecycle_mode.load() == 5) {
        return t::TimePoint{};
      }
      return t::TimePoint::max();
    };
    auto service = std::get<std::unique_ptr<w::IdentityService::Service>>(
        p::make_identity_rpc(app, verifier, nullptr, lifecycle));
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
    exercise_invocation_deadline(*stub, app, lifecycle_mode, lifecycle_limit);
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
    exercise_issue(leaf, ca, key);
    exercise_exchange(leaf, ca, key);
    exercise_verification_keys(leaf, ca, key, verifier);
    std::cout << "Six Identity RPCs over real mTLS: boundary projections, stable failures, unknown "
                 "commit, peer/deadline rejection and Core Issue/Exchange passed "
                 "(repository/signing fixtures "
                 "only)\n";
    return 0;
  } catch (...) {
    std::cerr << "Identity RPC fixture assertion line " << failed_line.load()
              << "; diagnostics redacted\n";
    return 1;
  }
}
