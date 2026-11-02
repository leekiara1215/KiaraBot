#include "kiarabot/core/runtime/platform/model_identity_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace codec = target_codec;
namespace t = kiarabot::core::target;
using Json = nlohmann::json;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose,
                   t::Acceptance acceptance = t::Acceptance::NotAccepted) {
  return {code, acceptance, recovery, {}, {}};
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
t::Failure forbidden() {
  return failure(t::ErrorCode::PermissionDenied, t::Recovery::Authenticate);
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
std::optional<std::string> base64url_decode(std::string_view value) {
  if (value.empty() || value.size() > 24576) {
    return std::nullopt;
  }
  static constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  if (value.size() % 4 == 1) {
    return std::nullopt;
  }
  std::string output;
  output.reserve(value.size() * 3 / 4);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char character : value) {
    const auto found = kAlphabet.find(character);
    if (found == std::string_view::npos) {
      return std::nullopt;
    }
    buffer = (buffer << 6U) | static_cast<std::uint32_t>(found);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xffU));
    }
  }
  return output;
}
std::optional<t::Sha256Digest> digest_from_hex(std::string_view value) {
  if (value.size() != 64) {
    return std::nullopt;
  }
  t::Sha256Digest digest;
  for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
    const auto character = [](char value) -> int {
      if (value >= '0' && value <= '9') {
        return value - '0';
      }
      if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
      }
      return -1;
    };
    const int high = character(value[index * 2]);
    const int low = character(value[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    digest.bytes[index] = static_cast<std::byte>((high << 4) | low);
  }
  return digest;
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
t::Failure grpc_failure(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::UNAUTHENTICATED:
    return unauthenticated();
  case grpc::StatusCode::PERMISSION_DENIED:
    return forbidden();
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
bool management_method(std::string_view method) {
  // Control REST management surface. Runtime (plugin model_chat) only calls Chat; all metadata,
  // call-status, operation and configuration reads are Control-origin delegations.
  return method == "/kiarabot.v1.ModelService/GetModelMetadata" ||
         method == "/kiarabot.v1.ModelService/ListModelMetadata" ||
         method == "/kiarabot.v1.ModelService/UpdateModel" ||
         method == "/kiarabot.v1.ModelService/RotateProviderSecret" ||
         method == "/kiarabot.v1.ModelService/GetCallStatus" ||
         method == "/kiarabot.v1.ModelService/GetOperation" ||
         method == "/kiarabot.v1.ModelService/DisableModel" ||
         method == "/kiarabot.v1.ModelService/EnableModel" ||
         method == "/kiarabot.v1.ModelService/ResolveModelCall" ||
         method == "/kiarabot.v1.ModelService/GetModelConfiguration";
}
} // namespace

target::Outcome<target::ModelCaller>
decode_model_delegation(std::string_view proof, const target::Sha256Digest& leaf_fingerprint,
                        target::TimePoint at) {
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
    if (document.is_discarded() || !document.is_object() || !document.contains("delegation") ||
        !document["delegation"].is_object()) {
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
    if (!subject || !canonical_uuid(*subject) || !token_id || !canonical_uuid(*token_id) ||
        !issuer || !audience || *audience != "model") {
      return unauthenticated();
    }
    const auto number = [&](const Json& value) -> std::optional<std::int64_t> {
      if (value.is_number_integer()) {
        return value.get<std::int64_t>();
      }
      if (value.is_string()) {
        try {
          std::size_t consumed = 0;
          const auto parsed = std::stoll(value.get<std::string>(), &consumed);
          if (consumed != value.get<std::string>().size()) {
            return std::nullopt;
          }
          return parsed;
        } catch (...) {
          return std::nullopt;
        }
      }
      return std::nullopt;
    };
    const auto issued = document.contains("iat") ? number(document["iat"]) : std::nullopt;
    const auto not_before = document.contains("nbf") ? number(document["nbf"]) : std::nullopt;
    const auto expires = document.contains("exp") ? number(document["exp"]) : std::nullopt;
    if (!issued || !not_before || !expires || *expires <= *issued || *expires - *issued > 60) {
      return unauthenticated();
    }
    const auto at_seconds = std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch());
    const auto skew = std::int64_t{2};
    if (at_seconds.count() + skew < *not_before || at_seconds.count() - skew >= *expires) {
      return unauthenticated();
    }
    const auto& delegation = document["delegation"];
    if (!delegation.contains("client_certificate") ||
        !delegation["client_certificate"].is_string()) {
      return unauthenticated();
    }
    const auto certificate = digest_from_hex(delegation["client_certificate"].get<std::string>());
    if (!certificate || *certificate != leaf_fingerprint) {
      return unauthenticated();
    }
    const auto actor = delegation.contains("direct_actor") && delegation["direct_actor"].is_string()
                           ? delegation["direct_actor"].get<std::string>()
                           : std::string{};
    if (!canonical_uuid(actor) || !delegation.contains("actor_chain") ||
        !delegation["actor_chain"].is_array() || delegation["actor_chain"].empty() ||
        delegation["actor_chain"].size() > 2) {
      return unauthenticated();
    }
    std::vector<std::string> chain;
    for (const auto& value : delegation["actor_chain"]) {
      if (!value.is_string() || !canonical_uuid(value.get<std::string>())) {
        return unauthenticated();
      }
      chain.push_back(value.get<std::string>());
    }
    const auto hops =
        delegation.contains("business_hops") ? number(delegation["business_hops"]) : std::nullopt;
    if (!hops || *hops != static_cast<std::int64_t>(chain.size()) || *hops < 1 || *hops > 2 ||
        chain.back() != actor) {
      return unauthenticated();
    }
    if (!delegation.contains("authorization_version") ||
        !delegation["authorization_version"].is_string()) {
      return unauthenticated();
    }
    std::size_t consumed = 0;
    std::uint64_t authorization_version = 0;
    try {
      authorization_version =
          std::stoull(delegation["authorization_version"].get<std::string>(), &consumed);
    } catch (...) {
      return unauthenticated();
    }
    if (consumed != delegation["authorization_version"].get<std::string>().size() ||
        authorization_version == 0) {
      return unauthenticated();
    }
    t::ModelCaller caller;
    caller.subject = t::SubjectId{*subject};
    caller.direct_actor = t::SubjectId{actor};
    caller.runtime_origin = *hops == 2;
    caller.owner = caller.runtime_origin ? std::optional<t::InstanceId>{t::InstanceId{*subject}}
                                         : std::nullopt;
    caller.delegation = std::string{proof};
    caller.peer_fingerprint = leaf_fingerprint;
    return caller;
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

ModelIdentityClient::ModelIdentityClient(ModelIdentityClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(v1::IdentityService::NewStub(channel_)) {}

ModelIdentityClient::~ModelIdentityClient() = default;

target::Outcome<std::unique_ptr<ModelIdentityClient>>
ModelIdentityClient::create(ModelIdentityClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || options.budget.count() <= 0 ||
      options.maximum_budget.count() <= 0 || options.budget > options.maximum_budget) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<ModelIdentityClient>(new ModelIdentityClient(std::move(options)));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

bool ModelIdentityClient::available() const noexcept {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

target::Outcome<t::ModelCaller>
ModelIdentityClient::authenticate(grpc::ServerContext& transport,
                                  const v1::RequestContext& request) {
  try {
    if (request.delegation().empty()) {
      return unauthenticated();
    }
    const auto auth = transport.auth_context();
    if (!auth) {
      return unauthenticated();
    }
    auto evidence = extract_peer_certificate(*auth);
    if (const auto* rejected = std::get_if<t::Failure>(&evidence)) {
      return *rejected;
    }
    auto caller = decode_model_delegation(
        request.delegation(), std::get<PeerCertificateEvidence>(evidence).leaf_fingerprint, now());
    if (const auto* rejected = std::get_if<t::Failure>(&caller)) {
      return *rejected;
    }
    auto value = std::get<t::ModelCaller>(caller);
    value.delegation = request.delegation();
    return value;
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

target::Outcome<t::ModelAuthorizationDecision>
ModelIdentityClient::authorize(const t::ModelCaller& caller, t::Permission permission,
                               const t::ResourceRef& resource, std::chrono::milliseconds budget) {
  try {
    if (!caller.subject || !caller.direct_actor || caller.delegation.empty() ||
        caller.method.empty()) {
      return unauthenticated();
    }
    const auto is_management = management_method(caller.method);
    if (is_management && caller.runtime_origin) {
      return forbidden();
    }
    if (!is_management && !caller.runtime_origin) {
      return forbidden();
    }
    if (!stub_) {
      return unavailable();
    }
    const auto started = now();
    auto deadline = started + std::min({budget, options_.budget, options_.maximum_budget});
    if (deadline <= started) {
      return failure(t::ErrorCode::Timeout, t::Recovery::Backoff);
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
    input.set_subject_proof(caller.delegation);
    input.set_full_method(caller.method);
    const auto scope = codec::to_wire(resource);
    const auto* wire = std::get_if<v1::ResourceRef>(&scope);
    if (!wire) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    *input.mutable_scope()->add_resources() = *wire;
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
                        t::Acceptance::NotAccepted,
                        t::Recovery::ReadCurrent,
                        {},
                        {}};
    }
    if (decision.has_denial()) {
      return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                     t::Acceptance::Unknown);
    }
    return t::ModelAuthorizationDecision{t::ResourceVersion{decision.authorization_version()}};
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
