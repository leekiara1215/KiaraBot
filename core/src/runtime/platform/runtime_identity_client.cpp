#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace codec = target_codec;
using Json = nlohmann::json;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose,
                   t::Acceptance acceptance = t::Acceptance::NotAccepted) {
  return {code, acceptance, recovery, std::nullopt, std::nullopt};
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
t::Failure unavailable() {
  return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool canonical_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
      continue;
    }
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
std::string random_uuid() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return {};
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result += '-';
    }
    result += kDigits[bytes[index] >> 4U];
    result += kDigits[bytes[index] & 0x0fU];
  }
  return result;
}
std::optional<std::string> base64url_decode(std::string_view input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::array<std::int16_t, 256> table{};
  table.fill(-1);
  for (std::size_t index = 0; index < 64; ++index) {
    table[static_cast<unsigned char>(kAlphabet[index])] = static_cast<std::int16_t>(index);
  }
  std::string output;
  output.reserve(input.size() * 3 / 4 + 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : input) {
    const auto value = table[static_cast<unsigned char>(character)];
    if (value < 0) {
      return std::nullopt;
    }
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output += static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xffU);
    }
  }
  return output;
}
std::optional<t::Sha256Digest> digest_from_hex(std::string_view value) {
  if (value.size() != 64) {
    return std::nullopt;
  }
  std::array<signed char, 64> digits{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character >= '0' && character <= '9') {
      digits[index] = static_cast<signed char>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digits[index] = static_cast<signed char>(character - 'a' + 10);
    } else {
      return std::nullopt;
    }
  }
  t::Sha256Digest digest;
  for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
    digest.bytes[index] = static_cast<std::byte>((digits[index * 2] << 4) | digits[index * 2 + 1]);
  }
  return digest;
}
std::string permission_name(t::Permission permission) {
  for (const auto& [name, value] : t::kPermissionNames) {
    if (value == permission) {
      return std::string{name};
    }
  }
  return {};
}
std::optional<t::TimePoint> seconds_time(std::int64_t seconds) {
  if (seconds < 0) {
    return std::nullopt;
  }
  constexpr std::int64_t kMaximum = std::numeric_limits<std::int64_t>::max() / 1000000000;
  if (seconds > kMaximum) {
    return std::nullopt;
  }
  return t::TimePoint{std::chrono::seconds{seconds}};
}
t::Failure grpc_failure(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::UNAUTHENTICATED:
    return unauthenticated();
  case grpc::StatusCode::PERMISSION_DENIED:
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
  case grpc::StatusCode::INVALID_ARGUMENT:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
  case grpc::StatusCode::UNAVAILABLE:
    return unavailable();
  default:
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}
bool timestamp(const t::TimePoint& value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
} // namespace

