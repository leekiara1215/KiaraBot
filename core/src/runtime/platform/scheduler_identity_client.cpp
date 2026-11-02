#include "kiarabot/core/runtime/platform/scheduler_identity_client.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/identity.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace v1 = kiarabot::v1;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
t::Failure unavailable() {
  return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
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
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = target_codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
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
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                      t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
}
// Derive the installation owner from the delegation permission bindings. The frozen delegation
// claim has no dedicated installation field; the issuer must bind an Installation resource scope
// for task delegation (Runtime/Identity convention). Absence is reported, never guessed.
target::Outcome<t::InstallationId>
installation_from(const t::AuthenticationContext& authentication) {
  std::optional<t::InstallationId> installation;
  for (const auto& scope : authentication.scopes) {
    if (scope.resource.kind != t::ResourceKind::Installation) {
      continue;
    }
    if (!canonical_uuid(scope.resource.id.value)) {
      return unauthenticated();
    }
    if (installation && installation->value != scope.resource.id.value) {
      return unauthenticated();
    }
    installation = t::InstallationId{scope.resource.id.value};
  }
  if (!installation) {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  }
  return *installation;
}
} // namespace

SchedulerIdentityClient::SchedulerIdentityClient(SchedulerIdentityClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(channel_ ? v1::IdentityService::NewStub(channel_) : nullptr) {}

SchedulerIdentityClient::~SchedulerIdentityClient() = default;

target::Outcome<std::unique_ptr<SchedulerIdentityClient>>
SchedulerIdentityClient::create(SchedulerIdentityClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || options.budget.count() <= 0 ||
      options.maximum_budget.count() <= 0 || options.budget > options.maximum_budget) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<SchedulerIdentityClient>(
        new SchedulerIdentityClient(std::move(options)));
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

bool SchedulerIdentityClient::available() const noexcept {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

target::Outcome<target::SchedulerCommandContext>
SchedulerIdentityClient::verify(std::string_view proof, const t::Sha256Digest& leaf_fingerprint,
                                const v1::RequestContext& request, std::string_view method,
                                const v1::ResourceScope* scope) {
  try {
    if (proof.empty() || method.empty()) {
      return unauthenticated();
    }
    auto decoded = decode_delegation_claims(proof, leaf_fingerprint, now(), t::Service::Scheduler);
    if (const auto* rejected = std::get_if<t::Failure>(&decoded)) {
      return *rejected;
    }
    const auto authentication = std::move(std::get<t::AuthenticationContext>(decoded));
    if (!canonical_uuid(authentication.subject.value) ||
        authentication.authorization_version.value == 0) {
      return unauthenticated();
    }
    auto installation = installation_from(authentication);
    if (const auto* rejected = std::get_if<t::Failure>(&installation)) {
      return *rejected;
    }
    if (!stub_) {
      return unavailable();
    }
    const auto started = now();
    auto deadline = started + std::min(options_.budget, options_.maximum_budget);
    const auto requested = target_codec::from_wire(request.deadline());
    if (const auto* wire = std::get_if<t::TimePoint>(&requested); wire && *wire < deadline) {
      deadline = *wire;
    }
    if (deadline <= started) {
      return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity);
    }
    v1::IdentityCheckAuthorizationRequest input;
    auto* context = input.mutable_context();
    context->set_request_id(random_uuid());
    context->set_trace_id(random_uuid());
    if (!timestamp(deadline, context->mutable_deadline())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    input.set_subject_proof(std::string(proof));
    input.set_full_method(std::string(method));
    if (scope != nullptr) {
      *input.mutable_scope() = *scope;
    }
    input.set_known_authorization_version(authentication.authorization_version.value);
    v1::IdentityCheckAuthorizationResponse output;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            deadline.time_since_epoch())});
    const auto status = stub_->CheckAuthorization(&call, input, &output);
    if (!status.ok()) {
      return grpc_failure(status);
    }
    if (!output.has_decision() || output.decision().authorization_version() == 0 ||
        !output.decision().has_checked_at()) {
      return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                        t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
    const auto checked = target_codec::from_wire(output.decision().checked_at());
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
    if (output.decision().has_denial()) {
      return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                        t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
    t::SchedulerCommandContext command;
    command.owner = t::InstanceId{authentication.subject.value};
    command.installation = std::get<t::InstallationId>(installation);
    command.request_id = request.request_id();
    command.trace_id = request.trace_id();
    command.deadline = deadline;
    command.idempotency_key = request.idempotency_key();
    return command;
  } catch (const std::bad_alloc&) {
    return unavailable();
  } catch (...) {
    return t::Failure{t::ErrorCode::Internal, t::Acceptance::Unknown,
                      t::Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
  }
}

target::Outcome<target::SchedulerCommandContext>
SchedulerIdentityClient::authenticate(grpc::ServerContext& transport,
                                      const v1::RequestContext& request, std::string_view method,
                                      const v1::ResourceScope* scope) {
  const auto evidence = extract_peer_certificate(*transport.auth_context());
  if (const auto* rejected = std::get_if<t::Failure>(&evidence)) {
    return *rejected;
  }
  const auto& certificate = std::get<PeerCertificateEvidence>(evidence);
  return verify(request.delegation(), certificate.leaf_fingerprint, request, method, scope);
}

} // namespace kiarabot::core::platform
