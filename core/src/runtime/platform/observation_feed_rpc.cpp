#include "kiarabot/core/runtime/platform/observation_feed_rpc.hpp"

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
  case t::Service::Identity:
    return "identity";
  case t::Service::Runtime:
    return "runtime";
  case t::Service::Scheduler:
    return "scheduler";
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
std::string_view task_state_name(t::TaskFeedState state) {
  switch (state) {
  case t::TaskFeedState::Pending:
    return "SCHEDULED_TASK_PENDING";
  case t::TaskFeedState::Running:
    return "SCHEDULED_TASK_RUNNING";
  case t::TaskFeedState::Retrying:
    return "SCHEDULED_TASK_RETRYING";
  case t::TaskFeedState::Completed:
    return "SCHEDULED_TASK_COMPLETED";
  case t::TaskFeedState::Failed:
    return "SCHEDULED_TASK_FAILED";
  case t::TaskFeedState::Cancelled:
    return "SCHEDULED_TASK_CANCELLED";
  }
  return {};
}
bool log_metadata(const t::LogObservation& log, v1::LogMetadata* output) {
  if (log.owner != t::Service::Identity && log.owner != t::Service::Runtime) {
    return false;
  }
  output->set_owner_service(std::string{owner_text(log.owner)});
  output->set_entry_id(log.entry_id);
  output->set_severity(log.severity);
  output->set_component(log.component);
  output->set_method(log.method);
  output->set_event(log.event);
  const auto resource = codec::to_wire(log.resource);
  if (!std::holds_alternative<v1::ResourceRef>(resource)) {
    return false;
  }
  *output->mutable_resource() = std::get<v1::ResourceRef>(resource);
  output->set_request_id(log.request_id);
  output->set_trace_id(log.trace_id);
  return timestamp(log.occurred_at, output->mutable_occurred_at());
}
bool task_metadata(const t::TaskObservation& task, v1::TaskMetadata* output) {
  output->set_owner_service("scheduler");
  output->set_task_id(task.task_id);
  output->set_owner_instance_id(task.owner_instance_id);
  v1::ScheduledTaskState state;
  if (!v1::ScheduledTaskState_Parse(std::string{task_state_name(task.state)}, &state)) {
    return false;
  }
  output->set_state(state);
  output->set_version(task.version);
  if (!timestamp(task.updated_at, output->mutable_updated_at())) {
    return false;
  }
  output->set_cancel_requested(task.cancel_requested);
  if (task.occurrence_id) {
    output->set_occurrence_id(*task.occurrence_id);
  }
  if (task.reason_code) {
    const auto code = codec::to_wire(*task.reason_code);
    const auto* wire = std::get_if<v1::ErrorCode>(&code);
    if (wire == nullptr) {
      return false;
    }
    output->set_reason_code(*wire);
  }
  return true;
}
bool message_metadata(const t::MessageObservation& message, v1::MessageMetadata* output) {
  output->set_owner_service("runtime");
  output->set_message_id(message.message_id);
  output->set_conversation_id(message.conversation_id);
  output->set_account_id(message.account_id);
  return timestamp(message.received_at, output->mutable_received_at());
}
bool change_cursor(const t::ObservationFeedCursor& value, v1::StreamCursor* output) {
  if (value.epoch.empty()) {
    return false;
  }
  output->set_source_service(std::string{owner_text(value.owner)});
  output->set_epoch(value.epoch);
  output->set_sequence(value.sequence);
  for (const auto& item : value.scope) {
    const auto encoded = codec::to_wire(item);
    if (!std::holds_alternative<v1::ResourceRef>(encoded)) {
      return false;
    }
    *output->mutable_scope()->add_resources() = std::get<v1::ResourceRef>(encoded);
  }
  return true;
}
} // namespace