target::Outcome<target::AuthenticationContext>
decode_delegation_claims(std::string_view proof, const target::Sha256Digest& leaf_fingerprint,
                         target::TimePoint now, target::Service expected_audience) {
  const std::string_view audience_text = expected_audience == target::Service::Runtime ? "runtime"
                                         : expected_audience == target::Service::Scheduler
                                             ? "scheduler"
                                         : expected_audience == target::Service::Model ? "model"
                                                                                       : "";
  try {
    if (proof.empty() || proof.size() > 32768) {
      return unauthenticated();
    }
    const auto first = proof.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : proof.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        proof.find('.', second + 1) != std::string_view::npos) {
      return unauthenticated();
    }
    auto payload = base64url_decode(proof.substr(first + 1, second - first - 1));
    if (!payload) {
      return unauthenticated();
    }
    const auto document = Json::parse(*payload, nullptr, false);
    if (document.is_discarded() || !document.is_object() || document.size() != 8 ||
        !document.contains("delegation") || !document["delegation"].is_object() ||
        document["delegation"].size() != 8) {
      return unauthenticated();
    }
    const auto text = [&](const char* name) -> std::optional<std::string> {
      if (!document.contains(name) || !document[name].is_string()) {
        return std::nullopt;
      }
      auto value = document[name].get<std::string>();
      if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos) {
        return std::nullopt;
      }
      return value;
    };
    const auto subject = text("sub");
    const auto audience = text("aud");
    const auto token_id = text("jti");
    const auto issuer = text("iss");
    if (audience_text.empty() || !subject || !canonical_uuid(*subject) || !token_id ||
        !canonical_uuid(*token_id) || !issuer || !audience || *audience != audience_text) {
      return unauthenticated();
    }
    const auto integer = [&](const Json& value) -> std::optional<std::int64_t> {
      if (!value.is_number_integer()) {
        return std::nullopt;
      }
      return value.get<std::int64_t>();
    };
    // Identity signs uint64 delegation claims as canonical decimal JSON strings (see the mature
    // identity_crypto signer), never as JSON numbers; reject anything the signer could not emit.
    const auto claim_integer = [&](const Json& value) -> std::optional<std::int64_t> {
      if (!value.is_string()) {
        return std::nullopt;
      }
      const auto input = value.get<std::string>();
      if (input.empty() || input.size() > 19 || (input.size() > 1 && input[0] == '0')) {
        return std::nullopt;
      }
      std::int64_t parsed = 0;
      const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed);
      if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || parsed < 0) {
        return std::nullopt;
      }
      return parsed;
    };
    if (!document.contains("iat") || !document.contains("nbf") || !document.contains("exp")) {
      return unauthenticated();
    }
    const auto issued = integer(document["iat"]);
    const auto not_before = integer(document["nbf"]);
    const auto expires = integer(document["exp"]);
    if (!issued || !not_before || !expires) {
      return unauthenticated();
    }
    const auto issued_at = seconds_time(*issued);
    const auto valid_from = seconds_time(*not_before);
    const auto valid_until = seconds_time(*expires);
    if (!issued_at || !valid_from || !valid_until) {
      return unauthenticated();
    }
    // Contract: total delegation validity is at most 60s; it must be current at use time.
    if (*valid_until <= *valid_from || *valid_until - *valid_from > std::chrono::seconds{60} ||
        now < *valid_from || now >= *valid_until || *issued_at > now + std::chrono::seconds{2}) {
      return unauthenticated();
    }
    const auto& delegation = document["delegation"];
    const auto actor = delegation.contains("direct_actor") && delegation["direct_actor"].is_string()
                           ? delegation["direct_actor"].get<std::string>()
                           : std::string{};
    if (!canonical_uuid(actor) || !delegation.contains("actor_chain") ||
        !delegation["actor_chain"].is_array() || delegation["actor_chain"].size() > 2) {
      return unauthenticated();
    }
    std::vector<t::SubjectId> actors;
    for (const auto& value : delegation["actor_chain"]) {
      if (!value.is_string() || !canonical_uuid(value.get<std::string>())) {
        return unauthenticated();
      }
      actors.push_back({value.get<std::string>()});
    }
    if (!delegation.contains("permissions") || !delegation["permissions"].is_array() ||
        delegation["permissions"].empty() || delegation["permissions"].size() > 128) {
      return unauthenticated();
    }
    std::vector<t::ResourceScope> scopes;
    for (const auto& item : delegation["permissions"]) {
      if (!item.is_array() || item.size() != 3 || !item[2].is_string()) {
        return unauthenticated();
      }
      const auto permission_claim = claim_integer(item[0]);
      const auto kind_claim = claim_integer(item[1]);
      if (!permission_claim || !kind_claim) {
        return unauthenticated();
      }
      const auto permission_value = *permission_claim;
      const auto kind_value = *kind_claim;
      // P14m: permissions include P07/P09/P12 wire values 41-50 (log.read=48, audit.read=49).
      // Bind the cast by the frozen enum maximum before known_permission, and accept the
      // catalog control scope whose id is a stable contract id, not a UUID.
      if (permission_value <= 0 || permission_value > static_cast<int>(t::Permission::HealthRead) ||
          kind_value < 0 || kind_value > static_cast<int>(t::ResourceKind::Catalog)) {
        return unauthenticated();
      }
      const auto permission = static_cast<t::Permission>(permission_value);
      if (!t::known_permission(permission)) {
        return unauthenticated();
      }
      const auto id = item[2].get<std::string>();
      const auto kind = static_cast<t::ResourceKind>(kind_value);
      if (!t::valid_policy_resource(t::ResourceRef{kind, {id}}) ||
          (kind != t::ResourceKind::Catalog && !canonical_uuid(id)) ||
          permission_name(permission).empty()) {
        return unauthenticated();
      }
      scopes.push_back({t::ResourceRef{kind, {id}}, permission_name(permission)});
    }
    if (!delegation.contains("client_certificate") ||
        !delegation["client_certificate"].is_string()) {
      return unauthenticated();
    }
    const auto certificate = digest_from_hex(delegation["client_certificate"].get<std::string>());
    if (!certificate || !(*certificate == leaf_fingerprint)) {
      return unauthenticated();
    }
    if (!delegation.contains("authorization_version")) {
      return unauthenticated();
    }
    const auto authorization_version = claim_integer(delegation["authorization_version"]);
    if (!authorization_version || *authorization_version <= 0) {
      return unauthenticated();
    }
    if (!delegation.contains("business_hops")) {
      return unauthenticated();
    }
    const auto hops = claim_integer(delegation["business_hops"]);
    if (!hops || *hops < 0 || *hops > 2) {
      return unauthenticated();
    }
    if (!delegation.contains("source_credential") || !delegation["source_credential"].is_array() ||
        delegation["source_credential"].size() != 2 ||
        !delegation["source_credential"][0].is_string()) {
      return unauthenticated();
    }
    const auto source_version = claim_integer(delegation["source_credential"][1]);
    if (!canonical_uuid(delegation["source_credential"][0].get<std::string>()) || !source_version ||
        *source_version <= 0) {
      return unauthenticated();
    }
    t::AuthenticationContext context;
    context.subject = {*subject};
    context.direct_peer = t::PlatformPeer{
        expected_audience == t::Service::Scheduler ? t::Service::Runtime : t::Service::Control,
        {actor}};
    context.audience = expected_audience;
    context.actors = std::move(actors);
    context.scopes = std::move(scopes);
    context.authorization_version = {static_cast<std::uint64_t>(*authorization_version)};
    context.verified_at = now;
    context.expires_at = *valid_until;
    context.proof = std::string{proof};
    return context;
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return unauthenticated();
  }
}

