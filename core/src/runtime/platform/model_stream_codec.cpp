#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <utility>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
namespace codec = target_codec;
t::Failure invalid() {
  return {t::ErrorCode::InvalidArgument,
          t::Acceptance::Unknown,
          t::Recovery::QuerySameIdentity,
          {},
          {}};
}
bool usage_valid(const v1::ModelStreamUsage& value) {
  return value.known() || (value.input_tokens() == 0 && value.output_tokens() == 0);
}
void usage_wire(const t::ModelProviderUsage& value, v1::ModelStreamUsage* out) {
  out->set_known(value.known);
  out->set_input_tokens(value.input_tokens);
  out->set_output_tokens(value.output_tokens);
}
bool usage_equal(const v1::ModelStreamUsage& a, const v1::ModelStreamUsage& b) {
  return a.known() == b.known() && a.input_tokens() == b.input_tokens() &&
         a.output_tokens() == b.output_tokens();
}
bool call_wire(const t::ModelIntent& value, const t::ModelStreamEvent& event,
               v1::ModelStreamCallSnapshot* out) {
  out->set_logical_call_id(value.logical_call_id);
  out->set_owner_instance_id(value.owner.value);
  out->set_model_id(value.model.value);
  out->set_config_version(value.config_version.value);
  out->set_secret_version(value.secret_version.value);
  switch (value.state) {
  case t::ModelCallState::Started:
    out->set_state(v1::OPERATION_RUNNING);
    out->set_execution_outcome(v1::EXECUTION_UNKNOWN);
    break;
  case t::ModelCallState::Succeeded:
    out->set_state(v1::OPERATION_SUCCEEDED);
    out->set_execution_outcome(v1::EXECUTION_KNOWN_SUCCEEDED);
    break;
  case t::ModelCallState::Failed:
    out->set_state(v1::OPERATION_FAILED);
    out->set_execution_outcome(value.disposition == t::ModelCallDisposition::Abandoned
                                   ? v1::EXECUTION_UNKNOWN
                                   : v1::EXECUTION_KNOWN_FAILED);
    break;
  case t::ModelCallState::Unknown:
    out->set_state(v1::OPERATION_BLOCKED);
    out->set_execution_outcome(v1::EXECUTION_UNKNOWN);
    break;
  }
  usage_wire({value.usage_known, value.input_tokens, value.output_tokens}, out->mutable_usage());
  out->set_may_have_billed(value.may_have_billed);
  auto stamp = codec::to_wire(value.observed_at);
  if (!std::holds_alternative<google::protobuf::Timestamp>(stamp)) {
    return false;
  }
  *out->mutable_observed_at() = std::get<google::protobuf::Timestamp>(stamp);
  if (value.error) {
    auto error = codec::make_error(*value.error, event.request_id, event.trace_id, false);
    if (!std::holds_alternative<v1::Error>(error)) {
      return false;
    }
    *out->mutable_reason() = std::get<v1::Error>(error);
  }
  out->set_provider_request_id(value.provider_request_id);
  out->set_latency_ms(value.latency_ms);
  return true;
}
std::optional<t::ModelIntent> call_value(const v1::ModelStreamCallSnapshot& value,
                                         const std::string& logical_id) {
  if (value.logical_call_id() != logical_id || !t::valid_model_identifier(logical_id) ||
      !t::valid_model_identifier(value.owner_instance_id()) ||
      !t::valid_model_identifier(value.model_id()) || value.config_version() == 0 ||
      value.secret_version() == 0 || !value.has_usage() || !usage_valid(value.usage()) ||
      !value.has_may_have_billed() || !value.has_observed_at() ||
      value.observed_at().seconds() <= 0 ||
      !t::valid_provider_request_id(value.provider_request_id()) || value.latency_ms() > 300000) {
    return std::nullopt;
  }
  t::ModelIntent out;
  switch (value.state()) {
  case v1::OPERATION_RUNNING:
    if (value.execution_outcome() != v1::EXECUTION_UNKNOWN || !value.may_have_billed()) {
      return std::nullopt;
    }
    out.state = t::ModelCallState::Started;
    break;
  case v1::OPERATION_BLOCKED:
    if (value.execution_outcome() != v1::EXECUTION_UNKNOWN || !value.may_have_billed()) {
      return std::nullopt;
    }
    out.state = t::ModelCallState::Unknown;
    break;
  case v1::OPERATION_SUCCEEDED:
    if (value.execution_outcome() != v1::EXECUTION_KNOWN_SUCCEEDED) {
      return std::nullopt;
    }
    out.state = t::ModelCallState::Succeeded;
    break;
  case v1::OPERATION_FAILED:
    if (value.execution_outcome() != v1::EXECUTION_KNOWN_FAILED &&
        value.execution_outcome() != v1::EXECUTION_UNKNOWN) {
      return std::nullopt;
    }
    out.state = t::ModelCallState::Failed;
    if (value.execution_outcome() == v1::EXECUTION_UNKNOWN) {
      out.disposition = t::ModelCallDisposition::Abandoned;
    }
    break;
  default:
    return std::nullopt;
  }
  const auto stamp = codec::from_wire(value.observed_at());
  if (!std::holds_alternative<t::TimePoint>(stamp)) {
    return std::nullopt;
  }
  out.observed_at = std::get<t::TimePoint>(stamp);
  out.owner = {value.owner_instance_id()};
  out.logical_call_id = logical_id;
  out.model = {value.model_id()};
  out.config_version = {value.config_version()};
  out.secret_version = {value.secret_version()};
  out.usage_known = value.usage().known();
  out.input_tokens = value.usage().input_tokens();
  out.output_tokens = value.usage().output_tokens();
  out.may_have_billed = value.may_have_billed();
  out.provider_request_id = value.provider_request_id();
  out.latency_ms = value.latency_ms();
  if (value.has_reason()) {
    auto code = codec::from_wire(value.reason().code());
    if (!std::holds_alternative<t::ErrorCode>(code) || !value.reason().details().empty()) {
      return std::nullopt;
    }
    out.error = std::get<t::ErrorCode>(code);
  }
  return out;
}
bool failure_wire(const t::Failure& value, const t::ModelStreamEvent& event, v1::RpcFailure* out) {
  auto error = codec::make_error(value.code, event.request_id, event.trace_id,
                                 value.recovery == t::Recovery::Backoff ||
                                     value.recovery == t::Recovery::QuerySameIdentity);
  if (!std::holds_alternative<v1::Error>(error)) {
    return false;
  }
  *out->mutable_error() = std::get<v1::Error>(error);
  auto* context = out->mutable_context();
  switch (value.acceptance) {
  case t::Acceptance::NotAccepted:
    context->set_acceptance(v1::NOT_ACCEPTED);
    break;
  case t::Acceptance::Accepted:
    context->set_acceptance(v1::DURABLY_ACCEPTED);
    break;
  case t::Acceptance::Unknown:
    context->set_acceptance(v1::ACCEPTANCE_UNKNOWN);
    break;
  }
  switch (value.recovery) {
  case t::Recovery::CorrectInput:
    context->set_recovery(v1::CORRECT_INPUT);
    break;
  case t::Recovery::Authenticate:
    context->set_recovery(v1::REAUTHENTICATE);
    break;
  case t::Recovery::ReadCurrent:
    context->set_recovery(v1::RESYNC_SNAPSHOT);
    break;
  case t::Recovery::QuerySameIdentity:
    context->set_recovery(v1::QUERY_SAME_INTENT);
    break;
  case t::Recovery::Backoff:
    context->set_recovery(v1::RETRY_SAME_ID);
    break;
  case t::Recovery::Diagnose:
    context->set_recovery(v1::DO_NOT_RETRY);
    break;
  }
  if (value.retry_after) {
    if (value.retry_after->count() < 0 || value.retry_after->count() > 60000) {
      return false;
    }
    context->set_retry_after_ms(static_cast<std::uint32_t>(value.retry_after->count()));
  }
  if (value.operation) {
    const auto& operation = *value.operation;
    const auto owner = operation.owner == t::Service::Runtime     ? "runtime"
                       : operation.owner == t::Service::Scheduler ? "scheduler"
                       : operation.owner == t::Service::Model     ? "model"
                                                                  : "";
    if (std::string_view{owner}.empty()) {
      return false;
    }
    context->mutable_operation()->set_owner_service(owner);
    context->mutable_operation()->set_operation_id(operation.id.value);
    auto resource = codec::to_wire(operation.resource);
    if (!std::holds_alternative<v1::ResourceRef>(resource)) {
      return false;
    }
    *context->mutable_operation()->mutable_resource() = std::get<v1::ResourceRef>(resource);
  }
  return std::holds_alternative<grpc::Status>(codec::to_grpc_failure(*out));
}
std::optional<t::Failure> failure_value(const v1::RpcFailure& value,
                                        const v1::ModelStreamEvent& event) {
  if (!value.has_error() || value.error().request_id() != event.request_id() ||
      value.error().trace_id() != event.trace_id() ||
      !std::holds_alternative<grpc::Status>(codec::to_grpc_failure(value))) {
    return std::nullopt;
  }
  auto code = codec::from_wire(value.error().code());
  if (!std::holds_alternative<t::ErrorCode>(code)) {
    return std::nullopt;
  }
  t::Failure out{
      std::get<t::ErrorCode>(code), t::Acceptance::Unknown, t::Recovery::QuerySameIdentity, {}, {}};
  switch (value.context().acceptance()) {
  case v1::NOT_ACCEPTED:
    out.acceptance = t::Acceptance::NotAccepted;
    break;
  case v1::DURABLY_ACCEPTED:
    out.acceptance = t::Acceptance::Accepted;
    break;
  case v1::ACCEPTANCE_UNKNOWN:
    out.acceptance = t::Acceptance::Unknown;
    break;
  default:
    return std::nullopt;
  }
  switch (value.context().recovery()) {
  case v1::CORRECT_INPUT:
    out.recovery = t::Recovery::CorrectInput;
    break;
  case v1::REAUTHENTICATE:
    out.recovery = t::Recovery::Authenticate;
    break;
  case v1::RESYNC_SNAPSHOT:
    out.recovery = t::Recovery::ReadCurrent;
    break;
  case v1::QUERY_SAME_INTENT:
    out.recovery = t::Recovery::QuerySameIdentity;
    break;
  case v1::RETRY_SAME_ID:
    out.recovery = t::Recovery::Backoff;
    break;
  case v1::DO_NOT_RETRY:
    out.recovery = t::Recovery::Diagnose;
    break;
  default:
    return std::nullopt;
  }
  if (value.context().retry_after_ms() != 0) {
    out.retry_after = std::chrono::milliseconds{value.context().retry_after_ms()};
  }
  if (value.context().has_operation()) {
    const auto& operation = value.context().operation();
    const auto owner = operation.owner_service() == "runtime"     ? t::Service::Runtime
                       : operation.owner_service() == "scheduler" ? t::Service::Scheduler
                                                                  : t::Service::Model;
    auto resource = codec::from_wire(operation.resource());
    if (!std::holds_alternative<t::ResourceRef>(resource)) {
      return std::nullopt;
    }
    out.operation =
        t::OperationRef{owner, {operation.operation_id()}, std::get<t::ResourceRef>(resource)};
  }
  return out;
}
} // namespace

