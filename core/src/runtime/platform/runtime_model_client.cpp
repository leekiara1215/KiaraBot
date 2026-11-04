#include "kiarabot/core/runtime/platform/runtime_model_client.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = kiarabot::core::target;
namespace codec = target_codec;
using namespace std::chrono_literals;
using SteadyClock = std::chrono::steady_clock;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose,
                   t::Acceptance acceptance = t::Acceptance::NotAccepted) {
  return {code, acceptance, recovery, {}, {}};
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
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
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  case grpc::StatusCode::PERMISSION_DENIED:
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::Authenticate);
  case grpc::StatusCode::INVALID_ARGUMENT:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  case grpc::StatusCode::UNAVAILABLE:
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  default:
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}
t::Failure unknown_delivery(t::ErrorCode code) {
  return failure(code, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
}
t::ErrorCode stream_failure_code(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return t::ErrorCode::Timeout;
  case grpc::StatusCode::CANCELLED:
  case grpc::StatusCode::UNAVAILABLE:
  case grpc::StatusCode::UNKNOWN:
    return t::ErrorCode::UpstreamUnavailable;
  default:
    return t::ErrorCode::Internal;
  }
}
} // namespace

ModelDelegationProvider unavailable_model_delegation() {
  return [](const t::RuntimeModelChatRequest&) -> t::Outcome<std::string> {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  };
}

RuntimeModelClient::RuntimeModelClient(RuntimeModelClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(v1::ModelService::NewStub(channel_)) {}

RuntimeModelClient::~RuntimeModelClient() = default;

t::Outcome<std::unique_ptr<RuntimeModelClient>>
RuntimeModelClient::create(RuntimeModelClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || !options.delegation_provider ||
      options.maximum_budget.count() <= 0) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<RuntimeModelClient>(new RuntimeModelClient(std::move(options)));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

bool RuntimeModelClient::available() const noexcept {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

t::Outcome<t::RuntimeModelChatReply>
RuntimeModelClient::chat(const t::RuntimeModelChatRequest& request, t::TimePoint deadline) {
  try {
    if (request.logical_call_id.empty() || request.model.empty() || request.messages.empty()) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    const auto started = now();
    if (deadline <= started) {
      return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
    }
    auto delegation = options_.delegation_provider(request);
    if (const auto* rejected = std::get_if<t::Failure>(&delegation)) {
      return *rejected;
    }
    auto& proof = std::get<std::string>(delegation);
    if (proof.empty() || proof.size() > 32768) {
      return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
    }
    v1::ModelGatewayChatRequest input;
    auto* context = input.mutable_context();
    const std::string request_id =
        request.request_id.empty() ? request.logical_call_id : request.request_id;
    context->set_request_id(request_id);
    // Model requires both transport identifiers to be canonical UUIDs; the plugin host request
    // carries one id, so reuse it as the trace id rather than sending an empty value.
    context->set_trace_id(request_id);
    // Chat is a mutating call: the durable logical_call_id is also the idempotency key so a
    // retry with the same call identity replays instead of re-billing the provider.
    context->set_idempotency_key(request.logical_call_id);
    if (!timestamp(std::min(deadline, started + options_.maximum_budget),
                   context->mutable_deadline())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    context->set_delegation(proof);
    input.set_logical_call_id(request.logical_call_id);
    input.set_model_id(request.model);
    for (const auto& message : request.messages) {
      auto* wire = input.add_messages();
      wire->set_role(message.role);
      wire->set_text(message.text);
    }
    v1::ModelGatewayChatResponse output;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::min(deadline, started + options_.maximum_budget).time_since_epoch())});
    const auto status = stub_->Chat(&call, input, &output);
    if (!status.ok()) {
      return grpc_failure(status);
    }
    if (output.disposition() == v1::FRESH_RESULT) {
      if (!output.has_result()) {
        return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                       t::Acceptance::Unknown);
      }
      return t::RuntimeModelChatReply{output.result().text(), output.result().input_tokens(),
                                      output.result().output_tokens()};
    }
    if (output.disposition() == v1::REPLAY_TERMINAL && output.has_result()) {
      return t::RuntimeModelChatReply{output.result().text(), output.result().input_tokens(),
                                      output.result().output_tokens()};
    }
    // REPLAY_IN_PROGRESS and unknown dispositions never fabricate a result; the plugin must
    // re-issue/query with the same logical_call_id (QUERY_SAME_INTENT recovery).
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::QuerySameIdentity,
                   t::Acceptance::Unknown);
  } catch (...) {
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}