RuntimeIdentityClient::RuntimeIdentityClient(RuntimeIdentityClientOptions options)
    : options_(std::move(options)) {
  grpc::ChannelArguments arguments;
  arguments.SetMaxReceiveMessageSize(1 << 20);
  arguments.SetMaxSendMessageSize(1 << 20);
  channel_ = grpc::CreateCustomChannel(options_.endpoint, options_.channel_credentials, arguments);
  if (channel_) {
    stub_ = v1::IdentityService::NewStub(channel_);
  }
}

RuntimeIdentityClient::~RuntimeIdentityClient() = default;

target::Outcome<std::unique_ptr<RuntimeIdentityClient>>
RuntimeIdentityClient::create(RuntimeIdentityClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || options.budget.count() <= 0 ||
      options.maximum_budget.count() <= 0 || options.budget > options.maximum_budget ||
      options.delegation_budget.count() <= 0 || options.delegation_maximum_budget.count() <= 0 ||
      options.delegation_budget > options.delegation_maximum_budget ||
      options.delegation_maximum_budget > std::chrono::seconds{30}) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<RuntimeIdentityClient>(new RuntimeIdentityClient(std::move(options)));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

bool RuntimeIdentityClient::available() const noexcept {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

target::Outcome<target::RuntimeAuthorizationDecision>
RuntimeIdentityClient::check(const target::RuntimeAuthorizationRequest& request) {
  try {
    const auto started = now();
    auto deadline = started + std::min(options_.budget, options_.maximum_budget);
    if (request.deadline < deadline) {
      deadline = request.deadline;
    }
    if (deadline <= started) {
      return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
    }
    if (request.authentication.proof.empty()) {
      return unauthenticated();
    }
    if (!stub_) {
      return unavailable();
    }
    v1::IdentityCheckAuthorizationRequest input;
    auto* context = input.mutable_context();
    const auto request_id = random_uuid();
    const auto trace_id = random_uuid();
    if (request_id.empty() || trace_id.empty()) {
      return unavailable();
    }
    context->set_request_id(request_id);
    context->set_trace_id(trace_id);
    if (!timestamp(deadline, context->mutable_deadline())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    input.set_subject_proof(request.authentication.proof);
    input.set_full_method(request.method);
    for (const auto& scope : request.scope) {
      const auto encoded = codec::to_wire(scope);
      const auto* wire = std::get_if<v1::ResourceRef>(&encoded);
      if (!wire) {
        return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
      }
      *input.mutable_scope()->add_resources() = *wire;
    }
    if (request.authentication.authorization_version.value > 0) {
      input.set_known_authorization_version(request.authentication.authorization_version.value);
    }
    v1::IdentityCheckAuthorizationResponse output;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            deadline.time_since_epoch())});
    const auto status = stub_->CheckAuthorization(&call, input, &output);
    if (!status.ok()) {
      return grpc_failure(status);
    }
    if (!output.has_decision()) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    const auto& decision = output.decision();
    if (decision.authorization_version() == 0 || !decision.has_checked_at()) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    const auto checked = codec::from_wire(decision.checked_at());
    const auto* checked_at = std::get_if<t::TimePoint>(&checked);
    if (!checked_at || *checked_at > now() + std::chrono::seconds{2} ||
        now() - *checked_at > std::chrono::seconds{60}) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    if (!decision.allowed()) {
      const auto code = decision.has_denial()
                            ? codec::from_wire(decision.denial().code())
                            : t::Outcome<t::ErrorCode>{t::ErrorCode::PermissionDenied};
      const auto* denial = std::get_if<t::ErrorCode>(&code);
      return t::Failure{denial ? *denial : t::ErrorCode::PermissionDenied,
                        t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, std::nullopt,
                        std::nullopt};
    }
    if (decision.has_denial()) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    t::RuntimeAuthorizationDecision result;
    result.allowed = true;
    result.subject = request.authentication.subject;
    result.permission = request.permission;
    result.scope = request.scope;
    result.authorization_version = {decision.authorization_version()};
    result.checked_at = *checked_at;
    return result;
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}

