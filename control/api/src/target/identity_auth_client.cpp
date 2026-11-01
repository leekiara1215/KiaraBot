#include "kiarabot/control/target/identity_auth_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

#include <google/protobuf/util/time_util.h>
#include <grpc/support/time.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include "kiarabot/core/domain/target/time.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace kiarabot::control::target {
namespace {
namespace wire = kiarabot::v1;
using Json = nlohmann::json;
using Timestamp = google::protobuf::Timestamp;
using TimeUtil = google::protobuf::util::TimeUtil;
struct InvalidInput {};
struct InvalidUpstream {};
struct RpcError {
  wire::ErrorCode code;
  std::optional<wire::FailureContext> context = std::nullopt;
  bool retryable = false;
};

bool safe_text(std::string_view text, std::size_t minimum, std::size_t maximum) {
  return text.size() >= minimum && text.size() <= maximum &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c < 127; });
}
std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  });
  return value;
}
bool json_content_type(std::string value) {
  value = lower_ascii(std::move(value));
  const auto trim = [](std::string text) {
    const auto begin = text.find_first_not_of(" \t");
    return begin == std::string::npos
               ? std::string{}
               : text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
  };
  const auto separator = value.find(';');
  if (trim(value.substr(0, separator)) != "application/json") {
    return false;
  }
  if (separator == std::string::npos) {
    return true;
  }
  const auto parameter = trim(value.substr(separator + 1));
  const auto equal = parameter.find('=');
  if (equal == std::string::npos || trim(parameter.substr(0, equal)) != "charset") {
    return false;
  }
  const auto encoding = trim(parameter.substr(equal + 1));
  return encoding == "utf-8" || encoding == "\"utf-8\"";
}
bool uuid(std::string_view value) {
  if (value.size() != 36 || value[14] != '4' ||
      (value[19] != '8' && value[19] != '9' && value[19] != 'a' && value[19] != 'b')) {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') {
        return false;
      }
    } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}