t::Outcome<t::RuntimeModelStreamResult>
RuntimeModelClient::chat_stream(const t::RuntimeModelStreamRequest& request,
                                t::RuntimeModelStreamSink& sink, std::stop_token cancellation,
                                t::TimePoint deadline) {
  try {
    if (!t::valid_model_identifier(request.request_id) ||
        !t::valid_model_identifier(request.trace_id) ||
        !t::valid_model_identifier(request.logical_call_id) || request.model.empty() ||
        request.model.size() > 256 || request.messages.empty() ||
        request.messages.size() > t::kModelMaximumMessages) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    std::size_t input_bytes = 0;
    for (const auto& message : request.messages) {
      if (message.text.size() > t::kModelMaximumRequestBytes - input_bytes ||
          !t::valid_model_message_role(message.role) ||
          message.text.find('\0') != std::string::npos) {
        return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
      }
      input_bytes += message.text.size();
    }
    const auto started = now();
    if (deadline <= started) {
      return unknown_delivery(t::ErrorCode::Timeout);
    }
    if (cancellation.stop_requested()) {
      t::RuntimeModelStreamResult stopped;
      stopped.cancelled_by_consumer = true;
      return stopped;
    }
    t::RuntimeModelChatRequest legacy;
    legacy.instance = request.instance;
    legacy.installation = request.installation;
    legacy.request_id = request.request_id;
    legacy.logical_call_id = request.logical_call_id;
    legacy.model = request.model;
    legacy.messages = request.messages;
    auto delegation = options_.delegation_provider(legacy);
    if (const auto* rejected = std::get_if<t::Failure>(&delegation)) {
      return *rejected;
    }
    auto& proof = std::get<std::string>(delegation);
    if (proof.empty() || proof.size() > 32768) {
      return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
    }
    v1::ModelGatewayChatRequest input;
    auto* context = input.mutable_context();
    context->set_request_id(request.request_id);
    context->set_trace_id(request.trace_id);
    // A stream attempt never carries the unary command idempotency key.
    context->set_delegation(proof);
    const auto attempt_deadline = std::min(deadline, started + options_.maximum_budget);
    if (!timestamp(attempt_deadline, context->mutable_deadline())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    input.set_logical_call_id(request.logical_call_id);
    input.set_model_id(request.model);
    for (const auto& message : request.messages) {
      auto* wire = input.add_messages();
      wire->set_role(message.role);
      wire->set_text(message.text);
    }
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            attempt_deadline.time_since_epoch())});
    const auto wall_deadline = attempt_deadline;
    const auto remaining = wall_deadline > now() ? wall_deadline - now() : t::TimePoint::duration{};
    const auto steady_deadline =
        SteadyClock::now() + std::chrono::duration_cast<SteadyClock::duration>(remaining);
    auto reader = stub_->ChatStream(&call, input);
    if (!reader) {
      return unknown_delivery(t::ErrorCode::Internal);
    }
    // gRPC blocking reads can only be interrupted through the call; the callback is declared
    // before the reader loop and is unregistered before the call leaves this scope.
    std::stop_callback cancel{cancellation, [&call] { call.TryCancel(); }};
    ModelStreamDecoder decoder{request.request_id, request.logical_call_id, request.trace_id};
    t::RuntimeModelStreamResult result;
    std::optional<t::ModelStreamTerminal> terminal;
    bool consumer_stopped = false;
    bool malformed = false;
    bool saw_started = false;
    v1::ModelStreamEvent frame;
    while (reader->Read(&frame)) {
      auto decoded = decoder.decode(frame);
      if (!std::holds_alternative<t::ModelStreamEvent>(decoded)) {
        malformed = true;
        call.TryCancel();
        break;
      }
      const auto& event = std::get<t::ModelStreamEvent>(decoded);
      saw_started = saw_started || std::holds_alternative<t::ModelStreamStarted>(event.body);
      if (const auto* delta = std::get_if<t::ModelStreamDelta>(&event.body)) {
        result.received_bytes += delta->text.size();
        ++result.received_delta_count;
      }
      auto admitted = sink.try_emit(event);
      for (auto retry = SteadyClock::now() + std::chrono::milliseconds{250};
           admitted == t::ModelStreamSinkResult::Full && !cancellation.stop_requested() &&
           SteadyClock::now() < steady_deadline && SteadyClock::now() < retry;) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        admitted = sink.try_emit(event);
      }
      if (admitted == t::ModelStreamSinkResult::Full) {
        call.TryCancel();
        consumer_stopped = true;
        break;
      }
      if (admitted == t::ModelStreamSinkResult::Cancelled) {
        call.TryCancel();
        consumer_stopped = true;
        break;
      }
      if (const auto* value = std::get_if<t::ModelStreamTerminal>(&event.body)) {
        terminal = *value;
        break;
      }
    }
    const auto status = reader->Finish();
    if (terminal) {
      result.terminal = std::move(terminal);
      result.cancelled_by_consumer = false;
      return result;
    }
    if (consumer_stopped || cancellation.stop_requested()) {
      result.cancelled_by_consumer = true;
      return result;
    }
    if (malformed) {
      return unknown_delivery(t::ErrorCode::UpstreamUnavailable);
    }
    if (!saw_started) {
      // Pre-start rejection: the structured trailer is the authority, never a fabricated terminal.
      auto wire = codec::from_grpc_failure(status);
      if (const auto* failure_wire = std::get_if<v1::RpcFailure>(&wire)) {
        auto structured =
            decode_model_stream_failure(*failure_wire, request.request_id, request.trace_id);
        if (structured) {
          return *structured;
        }
      }
      return grpc_failure(status);
    }
    // A started attempt that lost its terminal is unknown delivery, never completion.
    return unknown_delivery(stream_failure_code(status));
  } catch (...) {
    return unknown_delivery(t::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