t::Outcome<t::ObservationFeedCommand>
prepare_observation_feed(grpc::ServerContext& transport,
                         const v1::ObservationChangesRequest& request, t::Service owner,
                         t::ObservationFamily family,
                         const std::function<t::Outcome<t::TimePoint>()>& invocation_deadline) {
  try {
    const auto method = t::observation_feed_method(owner, family);
    if (method.empty() || owner == t::Service::Identity) {
      // Identity log changes authorize the registered Control platform peer directly and use a
      // separate owner-side entry point; this helper serves Runtime/Scheduler delegation peers.
      return {t::Failure{t::ErrorCode::CapabilityNotSupported,
                         t::Acceptance::NotAccepted,
                         t::Recovery::Diagnose,
                         {},
                         {}}};
    }
    const auto& context = request.context();
    if (!request.has_context() || !context.has_deadline() || !request.has_scope() ||
        request.page_size() > 100) {
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
    auto selected = scope(request.scope());
    if (const auto* failure = std::get_if<t::Failure>(&selected)) {
      return *failure;
    }
    auto resources = std::move(std::get<std::vector<t::ResourceRef>>(selected));
    if (!t::observation_feed_scope(owner, family, resources)) {
      return invalid();
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
    // The actual leaf SAN binds Control; Identity still verifies the registered credential and the
    // proof on every pure-application authorization.
    peer->service = t::Service::Control;
    t::ObservationFeedCommand result;
    result.context.authentication = std::move(identity);
    result.context.request_id = context.request_id();
    result.context.trace_id = context.trace_id();
    result.context.method = std::string{method};
    result.context.deadline = deadline;
    result.query.family = family;
    result.query.scope = std::move(resources);
    result.context.resource = result.query.scope.front();
    result.query.page_size = request.page_size();
    if (request.has_cursor()) {
      const auto& input = request.cursor();
      if (input.source_service() != owner_text(owner) || !input.has_scope()) {
        return invalid();
      }
      const auto cursor_scope = scope(input.scope());
      if (const auto* failure = std::get_if<t::Failure>(&cursor_scope)) {
        return *failure;
      }
      auto cursor_resources = std::get<std::vector<t::ResourceRef>>(cursor_scope);
      if (!t::observation_feed_scope(owner, family, cursor_resources) ||
          cursor_resources != result.query.scope || input.epoch().empty() ||
          input.epoch().size() > 128) {
        return invalid();
      }
      result.query.cursor = t::ObservationFeedCursor{owner, family, input.epoch(),
                                                     std::move(cursor_resources), input.sequence()};
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

t::Status encode_log_changes_page(const t::ObservationFeedPage& page,
                                  const v1::RequestContext& request,
                                  v1::LogChangesResponse& response) {
  static_cast<void>(request);
  try {
    v1::LogChangesResponse result;
    if (!change_cursor(page.next_cursor, result.mutable_next_cursor()) ||
        !timestamp(page.checked_at, result.mutable_checked_at())) {
      return internal();
    }
    result.set_has_more(page.has_more);
    for (const auto& item : page.changes) {
      const auto* log = std::get_if<t::LogObservation>(&item.payload);
      if (log == nullptr) {
        return internal();
      }
      auto* change = result.add_changes();
      if (!change_cursor(item.cursor, change->mutable_cursor()) ||
          !log_metadata(*log, change->mutable_log())) {
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

t::Status encode_task_changes_page(const t::ObservationFeedPage& page,
                                   const v1::RequestContext& request,
                                   v1::TaskChangesResponse& response) {
  static_cast<void>(request);
  try {
    v1::TaskChangesResponse result;
    if (!change_cursor(page.next_cursor, result.mutable_next_cursor()) ||
        !timestamp(page.checked_at, result.mutable_checked_at())) {
      return internal();
    }
    result.set_has_more(page.has_more);
    for (const auto& item : page.changes) {
      const auto* task = std::get_if<t::TaskObservation>(&item.payload);
      if (task == nullptr) {
        return internal();
      }
      auto* change = result.add_changes();
      if (!change_cursor(item.cursor, change->mutable_cursor()) ||
          !task_metadata(*task, change->mutable_task())) {
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

t::Status encode_message_metadata_changes_page(const t::ObservationFeedPage& page,
                                               const v1::RequestContext& request,
                                               v1::MessageMetadataChangesResponse& response) {
  static_cast<void>(request);
  try {
    v1::MessageMetadataChangesResponse result;
    if (!change_cursor(page.next_cursor, result.mutable_next_cursor()) ||
        !timestamp(page.checked_at, result.mutable_checked_at())) {
      return internal();
    }
    result.set_has_more(page.has_more);
    for (const auto& item : page.changes) {
      const auto* message = std::get_if<t::MessageObservation>(&item.payload);
      if (message == nullptr) {
        return internal();
      }
      auto* change = result.add_changes();
      if (!change_cursor(item.cursor, change->mutable_cursor()) ||
          !message_metadata(*message, change->mutable_message())) {
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