std::string request_id() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw InvalidUpstream{};
  }
  bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
  bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      result += '-';
    }
    result += kHex[bytes[i] >> 4U];
    result += kHex[bytes[i] & 0x0fU];
  }
  return result;
}
unsigned int http_status(wire::ErrorCode code) {
  switch (code) {
  case wire::INVALID_ARGUMENT:
    return 400;
  case wire::UNAUTHENTICATED:
    return 401;
  case wire::PERMISSION_DENIED:
    return 403;
  case wire::NOT_FOUND:
    return 404;
  case wire::CONFLICT:
    return 409;
  case wire::RATE_LIMITED:
    return 429;
  case wire::TIMEOUT:
    return 504;
  case wire::UPSTREAM_UNAVAILABLE:
  case wire::PLUGIN_UNAVAILABLE:
  case wire::CAPABILITY_NOT_SUPPORTED:
    return 503;
  default:
    return 500;
  }
}
grpc::StatusCode grpc_status(wire::ErrorCode code) {
  switch (code) {
  case wire::INVALID_ARGUMENT:
    return grpc::StatusCode::INVALID_ARGUMENT;
  case wire::UNAUTHENTICATED:
    return grpc::StatusCode::UNAUTHENTICATED;
  case wire::PERMISSION_DENIED:
    return grpc::StatusCode::PERMISSION_DENIED;
  case wire::NOT_FOUND:
    return grpc::StatusCode::NOT_FOUND;
  case wire::CONFLICT:
    return grpc::StatusCode::ABORTED;
  case wire::CAPABILITY_NOT_SUPPORTED:
    return grpc::StatusCode::UNIMPLEMENTED;
  case wire::RATE_LIMITED:
    return grpc::StatusCode::RESOURCE_EXHAUSTED;
  case wire::TIMEOUT:
    return grpc::StatusCode::DEADLINE_EXCEEDED;
  case wire::UPSTREAM_UNAVAILABLE:
  case wire::PLUGIN_UNAVAILABLE:
    return grpc::StatusCode::UNAVAILABLE;
  case wire::INTERNAL:
    return grpc::StatusCode::INTERNAL;
  default:
    return grpc::StatusCode::UNKNOWN;
  }
}
Json failure_context_json(const wire::FailureContext& context) {
  if (!wire::AcceptanceState_IsValid(context.acceptance()) ||
      context.acceptance() == wire::ACCEPTANCE_UNSPECIFIED ||
      !wire::RecoveryAction_IsValid(context.recovery()) ||
      context.recovery() == wire::RECOVERY_UNSPECIFIED ||
      (context.has_current_version() && context.current_version() == 0)) {
    throw InvalidUpstream{};
  }
  Json result{{"acceptance", wire::AcceptanceState_Name(context.acceptance())},
              {"recovery", wire::RecoveryAction_Name(context.recovery())}};
  if (context.has_operation()) {
    const auto& operation = context.operation();
    const std::set<std::string> services{"identity",  "runtime", "plugin-host",
                                         "scheduler", "model",   "control"};
    if (!services.contains(operation.owner_service()) || !uuid(operation.operation_id()) ||
        !operation.has_resource() || !uuid(operation.resource().id()) ||
        !wire::ResourceKind_IsValid(operation.resource().kind()) ||
        operation.resource().kind() == wire::RESOURCE_KIND_UNSPECIFIED) {
      throw InvalidUpstream{};
    }
    result["operation"] = {{"owner_service", operation.owner_service()},
                           {"operation_id", operation.operation_id()},
                           {"resource",
                            {{"kind", wire::ResourceKind_Name(operation.resource().kind())},
                             {"id", operation.resource().id()}}}};
  }
  if (context.retry_after_ms() > 0) {
    result["retry_after_ms"] = context.retry_after_ms();
  }
  if (context.has_current_version()) {
    result["current_version"] = std::to_string(context.current_version());
  }
  return result;
}
AuthHttpResponse failure(wire::ErrorCode code, const std::string& id,
                         const std::optional<wire::FailureContext>& context = std::nullopt,
                         bool retryable = false) {
  if (!wire::ErrorCode_IsValid(code) || code == wire::ERROR_CODE_UNSPECIFIED) {
    code = wire::INTERNAL;
  }
  Json body{{"code", wire::ErrorCode_Name(code)},
            {"message", wire::ErrorCode_Name(code)},
            {"request_id", id},
            {"trace_id", id},
            {"retryable", retryable}};
  if (context) {
    body["failure_context"] = failure_context_json(*context);
  }
  AuthHttpResponse response{http_status(code), body.dump(), {}};
  if (code == wire::RATE_LIMITED) {
    const auto delay =
        context ? (static_cast<std::uint64_t>(context->retry_after_ms()) + 999) / 1000 : 0;
    response.headers.emplace_back("Retry-After", std::to_string(std::max<std::uint64_t>(1, delay)));
  }
  return response;
}
void checked(const grpc::Status& status, const wire::RequestContext& context) {
  if (status.ok()) {
    return;
  }
  if (!status.error_details().empty()) {
    wire::RpcFailure detail;
    if (!detail.ParseFromString(status.error_details()) || !detail.has_error() ||
        !detail.has_context() || !wire::ErrorCode_IsValid(detail.error().code()) ||
        detail.error().code() == wire::ERROR_CODE_UNSPECIFIED ||
        grpc_status(detail.error().code()) != status.error_code() ||
        detail.error().request_id() != context.request_id() ||
        detail.error().trace_id() != context.trace_id()) {
      throw RpcError{wire::INTERNAL};
    }
    (void)failure_context_json(detail.context());
    // Stable facts survive transport categories. Free-text messages/details never cross to HTTP.
    throw RpcError{detail.error().code(), detail.context(), detail.error().retryable()};
  }
  // No received business facts: preserve ambiguity, never invent NotAccepted or rollback.
  if (status.error_code() == grpc::StatusCode::UNAVAILABLE ||
      status.error_code() == grpc::StatusCode::CANCELLED) {
    throw RpcError{wire::UPSTREAM_UNAVAILABLE};
  }
  if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
    throw RpcError{wire::TIMEOUT};
  }
  throw RpcError{wire::INTERNAL};
}
std::string time_text(const Timestamp& time) {
  if (!TimeUtil::IsTimestampValid(time) ||
      std::holds_alternative<core::target::Failure>(
          core::target::time_point_from_parts({time.seconds(), time.nanos()}))) {
    throw InvalidUpstream{};
  }
  return TimeUtil::ToString(time);
}
int compare_time(const Timestamp& a, const Timestamp& b) {
  if (a.seconds() != b.seconds()) {
    return a.seconds() < b.seconds() ? -1 : 1;
  }
  return (a.nanos() > b.nanos()) - (a.nanos() < b.nanos());
}
Json tokens_json(const wire::TokenPair& tokens, const Timestamp& checked_at) {
  if (!uuid(tokens.session_id()) || tokens.rotation() == 0 ||
      !safe_text(tokens.access_token(), 1, 4096) || !safe_text(tokens.refresh_token(), 1, 4096) ||
      !tokens.has_access_expires_at() || !tokens.has_refresh_expires_at()) {
    throw InvalidUpstream{};
  }
  const auto checked_text = time_text(checked_at);
  const auto access_text = time_text(tokens.access_expires_at());
  const auto refresh_text = time_text(tokens.refresh_expires_at());
  if (compare_time(tokens.access_expires_at(), checked_at) <= 0 ||
      compare_time(tokens.refresh_expires_at(), tokens.access_expires_at()) < 0) {
    throw InvalidUpstream{};
  }
  auto ttl = tokens.access_expires_at().seconds() - checked_at.seconds();
  if (tokens.access_expires_at().nanos() < checked_at.nanos()) {
    --ttl;
  }
  if (ttl > 2147483647) {
    throw InvalidUpstream{};
  }
  return {{"access_token", tokens.access_token()},
          {"refresh_token", tokens.refresh_token()},
          {"expires_in", std::max<std::int64_t>(0, ttl)},
          {"checked_at", checked_text},
          {"access_expires_at", access_text},
          {"refresh_expires_at", refresh_text}};
}
Json session_json(const wire::SessionRecord& session, const Timestamp& checked_at) {
  if (!uuid(session.session_id()) || !uuid(session.principal_id()) || session.rotation() == 0 ||
      !session.has_created_at() || !session.has_expires_at()) {
    throw InvalidUpstream{};
  }
  (void)time_text(checked_at);
  (void)time_text(session.created_at());
  (void)time_text(session.expires_at());
  if (compare_time(session.created_at(), checked_at) > 0 ||
      compare_time(session.expires_at(), session.created_at()) <= 0) {
    throw InvalidUpstream{};
  }
  return {{"session_id", session.session_id()},
          {"user_id", session.principal_id()},
          {"created_at", time_text(session.created_at())},
          {"expires_at", time_text(session.expires_at())},
          {"state", session.revoked()                                     ? "REVOKED"
                    : compare_time(session.expires_at(), checked_at) <= 0 ? "EXPIRED"
                                                                          : "ACTIVE"}};
}
Json scope_json(const wire::ResourceScope& scope) {
  if (scope.resources().empty()) {
    throw InvalidUpstream{};
  }
  Json resources = Json::array();
  std::set<std::pair<int, std::string>> seen;
  for (const auto& resource : scope.resources()) {
    if (!uuid(resource.id()) || resource.kind() == wire::RESOURCE_KIND_UNSPECIFIED ||
        !wire::ResourceKind_IsValid(resource.kind()) ||
        !seen.emplace(resource.kind(), resource.id()).second) {
      throw InvalidUpstream{};
    }
    resources.push_back(
        {{"kind", wire::ResourceKind_Name(resource.kind())}, {"id", resource.id()}});
  }
  return {{"resources", resources}};
}
Json principal_json(const wire::IdentityValidateSessionResponse& response) {
  if (!response.has_principal() || !response.has_session() || !response.has_checked_at() ||
      response.authorization_version() == 0 || !uuid(response.principal().principal_id()) ||
      !uuid(response.session().session_id()) ||
      response.session().principal_id() != response.principal().principal_id()) {
    throw InvalidUpstream{};
  }
  const auto& principal = response.principal();
  (void)session_json(response.session(), response.checked_at());
  if (principal.state() != wire::LIFECYCLE_ACTIVE || principal.version() == 0 ||
      principal.principal_kind().empty() || !principal.has_binding() ||
      !uuid(principal.binding().id()) ||
      principal.binding().kind() == wire::RESOURCE_KIND_UNSPECIFIED ||
      !wire::ResourceKind_IsValid(principal.binding().kind()) || response.session().revoked() ||
      compare_time(response.session().expires_at(), response.checked_at()) <= 0) {
    throw InvalidUpstream{};
  }
  if (principal.has_scope()) {
    (void)scope_json(principal.scope());
  }
  std::set<int> seen_roles;
  std::set<int> seen_permissions;
  std::set<std::string> seen_assignments;
  Json roles = Json::array();
  for (const auto role : response.principal().roles()) {
    if (!seen_roles.insert(role).second) {
      throw InvalidUpstream{};
    }
    switch (role) {
    case wire::ROLE_ADMIN:
      roles.push_back("admin");
      break;
    case wire::ROLE_OPERATOR:
      roles.push_back("operator");
      break;
    case wire::ROLE_VIEWER:
      roles.push_back("viewer");
      break;
    default:
      throw InvalidUpstream{};
    }
  }
  Json permissions = Json::array();
  for (const auto permission : response.principal().display_permissions()) {
    if (!wire::Permission_IsValid(permission) || permission == wire::PERMISSION_UNSPECIFIED ||
        !seen_permissions.insert(permission).second) {
      throw InvalidUpstream{};
    }
    permissions.push_back(wire::Permission_Name(permission));
  }
  Json assignments = Json::array();
  for (const auto& assignment : response.assignments()) {
    if (!uuid(assignment.assignment_id()) || !uuid(assignment.principal_id()) ||
        !seen_assignments.insert(assignment.assignment_id()).second ||
        assignment.principal_id() != response.principal().principal_id() ||
        assignment.version() == 0 || !assignment.has_scope()) {
      throw InvalidUpstream{};
    }
    Json entry{{"assignment_id", assignment.assignment_id()},
               {"principal_id", assignment.principal_id()},
               {"scope", scope_json(assignment.scope())},
               {"version", std::to_string(assignment.version())}};
    if (assignment.role() != wire::FIXED_ROLE_UNSPECIFIED) {
      if (!wire::FixedRole_IsValid(assignment.role()) || !assignment.permissions().empty()) {
        throw InvalidUpstream{};
      }
      entry["role"] = wire::FixedRole_Name(assignment.role());
    } else {
      if (assignment.permissions().empty()) {
        throw InvalidUpstream{};
      }
      std::set<int> seen_assignment_permissions;
      entry["permissions"] = Json::array();
      for (const auto permission : assignment.permissions()) {
        if (!wire::Permission_IsValid(permission) || permission == wire::PERMISSION_UNSPECIFIED ||
            !seen_assignment_permissions.insert(permission).second) {
          throw InvalidUpstream{};
        }
        entry["permissions"].push_back(wire::Permission_Name(permission));
      }
    }
    assignments.push_back(std::move(entry));
  }
  return {{"user_id", response.principal().principal_id()},
          {"session_id", response.session().session_id()},
          {"roles", roles},
          {"permissions", permissions},
          {"assignments", assignments},
          {"authorization_version", std::to_string(response.authorization_version())},
          {"checked_at", time_text(response.checked_at())}};
}
Json strict_json(const std::string& text, const std::set<std::string>& keys) {
  std::vector<std::set<std::string>> stack;
  bool duplicate = false;
  Json document;
  try {
    document = Json::parse(text, [&](int depth, Json::parse_event_t event, Json& parsed) {
      if (event == Json::parse_event_t::array_start ||
          (event == Json::parse_event_t::object_start && depth != 0)) {
        throw InvalidInput{};
      }
      if (event == Json::parse_event_t::object_start) {
        stack.emplace_back();
      }
      if (event == Json::parse_event_t::key &&
          !stack.back().insert(parsed.get<std::string>()).second) {
        duplicate = true;
      }
      if (event == Json::parse_event_t::object_end) {
        stack.pop_back();
      }
      return true;
    });
  } catch (const Json::exception&) {
    throw InvalidInput{};
  }
  if (duplicate || !document.is_object() || document.size() != keys.size()) {
    throw InvalidInput{};
  }
  for (const auto& key : keys) {
    if (!document.contains(key) || !document[key].is_string()) {
      throw InvalidInput{};
    }
  }
  return document;
}
std::string decode_query(std::string_view value) {
  std::string result;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%') {
      if (i + 2 >= value.size()) {
        throw InvalidInput{};
      }
      unsigned int decoded = 0;
      const auto parsed = std::from_chars(value.data() + i + 1, value.data() + i + 3, decoded, 16);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + i + 3 || decoded < 33 ||
          decoded > 126) {
        throw InvalidInput{};
      }
      result += static_cast<char>(decoded);
      i += 2;
    } else {
      result += value[i];
    }
  }
  if (!safe_text(result, 1, 4096)) {
    throw InvalidInput{};
  }
  return result;
}
wire::SnapshotPage session_page(const AuthHttpRequest& request, std::size_t separator) {
  wire::SnapshotPage page;
  page.set_page_size(20);
  if (separator != std::string::npos) {
    const auto query = request.target.substr(separator + 1);
    std::set<std::string> seen;
    std::size_t begin = 0;
    while (begin < query.size()) {
      const auto end = query.find('&', begin);
      const auto part =
          query.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      const auto equal = part.find('=');
      if (equal == std::string::npos) {
        throw InvalidInput{};
      }
      const auto name = part.substr(0, equal);
      const auto value = decode_query(std::string_view(part).substr(equal + 1));
      if (!seen.insert(name).second) {
        throw InvalidInput{};
      }
      if (name == "page_size") {
        unsigned int size = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || size == 0 ||
            size > 100) {
          throw InvalidInput{};
        }
        page.set_page_size(size);
      } else if (name == "page_token") {
        page.set_page_token(value);
      } else {
        throw InvalidInput{};
      }
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1;
      if (begin == query.size()) {
        throw InvalidInput{};
      }
    }
    if (query.empty()) {
      throw InvalidInput{};
    }
  }
  return page;
}
} // namespace