t::Outcome<v1::ModelStreamEvent>
encode_model_stream_event(const t::ModelStreamEvent& event) noexcept {
  try {
    if (!t::valid_model_identifier(event.request_id) ||
        !t::valid_model_identifier(event.logical_call_id) ||
        !t::valid_model_identifier(event.trace_id)) {
      return invalid();
    }
    v1::ModelStreamEvent out;
    out.set_request_id(event.request_id);
    out.set_logical_call_id(event.logical_call_id);
    out.set_trace_id(event.trace_id);
    out.set_sequence(event.sequence);
    if (const auto* started = std::get_if<t::ModelStreamStarted>(&event.body)) {
      switch (started->disposition) {
      case t::ModelStreamDisposition::Fresh:
        out.mutable_started()->set_disposition(v1::MODEL_STREAM_FRESH);
        break;
      case t::ModelStreamDisposition::ReplayInProgress:
        out.mutable_started()->set_disposition(v1::MODEL_STREAM_REPLAY_IN_PROGRESS);
        break;
      case t::ModelStreamDisposition::ReplayTerminal:
        out.mutable_started()->set_disposition(v1::MODEL_STREAM_REPLAY_TERMINAL);
        break;
      }
      if (!call_wire(started->call, event, out.mutable_started()->mutable_call())) {
        return invalid();
      }
    } else if (const auto* delta = std::get_if<t::ModelStreamDelta>(&event.body)) {
      if (!t::valid_model_stream_text(delta->text)) {
        return invalid();
      }
      out.mutable_delta()->set_text(delta->text);
    } else {
      const auto& terminal = std::get<t::ModelStreamTerminal>(event.body);
      auto* wire = out.mutable_terminal();
      switch (terminal.termination) {
      case t::ModelStreamTermination::Completed:
        wire->set_termination(v1::MODEL_STREAM_COMPLETED);
        break;
      case t::ModelStreamTermination::Truncated:
        wire->set_termination(v1::MODEL_STREAM_TRUNCATED);
        break;
      case t::ModelStreamTermination::Failed:
        wire->set_termination(v1::MODEL_STREAM_FAILED);
        break;
      case t::ModelStreamTermination::Cancelled:
        wire->set_termination(v1::MODEL_STREAM_CANCELLED);
        break;
      case t::ModelStreamTermination::Replay:
        wire->set_termination(v1::MODEL_STREAM_REPLAY);
        break;
      }
      switch (terminal.finish_reason) {
      case t::ModelStreamFinishReason::Unspecified:
        wire->set_finish_reason(v1::MODEL_STREAM_FINISH_REASON_UNSPECIFIED);
        break;
      case t::ModelStreamFinishReason::Stop:
        wire->set_finish_reason(v1::MODEL_STREAM_FINISH_STOP);
        break;
      case t::ModelStreamFinishReason::Length:
        wire->set_finish_reason(v1::MODEL_STREAM_FINISH_LENGTH);
        break;
      case t::ModelStreamFinishReason::ContentFilter:
        wire->set_finish_reason(v1::MODEL_STREAM_FINISH_CONTENT_FILTER);
        break;
      }
      wire->set_partial(terminal.partial);
      wire->set_emitted_bytes(terminal.emitted_bytes);
      wire->set_emitted_delta_count(terminal.emitted_delta_count);
      usage_wire(terminal.usage, wire->mutable_usage());
      wire->set_may_have_billed(terminal.may_have_billed);
      if (terminal.call && !call_wire(*terminal.call, event, wire->mutable_call())) {
        return invalid();
      }
      if (terminal.failure && !failure_wire(*terminal.failure, event, wire->mutable_failure())) {
        return invalid();
      }
    }
    return out;
  } catch (...) {
    return invalid();
  }
}

