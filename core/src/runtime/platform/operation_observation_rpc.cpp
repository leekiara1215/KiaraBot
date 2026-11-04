#include "kiarabot/core/runtime/platform/operation_observation_rpc.hpp"

#include <algorithm>
#include <chrono>
#include <string_view>

#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace codec = target_codec;
t::Failure invalid() {
  return {
      t::ErrorCode::InvalidArgument, t::Acceptance::NotAccepted, t::Recovery::CorrectInput, {}, {}};
}
t::Failure internal() {
  return {t::ErrorCode::Internal, t::Acceptance::NotAccepted, t::Recovery::Diagnose, {}, {}};
}
t::Failure unauthenticated() {
  return {
      t::ErrorCode::Unauthenticated, t::Acceptance::NotAccepted, t::Recovery::Authenticate, {}, {}};
}
t::Failure timeout() {
  return {t::ErrorCode::Timeout, t::Acceptance::NotAccepted, t::Recovery::Backoff, {}, {}};
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
std::string_view owner_text(t::Service owner) {
  switch (owner) {
  case t::Service::Runtime:
    return "runtime";
  case t::Service::Scheduler:
    return "scheduler";
  case t::Service::Model:
    return "model";
  case t::Service::Host:
    return "host";
  default:
    return {};
  }
}
std::string_view method(t::Service owner) {
  switch (owner) {
  case t::Service::Runtime:
    return "kiarabot.v1.RuntimeService/ListOperationChanges";
  case t::Service::Scheduler:
    return "kiarabot.v1.SchedulerService/ListOperationChanges";
  case t::Service::Model:
    return "kiarabot.v1.ModelService/ListOperationChanges";
  case t::Service::Host:
    return "kiarabot.v1.HostManagementService/ListOperationChanges";
  default:
    return {};
  }
}
t::Outcome<std::vector<t::ResourceRef>> scope(const v1::ResourceScope& input) {
  if (input.resources_size() == 0 || input.resources_size() > 127) {
    return invalid();
  }
  std::vector<t::ResourceRef> result;
  for (const auto& item : input.resources()) {
    const auto decoded = codec::from_wire(item);
    if (const auto* failure = std::get_if<t::Failure>(&decoded)) {
      return *failure;
    }
    result.push_back(std::get<t::ResourceRef>(decoded));
  }
  return result;
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* result = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *result;
    return true;
  }
  return false;
}
bool cursor(const t::OperationObservationCursor& input, v1::StreamCursor* output) {
  if (owner_text(input.owner).empty() || input.epoch.empty() || input.scope.empty()) {
    return false;
  }
  output->set_source_service(std::string{owner_text(input.owner)});
  output->set_epoch(input.epoch);
  output->set_sequence(input.sequence);
  for (const auto& item : input.scope) {
    const auto encoded = codec::to_wire(item);
    if (!std::holds_alternative<v1::ResourceRef>(encoded)) {
      return false;
    }
    *output->mutable_scope()->add_resources() = std::get<v1::ResourceRef>(encoded);
  }
  return true;
}
bool error(const std::optional<t::ErrorCode>& code, const v1::RequestContext& context,
           v1::Error* output) {
  if (!code) {
    return true;
  }
  const auto encoded = codec::make_error(*code, context.request_id(), context.trace_id(), false);
  if (!std::holds_alternative<v1::Error>(encoded)) {
    return false;
  }
  *output = std::get<v1::Error>(encoded);
  return true;
}
bool operation(const t::Operation& input, const v1::RequestContext& context,
               v1::Operation* output) {
  const auto state = static_cast<int>(input.state);
  const auto outcome = static_cast<int>(input.execution_outcome);
  if (state < 0 || state > 6 || outcome < 0 || outcome > 3 || input.updated_at <= t::TimePoint{} ||
      (input.state == t::OperationState::Blocked &&
       (!input.blocked_reason || !input.next_attempt))) {
    return false;
  }
  output->mutable_ref()->set_owner_service(std::string{owner_text(input.ref.owner)});
  output->mutable_ref()->set_operation_id(input.ref.id.value);
  const auto resource = codec::to_wire(input.ref.resource);
  if (!std::holds_alternative<v1::ResourceRef>(resource)) {
    return false;
  }
  *output->mutable_ref()->mutable_resource() = std::get<v1::ResourceRef>(resource);
  output->set_version(input.version.value);
  output->set_state(static_cast<v1::OperationState>(state + 1));
  output->set_execution_outcome(static_cast<v1::ExecutionOutcome>(outcome + 1));
  if (input.blocked_reason && !error(input.blocked_reason, context, output->mutable_reason())) {
    return false;
  }
  if (input.next_attempt && !timestamp(*input.next_attempt, output->mutable_next_attempt_at())) {
    return false;
  }
  if (!timestamp(input.updated_at, output->mutable_observed_at())) {
    return false;
  }
  for (const auto& item : input.participants) {
    const auto status = static_cast<int>(item.state);
    if (status < 0 || status > 6 || item.participant.empty() || item.participant.size() > 128) {
      return false;
    }
    auto* participant = output->add_participants();
    participant->set_participant(item.participant);
    participant->set_state(static_cast<v1::OperationState>(status + 1));
    if (item.reason && !error(item.reason, context, participant->mutable_reason())) {
      return false;
    }
    if (item.next_attempt &&
        !timestamp(*item.next_attempt, participant->mutable_next_attempt_at())) {
      return false;
    }
  }
  return true;
}
} // namespace