target::Outcome<ExchangedDelegation>
RuntimeIdentityClient::exchange_delegation(std::string_view parent_proof, target::Service audience,
                                           const std::vector<target::ResourceRef>& scope,
                                           const std::vector<target::Permission>& permissions,
                                           const target::Sha256Digest& caller_certificate) {
  try {
    if (parent_proof.empty() || parent_proof.size() > 32768 || scope.empty() ||
        permissions.empty() || !stub_) {
      return unauthenticated();
    }
    for (const unsigned char character : parent_proof) {
      if (character < 33 || character > 126) {
        return unauthenticated();
      }
    }
    std::string_view audience_text;
    switch (audience) {
    case target::Service::Model:
      audience_text = "model";
      break;
    case target::Service::Scheduler:
      audience_text = "scheduler";
      break;
    default:
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    v1::IdentityExchangeDelegationRequest input;
    auto* context = input.mutable_context();
    const auto request_id = random_uuid();
    const auto trace_id = random_uuid();
    if (request_id.empty() || trace_id.empty()) {
      return unavailable();
    }
    context->set_request_id(request_id);
    context->set_trace_id(trace_id);
    // Identity ExchangeDelegation requires a non-empty idempotency key on the wire even though the
    // RPC option says requires_idempotency=false; omitting it yields UNAUTHENTICATED. This is a
    // per-attempt transport key, not a durable command receipt.
    context->set_idempotency_key(random_uuid());
    const auto started = now();
    auto deadline =
        started + std::min(options_.delegation_budget, options_.delegation_maximum_budget);
    if (!timestamp(deadline, context->mutable_deadline())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    input.set_parent_proof(std::string{parent_proof});
    input.set_audience(std::string{audience_text});
    for (const auto& resource : scope) {
      const auto encoded = codec::to_wire(resource);
      const auto* wire = std::get_if<v1::ResourceRef>(&encoded);
      if (!wire) {
        return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
      }
      *input.mutable_scope()->add_resources() = *wire;
    }
    for (const auto permission : permissions) {
      const auto encoded = codec::to_wire(permission);
      const auto* wire = std::get_if<v1::Permission>(&encoded);
      if (!wire) {
        return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
      }
      input.add_permissions(*wire);
    }
    input.set_client_certificate_sha256(
        std::string(reinterpret_cast<const char*>(caller_certificate.bytes.data()),
                    caller_certificate.bytes.size()));
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            deadline.time_since_epoch())});
    v1::IdentityExchangeDelegationResponse output;
    const auto status = stub_->ExchangeDelegation(&call, input, &output);
    if (!status.ok()) {
      return grpc_failure(status);
    }
    if (!output.has_delegation() || output.delegation().proof().empty() ||
        output.delegation().proof().size() > 32768 ||
        output.delegation().audience() != audience_text) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    const auto expires = codec::from_wire(output.delegation().expires_at());
    const auto* expires_at = std::get_if<t::TimePoint>(&expires);
    if (!expires_at || *expires_at <= now()) {
      return unauthenticated();
    }
    if (output.delegation().business_hops() != 2) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    ExchangedDelegation result;
    result.proof = output.delegation().proof();
    result.expires_at = *expires_at;
    return result;
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}

target::Outcome<target::AuthenticationContext>
RuntimeIdentityClient::authenticate(grpc::ServerContext& transport,
                                    const v1::RequestContext& request) {
  const auto evidence = extract_peer_certificate(*transport.auth_context());
  if (const auto* failure = std::get_if<t::Failure>(&evidence)) {
    return *failure;
  }
  const auto& certificate = std::get<PeerCertificateEvidence>(evidence);
  return decode_delegation_claims(request.delegation(), certificate.leaf_fingerprint, now());
}

} // namespace kiarabot::core::platform