class IdentityAuthClient::Impl {
public:
  Impl(const std::string& endpoint, const core::platform::TlsMaterial& tls) {
    grpc::SslCredentialsOptions options;
    options.pem_root_certs = tls.ca;
    options.pem_cert_chain = tls.certificate;
    options.pem_private_key = tls.private_key;
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(1024 * 1024);
    arguments.SetMaxSendMessageSize(1024 * 1024);
    stub_ = wire::IdentityService::NewStub(
        grpc::CreateCustomChannel(endpoint, grpc::SslCredentials(options), arguments));
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(tls.certificate.data(), static_cast<int>(tls.certificate.size())),
        BIO_free);
    if (!bio) {
      throw InvalidUpstream{};
    }
    const std::unique_ptr<X509, decltype(&X509_free)> cert(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (!cert || X509_digest(cert.get(), EVP_sha256(), digest.data(), &length) != 1 ||
        length != 32) {
      throw InvalidUpstream{};
    }
    fingerprint_.assign(reinterpret_cast<const char*>(digest.data()), length);
  }
  template <typename Request, typename Response, typename Method>
  void call(Method method, Request& request, Response& response,
            const wire::RequestContext& context, std::chrono::steady_clock::time_point deadline) {
    *request.mutable_context() = context;
    grpc::ClientContext rpc;
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      throw RpcError{wire::TIMEOUT};
    }
    rpc.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                                  gpr_time_from_nanos(remaining.count(), GPR_TIMESPAN)));
    struct Registration {
      Impl& owner;
      grpc::ClientContext& context;
      ~Registration() {
        const std::lock_guard lock(owner.mutex_);
        owner.active_.erase(&context);
      }
    } registration{*this, rpc};
    {
      const std::lock_guard lock(mutex_);
      if (cancelled_) {
        throw RpcError{wire::UPSTREAM_UNAVAILABLE};
      }
      active_.insert(&rpc);
    }
    checked((stub_.get()->*method)(&rpc, request, &response), context);
  }
  void cancel() noexcept {
    const std::lock_guard lock(mutex_);
    cancelled_ = true;
    for (auto* context : active_) {
      context->TryCancel();
    }
  }
  AuthHttpResponse handle(const AuthHttpRequest& request) {
    std::string id = "control-request";
    try {
      id = request_id();
      if (request.body.size() > 1024 * 1024 || request.target.size() > 8192 ||
          request.headers.size() > 64) {
        throw InvalidInput{};
      }
      std::map<std::string, std::string> headers;
      std::size_t header_bytes = 0;
      for (const auto& [key, value] : request.headers) {
        auto name = key;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        });
        header_bytes += name.size() + value.size() + 4;
        if (!headers.emplace(name, value).second || header_bytes > 8192) {
          throw InvalidInput{};
        }
      }
      wire::RequestContext context;
      context.set_request_id(id);
      context.set_trace_id(id);
      const auto upstream_deadline =
          std::min(request.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{2});
      const auto remaining = upstream_deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        throw RpcError{wire::TIMEOUT};
      }
      const auto now =
          std::chrono::system_clock::now() +
          std::min(remaining, std::chrono::steady_clock::duration{std::chrono::seconds{2}});
      const auto nanos =
          std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
      context.mutable_deadline()->set_seconds(nanos / 1000000000);
      context.mutable_deadline()->set_nanos(static_cast<int>(nanos % 1000000000));
      const auto separator = request.target.find('?');
      const auto path = request.target.substr(0, separator);
      const bool login = path == "/api/v1/auth/login";
      const bool refresh = path == "/api/v1/auth/refresh";
      const bool me = path == "/api/v1/auth/me";
      const bool list = path == "/api/v1/auth/sessions";
      constexpr std::string_view kRevokePrefix = "/api/v1/auth/sessions/";
      const bool revoke = path.starts_with(kRevokePrefix) && path.ends_with("/revoke");
      if (!(login || refresh || me || list || revoke)) {
        return failure(wire::NOT_FOUND, id);
      }
      if (request.method != ((login || refresh || revoke) ? "POST" : "GET")) {
        auto result = failure(wire::INVALID_ARGUMENT, id);
        result.status = 405;
        result.headers.emplace_back("Allow", (login || refresh || revoke) ? "POST" : "GET");
        return result;
      }
      if (!list && separator != std::string::npos) {
        throw InvalidInput{};
      }
      if (login || refresh || revoke) {
        const auto key = headers.find("idempotency-key");
        if (key == headers.end() || !safe_text(key->second, 16, 128)) {
          throw InvalidInput{};
        }
        context.set_idempotency_key(key->second);
      }
      Json output;
      if (login || refresh) {
        if (!json_content_type(headers["content-type"])) {
          throw InvalidInput{};
        }
        if (login) {
          const auto body = strict_json(request.body, {"username", "password"});
          const auto username = body["username"].get<std::string>();
          const auto password = body["password"].get<std::string>();
          if (!safe_text(username, 1, 128) || password.empty() || password.size() > 1024 ||
              password.find('\0') != std::string::npos ||
              !std::all_of(username.begin(), username.end(), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '.' || c == '-';
              })) {
            throw InvalidInput{};
          }
          wire::IdentityLoginRequest input;
          input.set_username(username);
          input.set_password(password);
          wire::IdentityLoginResponse response;
          call(&wire::IdentityService::Stub::Login, input, response, context, upstream_deadline);
          if (!response.has_tokens() || !response.has_checked_at()) {
            throw InvalidUpstream{};
          }
          output = tokens_json(response.tokens(), response.checked_at());
        } else {
          const auto body = strict_json(request.body, {"refresh_token"});
          const auto token = body["refresh_token"].get<std::string>();
          if (!safe_text(token, 20, 4096)) {
            throw InvalidInput{};
          }
          wire::IdentityRefreshSessionRequest input;
          input.set_refresh_token(token);
          wire::IdentityRefreshSessionResponse response;
          call(&wire::IdentityService::Stub::RefreshSession, input, response, context,
               upstream_deadline);
          if (!response.has_tokens() || !response.has_checked_at()) {
            throw InvalidUpstream{};
          }
          output = tokens_json(response.tokens(), response.checked_at());
        }
      } else {
        if (!request.body.empty()) {
          throw InvalidInput{};
        }
        const auto page = list ? session_page(request, separator) : wire::SnapshotPage{};
        if (revoke &&
            !uuid(path.substr(kRevokePrefix.size(), path.size() - kRevokePrefix.size() - 7))) {
          throw InvalidInput{};
        }
        const auto authorization = headers.find("authorization");
        if (authorization == headers.end() ||
            (authorization->second.size() < 7 ||
             lower_ascii(authorization->second.substr(0, 7)) != "bearer ") ||
            !safe_text(std::string_view(authorization->second).substr(7), 1, 4096)) {
          throw RpcError{wire::UNAUTHENTICATED};
        }
        const auto access = authorization->second.substr(7);
        wire::IdentityValidateSessionRequest validation;
        validation.set_access_token(access);
        wire::IdentityValidateSessionResponse current;
        call(&wire::IdentityService::Stub::ValidateSession, validation, current, context,
             upstream_deadline);
        // Validate the authoritative projection before issuing any subsequent operation.
        auto principal = principal_json(current);
        if (me) {
          output = std::move(principal);
        } else {
          wire::IdentityIssueDelegationRequest delegation;
          delegation.set_subject_proof(access);
          delegation.set_audience("identity");
          delegation.set_client_certificate_sha256(fingerprint_);
          auto* resource = delegation.mutable_scope()->add_resources();
          if (list) {
            resource->set_kind(wire::RESOURCE_PRINCIPAL);
            resource->set_id(current.principal().principal_id());
          } else {
            const auto session =
                path.substr(kRevokePrefix.size(), path.size() - kRevokePrefix.size() - 7);
            if (!uuid(session)) {
              throw InvalidInput{};
            }
            resource->set_kind(wire::RESOURCE_SESSION);
            resource->set_id(session);
          }
          delegation.add_permissions(list ? wire::PERMISSION_IDENTITY_SESSION_READ
                                          : wire::PERMISSION_IDENTITY_SESSION_REVOKE);
          wire::IdentityIssueDelegationResponse proof;
          auto delegation_context = context;
          // This key belongs to the intermediate operation only, never replaces the caller's key.
          delegation_context.set_idempotency_key(id);
          call(&wire::IdentityService::Stub::IssueDelegation, delegation, proof, delegation_context,
               upstream_deadline);
          if (!proof.has_delegation() || proof.delegation().proof().empty() ||
              proof.delegation().audience() != "identity") {
            throw InvalidUpstream{};
          }
          context.set_delegation(proof.delegation().proof());
          if (list) {
            wire::IdentityListSessionsRequest input;
            input.set_principal_id(current.principal().principal_id());
            *input.mutable_page() = page;
            wire::IdentityListSessionsResponse response;
            call(&wire::IdentityService::Stub::ListSessions, input, response, context,
                 upstream_deadline);
            if (!response.has_checked_at() || !response.has_page() ||
                !uuid(response.page().snapshot_id()) || !response.page().has_expires_at() ||
                response.page().page_size() != input.page().page_size() ||
                response.page().page_size() == 0 || response.page().page_size() > 100 ||
                static_cast<unsigned int>(response.sessions_size()) > response.page().page_size()) {
              throw InvalidUpstream{};
            }
            (void)time_text(response.checked_at());
            (void)time_text(response.page().expires_at());
            const auto page_ttl =
                response.page().expires_at().seconds() - response.checked_at().seconds();
            if (compare_time(response.page().expires_at(), response.checked_at()) <= 0 ||
                page_ttl > 600 ||
                (page_ttl == 600 &&
                 response.page().expires_at().nanos() > response.checked_at().nanos()) ||
                (!response.page().next_page_token().empty() &&
                 !safe_text(response.page().next_page_token(), 1, 4096))) {
              throw InvalidUpstream{};
            }
            Json items = Json::array();
            std::set<std::string> seen_sessions;
            const wire::SessionRecord* previous = nullptr;
            for (const auto& session : response.sessions()) {
              if (!seen_sessions.insert(session.session_id()).second ||
                  session.principal_id() != input.principal_id() ||
                  (previous && (compare_time(previous->created_at(), session.created_at()) > 0 ||
                                (compare_time(previous->created_at(), session.created_at()) == 0 &&
                                 previous->session_id() >= session.session_id())))) {
                throw InvalidUpstream{};
              }
              items.push_back(session_json(session, response.checked_at()));
              previous = &session;
            }
            Json page{{"page_size", response.page().page_size()}};
            if (!response.page().next_page_token().empty()) {
              page["next_page_token"] = response.page().next_page_token();
            }
            output = {
                {"items", items}, {"page", page}, {"checked_at", time_text(response.checked_at())}};
          } else {
            wire::IdentityRevokeSessionRequest input;
            input.set_session_id(resource->id());
            wire::IdentityRevokeSessionResponse response;
            call(&wire::IdentityService::Stub::RevokeSession, input, response, context,
                 upstream_deadline);
            if (!response.has_session() || !response.has_checked_at() ||
                !response.session().revoked() ||
                response.session().session_id() != input.session_id()) {
              throw InvalidUpstream{};
            }
            (void)session_json(response.session(), response.checked_at());
            return {204, {}, {}};
          }
        }
      }
      return {200, output.dump(), {}};
    } catch (const InvalidInput&) {
      return failure(wire::INVALID_ARGUMENT, id);
    } catch (const Json::exception&) {
      return failure(wire::INTERNAL, id);
    } catch (const RpcError& error) {
      return failure(error.code, id, error.context, error.retryable);
    } catch (...) {
      return failure(wire::INTERNAL, id);
    }
  }

private:
  std::unique_ptr<wire::IdentityService::Stub> stub_;
  std::string fingerprint_;
  std::mutex mutex_;
  std::set<grpc::ClientContext*> active_;
  bool cancelled_ = false;
};
IdentityAuthClient::IdentityAuthClient(const std::string& endpoint,
                                       const core::platform::TlsMaterial& tls)
    : impl_(std::make_unique<Impl>(endpoint, tls)) {}
IdentityAuthClient::~IdentityAuthClient() {
  impl_->cancel();
}
AuthHttpResponse IdentityAuthClient::handle(const AuthHttpRequest& request) {
  return impl_->handle(request);
}
void IdentityAuthClient::cancel() noexcept {
  impl_->cancel();
}
} // namespace kiarabot::control::target