t::Outcome<v1::RpcFailure> encode_model_stream_failure(const t::Failure& failure,
                                                       std::string_view request_id,
                                                       std::string_view trace_id) noexcept {
  try {
    v1::RpcFailure result;
    const t::ModelStreamEvent event{
        std::string{request_id}, {}, 0, std::string{trace_id}, t::ModelStreamTerminal{}};
    if (!failure_wire(failure, event, &result)) {
      return invalid();
    }
    return result;
  } catch (...) {
    return invalid();
  }
}

std::optional<t::Failure> decode_model_stream_failure(const v1::RpcFailure& failure,
                                                      std::string_view request_id,
                                                      std::string_view trace_id) noexcept {
  try {
    v1::ModelStreamEvent event;
    event.set_request_id(request_id.data(), request_id.size());
    event.set_trace_id(trace_id.data(), trace_id.size());
    return failure_value(failure, event);
  } catch (...) {
    return std::nullopt;
  }
}

ModelStreamDecoder::ModelStreamDecoder(std::string request_id, std::string logical_call_id,
                                       std::string trace_id)
    : request_id_(std::move(request_id)), logical_call_id_(std::move(logical_call_id)),
      trace_id_(std::move(trace_id)) {}

t::Outcome<t::ModelStreamEvent>
ModelStreamDecoder::decode(const v1::ModelStreamEvent& event) noexcept {
  const auto reject = [&]() -> t::Outcome<t::ModelStreamEvent> {
    invalid_ = true;
    return invalid();
  };
  try {
    if (invalid_ || terminal_ || !t::valid_model_identifier(request_id_) ||
        !t::valid_model_identifier(logical_call_id_) || !t::valid_model_identifier(trace_id_) ||
        event.request_id() != request_id_ || event.logical_call_id() != logical_call_id_ ||
        event.trace_id() != trace_id_ || event.sequence() != next_sequence_) {
      return reject();
    }
    t::ModelStreamEvent out{request_id_, logical_call_id_, event.sequence(), trace_id_,
                            t::ModelStreamTerminal{}};
    if (event.has_started()) {
      const auto& value = event.started();
      if (started_ || next_sequence_ != 0 || !value.has_call()) {
        return reject();
      }
      auto call = call_value(value.call(), logical_call_id_);
      if (!call) {
        return reject();
      }
      t::ModelStreamDisposition disposition;
      switch (value.disposition()) {
      case v1::MODEL_STREAM_FRESH:
        disposition = t::ModelStreamDisposition::Fresh;
        break;
      case v1::MODEL_STREAM_REPLAY_IN_PROGRESS:
        disposition = t::ModelStreamDisposition::ReplayInProgress;
        break;
      case v1::MODEL_STREAM_REPLAY_TERMINAL:
        disposition = t::ModelStreamDisposition::ReplayTerminal;
        break;
      default:
        return reject();
      }
      if ((disposition == t::ModelStreamDisposition::ReplayTerminal) ==
          (call->state == t::ModelCallState::Started)) {
        return reject();
      }
      out.body = t::ModelStreamStarted{disposition, *call};
      started_ = value;
    } else if (event.has_delta()) {
      if (!started_ || started_->disposition() != v1::MODEL_STREAM_FRESH ||
          !t::valid_model_stream_text(event.delta().text()) ||
          bytes_ + event.delta().text().size() > t::kModelMaximumResultBytes) {
        return reject();
      }
      out.body = t::ModelStreamDelta{event.delta().text()};
      bytes_ += event.delta().text().size();
      ++deltas_;
    } else if (event.has_terminal()) {
      const auto& value = event.terminal();
      if (!value.has_usage() || !usage_valid(value.usage()) || !value.has_may_have_billed() ||
          value.emitted_bytes() != bytes_ || value.emitted_delta_count() != deltas_ ||
          value.partial() != (bytes_ != 0 && value.termination() != v1::MODEL_STREAM_COMPLETED)) {
        return reject();
      }
      t::ModelStreamTerminal terminal;
      switch (value.termination()) {
      case v1::MODEL_STREAM_COMPLETED:
        terminal.termination = t::ModelStreamTermination::Completed;
        break;
      case v1::MODEL_STREAM_TRUNCATED:
        terminal.termination = t::ModelStreamTermination::Truncated;
        break;
      case v1::MODEL_STREAM_FAILED:
        terminal.termination = t::ModelStreamTermination::Failed;
        break;
      case v1::MODEL_STREAM_CANCELLED:
        terminal.termination = t::ModelStreamTermination::Cancelled;
        break;
      case v1::MODEL_STREAM_REPLAY:
        terminal.termination = t::ModelStreamTermination::Replay;
        break;
      default:
        return reject();
      }
      switch (value.finish_reason()) {
      case v1::MODEL_STREAM_FINISH_REASON_UNSPECIFIED:
        terminal.finish_reason = t::ModelStreamFinishReason::Unspecified;
        break;
      case v1::MODEL_STREAM_FINISH_STOP:
        terminal.finish_reason = t::ModelStreamFinishReason::Stop;
        break;
      case v1::MODEL_STREAM_FINISH_LENGTH:
        terminal.finish_reason = t::ModelStreamFinishReason::Length;
        break;
      case v1::MODEL_STREAM_FINISH_CONTENT_FILTER:
        terminal.finish_reason = t::ModelStreamFinishReason::ContentFilter;
        break;
      default:
        return reject();
      }
      if ((!started_ && value.termination() != v1::MODEL_STREAM_FAILED &&
           value.termination() != v1::MODEL_STREAM_CANCELLED) ||
          (value.termination() == v1::MODEL_STREAM_FAILED && !value.has_failure()) ||
          ((value.termination() == v1::MODEL_STREAM_COMPLETED ||
            value.termination() == v1::MODEL_STREAM_TRUNCATED ||
            value.termination() == v1::MODEL_STREAM_REPLAY) &&
           value.has_failure())) {
        return reject();
      }
      if (value.has_failure()) {
        terminal.failure = failure_value(value.failure(), event);
        if (!terminal.failure) {
          return reject();
        }
      }
      if (value.has_call()) {
        terminal.call = call_value(value.call(), logical_call_id_);
        if (!terminal.call || !usage_equal(value.usage(), value.call().usage()) ||
            value.may_have_billed() != value.call().may_have_billed()) {
          return reject();
        }
        if (started_) {
          const auto& pinned = started_->call();
          const auto& call = value.call();
          if (call.owner_instance_id() != pinned.owner_instance_id() ||
              call.model_id() != pinned.model_id() ||
              call.config_version() != pinned.config_version() ||
              call.secret_version() != pinned.secret_version()) {
            return reject();
          }
        }
      }
      const bool replay = started_ && started_->disposition() != v1::MODEL_STREAM_FRESH;
      if (replay != (value.termination() == v1::MODEL_STREAM_REPLAY) ||
          (replay && (deltas_ != 0 || !value.has_call() ||
                      value.call().SerializeAsString() != started_->call().SerializeAsString()))) {
        return reject();
      }
      if (value.termination() == v1::MODEL_STREAM_COMPLETED ||
          value.termination() == v1::MODEL_STREAM_TRUNCATED) {
        if (!terminal.call || terminal.call->state != t::ModelCallState::Succeeded ||
            (value.termination() == v1::MODEL_STREAM_COMPLETED &&
             value.finish_reason() != v1::MODEL_STREAM_FINISH_STOP) ||
            (value.termination() == v1::MODEL_STREAM_TRUNCATED &&
             value.finish_reason() != v1::MODEL_STREAM_FINISH_LENGTH &&
             value.finish_reason() != v1::MODEL_STREAM_FINISH_CONTENT_FILTER)) {
          return reject();
        }
      }
      terminal.partial = value.partial();
      terminal.emitted_bytes = bytes_;
      terminal.emitted_delta_count = deltas_;
      terminal.usage = {value.usage().known(), value.usage().input_tokens(),
                        value.usage().output_tokens()};
      terminal.may_have_billed = value.may_have_billed();
      out.body = std::move(terminal);
      terminal_ = true;
    } else {
      return reject();
    }
    ++next_sequence_;
    return out;
  } catch (...) {
    return reject();
  }
}
} // namespace kiarabot::core::platform