t::Outcome<t::OperationObservationCommand> prepare_operation_observation(
    grpc::ServerContext& transport, const v1::OperationChangesRequest& request, t::Service owner,
    const std::function<t::Outcome<t::TimePoint>()>& invocation_deadline) {
  try {
    const auto& context = request.context();
    if (!request.has_context() || !context.has_deadline() || !request.has_scope() ||
        method(owner).empty() || request.page_size() > 100) {
      return invalid();
    }
    const auto decoded = codec::from_wire(context.deadline());
    if (!std::holds_alternative<t::TimePoint>(decoded)) {
      return invalid();
    }
    auto deadline =
        std::min(std::get<t::TimePoint>(decoded), now() + std::chrono::milliseconds{500});
    if (transport.deadline() != std::chrono::system_clock::time_point::max()) {
      deadline =
          std::min(deadline, t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 transport.deadline().time_since_epoch())});
    }
    if (invocation_deadline) {
      const auto own = invocation_deadline();
      if (const auto* failure = std::get_if<t::Failure>(&own)) {
        return *failure;
      }
      deadline = std::min(deadline, std::get<t::TimePoint>(own));
    }
    if (transport.IsCancelled() || deadline <= now()) {
      return timeout();
    }
    const auto selected = scope(request.scope());
    if (const auto* failure = std::get_if<t::Failure>(&selected)) {
      return *failure;
    }
    const auto authentication = transport.auth_context();
    if (!authentication) {
      return unauthenticated();
    }
    const auto evidence = extract_peer_certificate(*authentication);
    if (const auto* failure = std::get_if<t::Failure>(&evidence)) {
      return *failure;
    }
    const auto& certificate = std::get<PeerCertificateEvidence>(evidence);
    constexpr std::string_view kControlSan = "spiffe://kiarabot/control";
    const bool control =
        std::any_of(certificate.uri_sans.begin(), certificate.uri_sans.end(), [&](const auto& san) {
          return san == kControlSan || san.starts_with(std::string{kControlSan} + "/") ||
                 san.starts_with(std::string{kControlSan} + "-");
        });
    if (!control) {
      return unauthenticated();
    }
    auto claims =
        decode_delegation_claims(context.delegation(), certificate.leaf_fingerprint, now(), owner);
    if (const auto* failure = std::get_if<t::Failure>(&claims)) {
      return *failure;
    }
    auto identity = std::move(std::get<t::AuthenticationContext>(claims));
    auto* peer = std::get_if<t::PlatformPeer>(&identity.direct_peer);
    if (!peer) {
      return unauthenticated();
    }
    // The shared Scheduler metadata decoder historically assumes Runtime caller.
    // Here the ACTUAL leaf SAN binds Control; Identity still verifies its current
    // registered credential and the proof on every pure-application authorization.
    peer->service = t::Service::Control;
    t::OperationObservationCommand result;
    result.context.authentication = std::move(identity);
    result.context.request_id = context.request_id();
    result.context.trace_id = context.trace_id();
    result.context.method = std::string{method(owner)};
    result.context.deadline = deadline;
    result.query.scope = std::get<std::vector<t::ResourceRef>>(selected);
    result.context.resource = result.query.scope.front();
    result.query.page_size = request.page_size();
    if (request.has_cursor()) {
      const auto& input = request.cursor();
      if (input.source_service() != owner_text(owner) || !input.has_scope()) {
        return invalid();
      }
      const auto resources = scope(input.scope());
      if (const auto* failure = std::get_if<t::Failure>(&resources)) {
        return *failure;
      }
      result.query.cursor = t::OperationObservationCursor{
          owner, input.epoch(), std::get<std::vector<t::ResourceRef>>(resources), input.sequence()};
    }
    const auto remaining = deadline - now();
    if (remaining <= t::TimePoint::duration::zero() || transport.IsCancelled()) {
      return timeout();
    }
    result.query.deadline = std::chrono::steady_clock::now() + remaining;
    return result;
  } catch (...) {
    return internal();
  }
}

t::Status encode_operation_observation_page(const t::OperationObservationPage& page,
                                            const v1::RequestContext& request,
                                            v1::OperationChangesResponse& response) {
  try {
    v1::OperationChangesResponse result;
    if (!cursor(page.next_cursor, result.mutable_next_cursor()) ||
        !timestamp(page.checked_at, result.mutable_checked_at())) {
      return internal();
    }
    result.set_has_more(page.has_more);
    for (const auto& item : page.changes) {
      auto* change = result.add_changes();
      if (!cursor(item.cursor, change->mutable_cursor()) ||
          !operation(item.operation, request, change->mutable_operation())) {
        return internal();
      }
    }
    if (result.ByteSizeLong() > 1024 * 1024) {
      return internal();
    }
    response.Swap(&result);
    return std::monostate{};
  } catch (...) {
    return internal();
  }
}
} // namespace kiarabot::core::platform
