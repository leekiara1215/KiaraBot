#include "kiarabot/core/runtime/platform/host_identity_client.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <random>
#include <utility>

#include <kiarabot/v1/common.pb.h>

#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace v1 = kiarabot::v1;
namespace codec = target_codec;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure unavailable() {
  return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
std::string random_uuid() {
  static std::mutex mutex;
  static std::mt19937_64 generator{std::random_device{}()};
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  {
    std::lock_guard lock(mutex);
    high = generator();
    low = generator();
  }
  high = (high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
  low = (low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text(36, '-');
  const std::uint64_t parts[2] = {high, low};
  std::size_t output = 0;
  for (int index = 15; index >= 0; --index) {
    const auto byte = static_cast<unsigned char>((parts[index / 8] >> ((index % 8) * 8)) & 0xff);
    if (output == 8 || output == 13 || output == 18 || output == 23) {
      ++output;
    }
    text[output++] = kHex[byte >> 4];
    text[output++] = kHex[byte & 0x0f];
  }
  return text;
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
  case grpc::StatusCode::INVALID_ARGUMENT:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  case grpc::StatusCode::PERMISSION_DENIED:
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
  case grpc::StatusCode::UNAUTHENTICATED:
    return unauthenticated();
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
  case grpc::StatusCode::ABORTED:
  case grpc::StatusCode::ALREADY_EXISTS:
    return failure(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
  case grpc::StatusCode::UNAVAILABLE:
    return unavailable();
  default:
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                      t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
}
// Canonical owner method for the exact permission this application path checks. Identity method
// policy only accepts registered owner methods; both share the frozen HostManagement scope kind.
std::string_view method_for(t::Permission permission) {
  switch (permission) {
  case t::Permission::HostRead:
    return "kiarabot.v1.HostManagementService/GetMaintenance";
  case t::Permission::HostExecute:
    return "kiarabot.v1.HostManagementService/BeginMaintenance";
  default:
    return {};
  }
}
} // namespace

HostIdentityClient::HostIdentityClient(HostIdentityClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(channel_ ? v1::IdentityService::NewStub(channel_) : nullptr) {}

HostIdentityClient::~HostIdentityClient() = default;

target::Outcome<std::unique_ptr<HostIdentityClient>>
HostIdentityClient::create(HostIdentityClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials ||
      options.budget <= std::chrono::milliseconds::zero() ||
      options.budget > options.maximum_budget ||
      options.maximum_budget > std::chrono::seconds{30}) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  return std::unique_ptr<HostIdentityClient>(new HostIdentityClient(std::move(options)));
}

bool HostIdentityClient::available() const noexcept {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

target::Outcome<target::AuthenticationContext>
HostIdentityClient::authenticate(grpc::ServerContext& transport,
                                 const v1::RequestContext& request) {
  try {
    const auto evidence = extract_peer_certificate(*transport.auth_context());
    if (const auto* rejected = std::get_if<t::Failure>(&evidence)) {
      return *rejected;
    }
    const auto& certificate = std::get<PeerCertificateEvidence>(evidence);
    if (certificate.der_chain.empty()) {
      return unauthenticated();
    }
    // Structural envelope decode only: audience=host, leaf binding, expiry and plaintext scope.
    // The authoritative signature/current-authority decision is the online check() below.
    auto decoded = decode_delegation_claims(request.delegation(), certificate.leaf_fingerprint,
                                            now(), t::Service::Host);
    if (const auto* rejected = std::get_if<t::Failure>(&decoded)) {
      return *rejected;
    }
    auto context = std::move(std::get<t::AuthenticationContext>(decoded));
    const auto* peer = std::get_if<t::PlatformPeer>(&context.direct_peer);
    if (!peer || peer->service != t::Service::Control || context.proof.empty() ||
        context.authorization_version.value == 0) {
      return unauthenticated();
    }
    return context;
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                      t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
}

target::Outcome<target::ResourceVersion>
HostIdentityClient::check(const target::AuthenticationContext& context,
                          target::Permission permission, const target::ResourceRef& resource,
                          target::TimePoint deadline) {
  try {
    if (!stub_) {
      return unavailable();
    }
    if (context.proof.empty() || resource.kind != t::ResourceKind::Host ||
        resource.id.value.empty()) {
      return unauthenticated();
    }
    const auto method = method_for(permission);
    if (method.empty()) {
      return failure(t::ErrorCode::CapabilityNotSupported);
    }
    const auto started = now();
    auto budget = std::min(options_.budget, options_.maximum_budget);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - started);
    if (remaining <= std::chrono::milliseconds::zero()) {
      return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
    }
    budget = std::min(budget, remaining);
    v1::IdentityCheckAuthorizationRequest input;
    auto* wire = input.mutable_context();
    wire->set_request_id(random_uuid());
    wire->set_trace_id(random_uuid());
    wire->set_idempotency_key(random_uuid());
    if (!timestamp(started + budget, wire->mutable_deadline())) {
      return failure(t::ErrorCode::Internal);
    }
    input.set_subject_proof(context.proof);
    input.set_full_method(std::string(method));
    const auto encoded = codec::to_wire(resource);
    if (!std::holds_alternative<v1::ResourceRef>(encoded)) {
      return unauthenticated();
    }
    *input.mutable_scope()->add_resources() = std::get<v1::ResourceRef>(encoded);
    input.set_known_authorization_version(context.authorization_version.value);
    v1::IdentityCheckAuthorizationResponse output;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::now() + budget);
    const auto status = stub_->CheckAuthorization(&call, input, &output);
    if (!status.ok()) {
      return grpc_failure(status);
    }
    if (!output.has_decision() || output.decision().authorization_version() == 0 ||
        !output.decision().has_checked_at() ||
        output.decision().allowed() == output.decision().has_denial()) {
      return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                        t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
    if (output.decision().has_denial() &&
        output.decision().denial().code() != v1::PERMISSION_DENIED) {
      return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                        t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
    const auto checked = codec::from_wire(output.decision().checked_at());
    const auto* checked_at = std::get_if<t::TimePoint>(&checked);
    const auto current = now();
    if (!checked_at || *checked_at > current + std::chrono::seconds{2} ||
        current - *checked_at > std::chrono::seconds{60}) {
      return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                        t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
    if (!output.decision().allowed()) {
      return failure(t::ErrorCode::PermissionDenied, t::Recovery::ReadCurrent);
    }
    return t::ResourceVersion{output.decision().authorization_version()};
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                      t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
}

} // namespace kiarabot::core::platform
