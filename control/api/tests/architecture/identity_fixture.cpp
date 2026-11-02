#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "kiarabot/v1/identity.grpc.pb.h"

namespace {
namespace wire = kiarabot::v1;
constexpr char kSession[] = "11111111-1111-4111-8111-111111111111";
constexpr char kThird[] = "44444444-4444-4444-8444-444444444444";
constexpr char kOther[] = "22222222-2222-4222-8222-222222222222";
constexpr char kKey[] = "client-retry-key-0001";
constexpr std::int64_t kChecked = 1700000000;
std::string read(const char* path) {
  std::ifstream input(path);
  std::ostringstream result;
  result << input.rdbuf();
  return result.str();
}
grpc::Status error(wire::ErrorCode code, grpc::StatusCode grpc_code,
                   const wire::RequestContext& context) {
  wire::RpcFailure failure;
  failure.mutable_context()->set_acceptance(wire::NOT_ACCEPTED);
  failure.mutable_context()->set_recovery(wire::DO_NOT_RETRY);
  failure.mutable_error()->set_code(code);
  failure.mutable_error()->set_request_id(context.request_id());
  failure.mutable_error()->set_trace_id(context.trace_id());
  failure.mutable_error()->set_message("never-print-upstream-secret");
  return {grpc_code, "never-print-upstream-secret", failure.SerializeAsString()};
}
void session(wire::SessionRecord* output, const std::string& id, bool revoked = false) {
  output->set_session_id(id);
  output->set_principal_id("33333333-3333-4333-8333-333333333333");
  output->set_rotation(1);
  output->set_revoked(revoked);
  output->mutable_created_at()->set_seconds(kChecked - 60);
  output->mutable_expires_at()->set_seconds(id == kOther ? kChecked - 1 : kChecked + 600);
}
template <typename Response> void tokens(Response* output, const std::string& suffix) {
  output->mutable_checked_at()->set_seconds(kChecked);
  output->mutable_checked_at()->set_nanos(500000000);
  auto* pair = output->mutable_tokens();
  pair->set_session_id(kSession);
  pair->set_access_token("secret-access-credential-" + suffix);
  pair->set_refresh_token("secret-refresh-credential-" + suffix);
  pair->set_rotation(1);
  pair->mutable_access_expires_at()->set_seconds(kChecked + 900);
  pair->mutable_access_expires_at()->set_nanos(100000000);
  pair->mutable_refresh_expires_at()->set_seconds(kChecked + 2592000);
}

struct AuditRow {
  std::string id;
  std::string actor = "alice";
  std::string request = "request";
  std::string trace = "trace";
  std::string method = "List";
  std::int64_t nanos = 0;
  std::string severity = "info";
  std::string message = "request.recorded";
};
std::vector<AuditRow> audit_rows() {
  return {{"10", "alice", "request-10", "trace-10", "Login", 5'000'000'000},
          {"9", "alice", "request-9", "trace-9", "RefreshSession", 4'000'000'000},
          {"30", "alice", "request-30", "trace-30", "GetPrincipalStatus", 3'000'000'000},
          {"8", "alice", "request-8", "trace-8", "RevokeSession", 1'000'000'000}};
}
std::int64_t timestamp_nanos(const google::protobuf::Timestamp& value) {
  return value.seconds() * 1'000'000'000LL + value.nanos();
}
std::optional<std::pair<std::int64_t, std::string>> audit_token(std::string_view value) {
  const auto colon = value.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  std::int64_t nanos = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + colon, nanos);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + colon) {
    return std::nullopt;
  }
  return std::pair{nanos, std::string{value.substr(colon + 1)}};
}
std::vector<AuditRow>
filter_rows(const wire::IdentityAuditFilter& filter) {
  std::vector<AuditRow> rows;
  const auto cursor = filter.page().page_token().empty()
                          ? std::optional<std::pair<std::int64_t, std::string>>{}
                          : audit_token(filter.page().page_token());
  for (const auto& row : audit_rows()) {
    if (!filter.actor().empty() && row.actor != filter.actor()) {
      continue;
    }
    if (filter.has_resource() &&
        (filter.resource().kind() != wire::RESOURCE_CATALOG ||
         filter.resource().id() != "catalog")) {
      continue;
    }
    if (filter.has_occurred_from() && row.nanos < timestamp_nanos(filter.occurred_from())) {
      continue;
    }
    if (filter.has_occurred_until() && row.nanos >= timestamp_nanos(filter.occurred_until())) {
      continue;
    }
    if (cursor && (row.nanos > cursor->first ||
                   (row.nanos == cursor->first && row.id >= cursor->second))) {
      continue;
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(), [](const AuditRow& left, const AuditRow& right) {
    return left.nanos != right.nanos ? left.nanos > right.nanos : left.id > right.id;
  });
  return rows;
}
void fill_audit_row(const AuditRow& row, wire::IdentityAuditRecord* output) {
  output->set_entry_id(row.id);
  output->set_actor(row.actor);
  output->mutable_resource()->set_kind(wire::RESOURCE_CATALOG);
  output->mutable_resource()->set_id("catalog");
  output->set_request_id(row.request);
  output->set_trace_id(row.trace);
  output->set_method(row.method);
  output->set_rejection(wire::ERROR_CODE_UNSPECIFIED);
  output->set_authorization_version(7);
  output->mutable_occurred_at()->set_seconds(row.nanos / 1'000'000'000LL);
  output->mutable_occurred_at()->set_nanos(static_cast<std::int32_t>(row.nanos % 1'000'000'000LL));
}
void fill_log_row(const AuditRow& row, wire::IdentityLogRecord* output) {
  output->set_entry_id(row.id);
  output->set_severity(row.severity);
  output->set_component("identity");
  output->set_method(row.method);
  output->set_actor(row.actor);
  output->mutable_resource()->set_kind(wire::RESOURCE_CATALOG);
  output->mutable_resource()->set_id("catalog");
  output->set_request_id(row.request);
  output->set_trace_id(row.trace);
  output->set_message(row.message);
  output->mutable_occurred_at()->set_seconds(row.nanos / 1'000'000'000LL);
  output->mutable_occurred_at()->set_nanos(static_cast<std::int32_t>(row.nanos % 1'000'000'000LL));
}

class Fixture final : public wire::IdentityService::Service {
public:
  grpc::Status Login(grpc::ServerContext* context, const wire::IdentityLoginRequest* input,
                     wire::IdentityLoginResponse* output) override {
    if (!context->auth_context()->IsPeerAuthenticated()) {
      return error(wire::UNAUTHENTICATED, grpc::StatusCode::UNAUTHENTICATED, input->context());
    }
    if (input->username() == "slow") {
      while (!context->IsCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
      }
      return {grpc::StatusCode::DEADLINE_EXCEEDED, ""};
    }
    if (input->username() == "unavailable") {
      return {grpc::StatusCode::UNAVAILABLE, "never-print-upstream-secret"};
    }
    if (input->username() == "plugin_failure" || input->username() == "typed_timeout" ||
        input->username() == "bad_status") {
      const auto code =
          input->username() == "typed_timeout" ? wire::TIMEOUT : wire::PLUGIN_UNAVAILABLE;
      const auto category = input->username() == "typed_timeout"
                                ? grpc::StatusCode::DEADLINE_EXCEEDED
                                : grpc::StatusCode::UNAVAILABLE;
      wire::RpcFailure failure;
      if (!failure.ParseFromString(error(code, category, input->context()).error_details())) {
        return {grpc::StatusCode::INTERNAL, ""};
      }
      failure.mutable_error()->set_retryable(true);
      auto* failure_context = failure.mutable_context();
      failure_context->set_acceptance(wire::ACCEPTANCE_UNKNOWN);
      failure_context->set_recovery(wire::QUERY_SAME_INTENT);
      failure_context->set_retry_after_ms(2500);
      failure_context->set_current_version(18446744073709551615ULL);
      auto* operation = failure_context->mutable_operation();
      operation->set_owner_service("runtime");
      operation->set_operation_id("77777777-7777-4777-8777-777777777777");
      operation->mutable_resource()->set_kind(wire::RESOURCE_SESSION);
      operation->mutable_resource()->set_id(kSession);
      return {input->username() == "bad_status" ? grpc::StatusCode::PERMISSION_DENIED : category,
              "never-print-upstream-secret", failure.SerializeAsString()};
    }
    if (input->username() == "no_context" || input->username() == "unknown_context") {
      wire::RpcFailure failure;
      failure.mutable_error()->set_code(wire::PERMISSION_DENIED);
      if (input->username() == "unknown_context") {
        auto* failure_context = failure.mutable_context();
        const auto* acceptance = failure_context->GetDescriptor()->FindFieldByName("acceptance");
        if (acceptance == nullptr) {
          return {grpc::StatusCode::INTERNAL, ""};
        }
        failure_context->GetReflection()->SetEnumValue(failure_context, acceptance, 999);
        failure.mutable_context()->set_recovery(wire::DO_NOT_RETRY);
      }
      return {grpc::StatusCode::PERMISSION_DENIED, "", failure.SerializeAsString()};
    }
    if (input->username() == "no_detail") {
      return {grpc::StatusCode::PERMISSION_DENIED, "never-print-upstream-secret"};
    }
    if (input->username() == "denied") {
      return error(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED, input->context());
    }
    if (input->username() == "limited") {
      return error(wire::RATE_LIMITED, grpc::StatusCode::RESOURCE_EXHAUSTED, input->context());
    }
    if (input->password() != "never-print-password" || input->context().idempotency_key() != kKey ||
        !input->context().delegation().empty() || !input->context().has_deadline() ||
        input->context().trace_id() != input->context().request_id() ||
        input->context().request_id().empty()) {
      return error(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    if (input->username() == "missing_time") {
      output->mutable_tokens();
      return grpc::Status::OK;
    }
    tokens(output, "login");
    if (input->username() == "expired_tokens") {
      output->mutable_tokens()->mutable_access_expires_at()->set_seconds(kChecked - 2);
      output->mutable_tokens()->mutable_refresh_expires_at()->set_seconds(kChecked - 1);
    }
    if (input->username() == "expired_access_replay") {
      // Original pair/expiries are unchanged; only the authoritative replay check advances.
      output->mutable_checked_at()->set_seconds(kChecked + 901);
    }
    if (input->username() == "reverse_expiry") {
      output->mutable_tokens()->mutable_refresh_expires_at()->set_seconds(kChecked + 1);
    }
    if (input->username() == "bad_token_time") {
      output->mutable_tokens()->mutable_access_expires_at()->set_seconds(9223372036854775807LL);
    }
    if (input->username() == "bad_token_id") {
      output->mutable_tokens()->set_session_id("bad");
    }
    if (input->username() == "zero_rotation") {
      output->mutable_tokens()->set_rotation(0);
    }
    return grpc::Status::OK;
  }
  grpc::Status RefreshSession(grpc::ServerContext*,
                              const wire::IdentityRefreshSessionRequest* input,
                              wire::IdentityRefreshSessionResponse* output) override {
    if (input->context().idempotency_key() != kKey ||
        input->refresh_token() != "secret-refresh-credential-login") {
      return error(wire::UNAUTHENTICATED, grpc::StatusCode::UNAUTHENTICATED, input->context());
    }
    tokens(output, "refresh");
    return grpc::Status::OK;
  }
  grpc::Status ValidateSession(grpc::ServerContext*,
                               const wire::IdentityValidateSessionRequest* input,
                               wire::IdentityValidateSessionResponse* output) override {
    const std::lock_guard lock(mutex_);
    if (input->access_token() == "dependency-down") {
      return {grpc::StatusCode::UNAVAILABLE, ""};
    }
    if (revoked_ || (input->access_token() != "secret-access-credential-login" &&
                     !input->access_token().starts_with("fixture-"))) {
      return error(wire::UNAUTHENTICATED, grpc::StatusCode::UNAUTHENTICATED, input->context());
    }
    if (!input->context().delegation().empty()) {
      return error(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    auto* principal = output->mutable_principal();
    principal->set_principal_id("33333333-3333-4333-8333-333333333333");
    principal->set_principal_kind("human");
    principal->set_version(1);
    principal->set_state(wire::LIFECYCLE_ACTIVE);
    principal->mutable_binding()->set_kind(wire::RESOURCE_PRINCIPAL);
    principal->mutable_binding()->set_id(principal->principal_id());
    principal->add_roles(wire::ROLE_VIEWER);
    principal->add_display_permissions(wire::PERMISSION_IDENTITY_SESSION_READ);
    session(output->mutable_session(), kSession);
    auto* assignment = output->add_assignments();
    assignment->set_assignment_id("66666666-6666-4666-8666-666666666666");
    assignment->set_principal_id("33333333-3333-4333-8333-333333333333");
    assignment->set_role(wire::ROLE_VIEWER);
    assignment->set_version(18446744073709551615ULL);
    auto* scope = assignment->mutable_scope()->add_resources();
    scope->set_kind(wire::RESOURCE_SESSION);
    scope->set_id(kSession);
    output->set_authorization_version(18446744073709551615ULL);
    output->mutable_checked_at()->set_seconds(kChecked);
    const auto mode = input->access_token();
    if (mode == "fixture-disabled") {
      principal->set_state(wire::LIFECYCLE_DISABLED);
    }
    if (mode == "fixture-revoked") {
      output->mutable_session()->set_revoked(true);
    }
    if (mode == "fixture-expired") {
      output->mutable_session()->mutable_expires_at()->set_seconds(kChecked);
    }
    if (mode == "fixture-bad-time") {
      output->mutable_checked_at()->set_nanos(1000000000);
    }
    if (mode == "fixture-future-created") {
      output->mutable_session()->mutable_created_at()->set_seconds(kChecked + 1);
    }
    if (mode == "fixture-bad-id") {
      principal->mutable_binding()->set_id("bad");
    }
    if (mode == "fixture-bad-scope") {
      scope->set_id("bad");
    }
    if (mode == "fixture-bad-assignment") {
      assignment->set_assignment_id("bad");
    }
    if (mode == "fixture-zero-version") {
      principal->set_version(0);
    }
    return grpc::Status::OK;
  }
  grpc::Status IssueDelegation(grpc::ServerContext* context,
                               const wire::IdentityIssueDelegationRequest* input,
                               wire::IdentityIssueDelegationResponse* output) override {
    const auto certs = context->auth_context()->FindPropertyValues("x509_pem_cert");
    if (certs.size() != 1) {
      return error(wire::UNAUTHENTICATED, grpc::StatusCode::UNAUTHENTICATED, input->context());
    }
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(certs[0].data(), static_cast<int>(certs[0].size())), BIO_free);
    const std::unique_ptr<X509, decltype(&X509_free)> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (!cert || X509_digest(cert.get(), EVP_sha256(), digest, &length) != 1 || length != 32 ||
        input->client_certificate_sha256() !=
            std::string(reinterpret_cast<const char*>(digest), length) ||
        input->subject_proof() != "secret-access-credential-login" ||
        (input->audience() != "identity" && input->audience() != "runtime" &&
         input->audience() != "model") ||
        input->scope().resources_size() != 1 || input->permissions_size() != 1) {
      return error(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED, input->context());
    }
    const auto& resource = input->scope().resources(0);
    const bool identity_delegation =
        input->audience() == "identity" &&
        ((resource.kind() == wire::RESOURCE_PRINCIPAL &&
          resource.id() == "33333333-3333-4333-8333-333333333333" &&
          input->permissions(0) == wire::PERMISSION_IDENTITY_SESSION_READ) ||
         (resource.kind() == wire::RESOURCE_SESSION &&
          (resource.id() == kSession || resource.id() == kOther) &&
          input->permissions(0) == wire::PERMISSION_IDENTITY_SESSION_REVOKE));
    const bool model_delegation = input->audience() == "model" &&
                                  resource.kind() == wire::RESOURCE_MODEL &&
                                  !resource.id().empty() &&
                                  (input->permissions(0) == wire::PERMISSION_MODEL_MANAGE ||
                                   input->permissions(0) == wire::PERMISSION_MODEL_READ ||
                                   input->permissions(0) == wire::PERMISSION_MODEL_CALL_INSPECT ||
                                   input->permissions(0) == wire::PERMISSION_RESOURCE_READ);
    const bool audit_log_delegation =
        input->audience() == "runtime" && resource.kind() == wire::RESOURCE_CATALOG &&
        resource.id() == "control" &&
        (input->permissions(0) == wire::PERMISSION_LOG_READ ||
         input->permissions(0) == wire::PERMISSION_AUDIT_READ);
    if (!identity_delegation && !model_delegation && !audit_log_delegation) {
      return error(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED, input->context());
    }
    output->mutable_delegation()->set_proof(
        std::string{model_delegation ? "fixture-model-proof:"
                    : audit_log_delegation ? "fixture-runtime-proof:"
                                           : "fixture-proof:"} +
        resource.id());
    output->mutable_delegation()->set_audience("identity");
    output->mutable_delegation()->mutable_expires_at()->set_seconds(kChecked + 30);
    return grpc::Status::OK;
  }
  grpc::Status ListAudit(grpc::ServerContext*,
                         const wire::IdentityListAuditRequest* input,
                         wire::IdentityListAuditResponse* output) override {
    if (!input->has_filter() || input->filter().scope().resources_size() != 1) {
      return error(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    auto rows = filter_rows(input->filter());
    const auto size = std::min<std::size_t>(
        rows.size(), input->filter().page().page_size() == 0
                         ? 20
                         : static_cast<std::size_t>(input->filter().page().page_size()));
    for (std::size_t index = 0; index < size; ++index) {
      fill_audit_row(rows[index], output->add_entries());
    }
    if (rows.size() > size) {
      output->mutable_page()->set_next_page_token(std::to_string(rows[size - 1].nanos) + ":" +
                                                  rows[size - 1].id);
    }
    output->mutable_page()->set_page_size(static_cast<std::uint32_t>(size));
    output->mutable_checked_at()->set_seconds(kChecked);
    return grpc::Status::OK;
  }
  grpc::Status ListLogs(grpc::ServerContext*, const wire::IdentityListLogsRequest* input,
                        wire::IdentityListLogsResponse* output) override {
    if (!input->has_filter() || input->filter().scope().resources_size() != 1) {
      return error(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    auto rows = filter_rows(input->filter());
    const auto size = std::min<std::size_t>(
        rows.size(), input->filter().page().page_size() == 0
                         ? 20
                         : static_cast<std::size_t>(input->filter().page().page_size()));
    for (std::size_t index = 0; index < size; ++index) {
      fill_log_row(rows[index], output->add_entries());
    }
    if (rows.size() > size) {
      output->mutable_page()->set_next_page_token(std::to_string(rows[size - 1].nanos) + ":" +
                                                  rows[size - 1].id);
    }
    output->mutable_page()->set_page_size(static_cast<std::uint32_t>(size));
    output->mutable_checked_at()->set_seconds(kChecked);
    return grpc::Status::OK;
  }
  grpc::Status ListSessions(grpc::ServerContext*, const wire::IdentityListSessionsRequest* input,
                            wire::IdentityListSessionsResponse* output) override {
    if (input->context().delegation() != "fixture-proof:33333333-3333-4333-8333-333333333333" ||
        input->principal_id() != "33333333-3333-4333-8333-333333333333") {
      return error(wire::PERMISSION_DENIED, grpc::StatusCode::PERMISSION_DENIED, input->context());
    }
    if (input->page().page_token() == "bad") {
      return error(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    session(output->add_sessions(), kSession);
    session(output->add_sessions(), kOther);
    session(output->add_sessions(), kThird, true);
    output->mutable_checked_at()->set_seconds(kChecked);
    output->mutable_page()->set_page_size(input->page().page_size());
    output->mutable_page()->set_snapshot_id("55555555-5555-4555-8555-555555555555");
    output->mutable_page()->mutable_expires_at()->set_seconds(kChecked + 600);
    const auto mode = input->page().page_token();
    if (mode == "missing_page_time") {
      output->mutable_page()->clear_expires_at();
    }
    if (mode == "bad_snapshot") {
      output->mutable_page()->set_snapshot_id("bad");
    }
    if (mode == "bad_page_size") {
      output->mutable_page()->set_page_size(0);
    }
    if (mode == "duplicate_session") {
      output->mutable_sessions(1)->set_session_id(kSession);
    }
    if (mode == "foreign_session") {
      output->mutable_sessions(0)->set_principal_id(kOther);
    }
    if (mode == "bad_session_id") {
      output->mutable_sessions(0)->set_session_id("bad");
    }
    if (mode == "stale_page") {
      output->mutable_page()->mutable_expires_at()->set_seconds(kChecked);
    }
    if (mode == "missing_checked") {
      output->clear_checked_at();
    }
    if (input->page().page_token().empty()) {
      output->mutable_page()->set_next_page_token("opaque-next-token");
    }
    return grpc::Status::OK;
  }
  grpc::Status RevokeSession(grpc::ServerContext*, const wire::IdentityRevokeSessionRequest* input,
                             wire::IdentityRevokeSessionResponse* output) override {
    const std::lock_guard lock(mutex_);
    if (input->session_id() == kOther) {
      return error(wire::NOT_FOUND, grpc::StatusCode::NOT_FOUND, input->context());
    }
    if (input->context().delegation() != "fixture-proof:" + input->session_id() ||
        input->context().idempotency_key() != kKey) {
      return error(wire::INVALID_ARGUMENT, grpc::StatusCode::INVALID_ARGUMENT, input->context());
    }
    revoked_ = true;
    session(output->mutable_session(), kSession, true);
    output->mutable_checked_at()->set_seconds(kChecked);
    return grpc::Status::OK;
  }

private:
  std::mutex mutex_;
  bool revoked_ = false;
};
} // namespace
int main(int argc, char* argv[]) {
  if (argc != 4) {
    return 2;
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_root_certs = read(argv[1]);
  options.pem_key_cert_pairs.push_back({read(argv[3]), read(argv[2])});
  options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options), &port);
  Fixture fixture;
  builder.RegisterService(&fixture);
  auto server = builder.BuildAndStart();
  if (!server) {
    return 1;
  }
  std::cout << port << '\n' << std::flush;
  server->Wait();
}
