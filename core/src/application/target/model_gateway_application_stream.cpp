#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "kiarabot/core/application/target/model_gateway_application.hpp"

namespace kiarabot::core::target {
namespace {
using SteadyClock = std::chrono::steady_clock;

TimePoint stream_wall_now() {
  return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
Failure stream_failure(ErrorCode code, Acceptance acceptance = Acceptance::Unknown) {
  return {code,
          acceptance,
          acceptance == Acceptance::Unknown ? Recovery::QuerySameIdentity : Recovery::Diagnose,
          {},
          {}};
}

// The Provider is a port, not a trusted memory bound. Both limits are enforced before copying.
class ProviderQueue final : public ModelProviderStreamSink {
public:
  explicit ProviderQueue(std::stop_token cancellation) : cancellation_(cancellation) {}
  ModelStreamSinkResult try_emit(const ModelStreamDelta& delta) override {
    if (cancellation_.stop_requested()) {
      return ModelStreamSinkResult::Cancelled;
    }
    std::lock_guard lock{mutex};
    if (invalid || !valid_model_stream_text(delta.text) ||
        total_bytes_ + delta.text.size() > kModelMaximumResultBytes) {
      invalid = true;
      changed.notify_one();
      return ModelStreamSinkResult::Cancelled;
    }
    if (pending.size() >= kModelStreamMaximumQueuedFrames ||
        queued_bytes + delta.text.size() > kModelStreamMaximumQueuedBytes) {
      return ModelStreamSinkResult::Full;
    }
    pending.push_back(delta);
    queued_bytes += delta.text.size();
    total_bytes_ += delta.text.size();
    changed.notify_one();
    return ModelStreamSinkResult::Accepted;
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<ModelStreamDelta> pending;
  std::size_t queued_bytes = 0;
  bool invalid = false;
  // Completion has its own slot and cannot be starved by a full delta queue.
  std::optional<ModelProviderStreamResult> result;

private:
  std::stop_token cancellation_;
  std::size_t total_bytes_ = 0;
};
} // namespace

ModelStreamTerminal ModelGatewayApplication::chat_stream(const ModelChatCommand& command,
                                                         ModelStreamSink& sink,
                                                         ModelStreamCancellation cancellation) {
  ModelStreamTerminal terminal;
  terminal.may_have_billed = false;
  terminal.usage.known = true;
  std::uint64_t sequence = 0;
  std::optional<Failure> authorization_failure;
  std::function<std::optional<Failure>()> refresh_output;
  auto next_authorization = SteadyClock::now();
  std::stop_source stop;
  std::stop_callback external_cancel{cancellation, [&stop] { stop.request_stop(); }};
  // Text deadline is fixed once, never renewed by queueing or authorization checks.
  auto output_deadline = SteadyClock::now() + std::chrono::seconds{30};
  const auto remaining = command.context.deadline - stream_wall_now();
  output_deadline = std::min(output_deadline, SteadyClock::now() + remaining);
  const auto emit = [&](const auto& body, bool final = false) -> bool {
    ModelStreamEvent event{command.context.request_id, command.logical_call_id, sequence,
                           command.context.trace_id, body};
    const auto enqueue_deadline =
        std::min(SteadyClock::now() + kModelStreamEnqueueBudget,
                 final ? output_deadline + std::chrono::seconds{5} : output_deadline);
    do {
      if (!final && (stop.stop_requested() || SteadyClock::now() >= output_deadline)) {
        return false;
      }
      if (!final && refresh_output && SteadyClock::now() >= next_authorization) {
        authorization_failure = refresh_output();
        if (authorization_failure) {
          stop.request_stop();
          return false;
        }
      }
      try {
        switch (sink.try_emit(event)) {
        case ModelStreamSinkResult::Accepted:
          ++sequence;
          return true;
        case ModelStreamSinkResult::Cancelled:
          stop.request_stop();
          return false;
        case ModelStreamSinkResult::Full:
          break;
        }
      } catch (...) {
        stop.request_stop();
        return false;
      }
      if (SteadyClock::now() >= enqueue_deadline) {
        stop.request_stop();
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (true);
  };
  const auto finish = [&]() {
    terminal.partial =
        terminal.emitted_bytes != 0 && terminal.termination != ModelStreamTermination::Completed;
    (void)emit(terminal, true);
    return terminal;
  };
  if (stop.stop_requested()) {
    terminal.termination = ModelStreamTermination::Cancelled;
    terminal.failure = stream_failure(ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted);
    return finish();
  }
  try {
    auto admitted = admit_chat(command);
    if (const auto* rejected = std::get_if<Failure>(&admitted)) {
      terminal.failure = *rejected;
      terminal.may_have_billed = rejected->acceptance == Acceptance::Unknown;
      terminal.usage.known = !terminal.may_have_billed;
      return finish();
    }
    auto admission = std::move(std::get<ChatAdmission>(admitted));
    terminal.call = admission.call;
    terminal.may_have_billed = admission.call.may_have_billed;
    terminal.usage = {admission.call.usage_known, admission.call.input_tokens,
                      admission.call.output_tokens};
    const auto authorize_output = [&]() -> std::optional<Failure> {
      auto decision = authorize(command.caller, Permission::ModelChat,
                                {ResourceKind::Model, admission.call.model}, command.context);
      next_authorization = SteadyClock::now() + std::chrono::seconds{1};
      if (const auto* rejected = std::get_if<Failure>(&decision)) {
        return *rejected;
      }
      return std::nullopt;
    };
    refresh_output = authorize_output;
    if (admission.disposition != ModelChatDisposition::FreshResult) {
      terminal.termination = ModelStreamTermination::Replay;
      if (auto rejected = authorize_output()) {
        terminal.termination = ModelStreamTermination::Failed;
        terminal.failure = *rejected;
        return finish();
      }
      const auto disposition = admission.disposition == ModelChatDisposition::ReplayInProgress
                                   ? ModelStreamDisposition::ReplayInProgress
                                   : ModelStreamDisposition::ReplayTerminal;
      if (!emit(ModelStreamStarted{disposition, admission.call})) {
        terminal.termination = ModelStreamTermination::Cancelled;
      }
      // A replay contains no secret load, Provider work, text or invocation state mutation.
      if (auto rejected = authorize_output()) {
        terminal.termination = ModelStreamTermination::Failed;
        terminal.failure = *rejected;
      }
      return finish();
    }

    output_deadline =
        std::min(output_deadline,
                 SteadyClock::now() + std::chrono::milliseconds{admission.config.timeout_ms});
    ModelProviderStreamResult provider;
    provider.outcome = ModelProviderOutcome::DefiniteFailure;
    provider.may_have_billed = false;
    provider.usage.known = true;
    std::optional<Failure> delivery_failure;
    bool delivery_cancelled = false;
    bool provider_contract_invalid = false;
    if (auto rejected = authorize_output()) {
      delivery_failure = *rejected;
      provider.failure = *rejected;
    } else if (stop.stop_requested()) {
      provider.termination = ModelStreamTermination::Cancelled;
      provider.failure = stream_failure(ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted);
    } else if (!dependencies_.provider.supports_streaming()) {
      provider.failure = Failure{ErrorCode::CapabilityNotSupported,
                                 Acceptance::NotAccepted,
                                 Recovery::CorrectInput,
                                 {},
                                 {}};
    } else if (!emit(ModelStreamStarted{ModelStreamDisposition::Fresh, admission.call})) {
      provider.termination = ModelStreamTermination::Cancelled;
      provider.failure = stream_failure(ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted);
    } else {
      auto plaintext = dependencies_.crypto.open(
          admission.secret.sealed_secret,
          model_secret_aad(admission.secret.model_id, admission.secret.secret_version,
                           admission.secret.root_key_version),
          admission.secret.nonce);
      if (std::holds_alternative<Failure>(plaintext)) {
        provider.failure = stream_failure(ErrorCode::UpstreamUnavailable, Acceptance::NotAccepted);
      } else {
        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(
            output_deadline - SteadyClock::now());
        if (budget < std::chrono::milliseconds{kModelMinimumTimeoutMs}) {
          provider.failure = stream_failure(ErrorCode::Timeout, Acceptance::NotAccepted);
        } else {
          const auto* secret = std::get<OwnedSecret>(plaintext).get();
          ModelProviderRequest request{admission.config.endpoint, admission.config.provider_model,
                                       static_cast<std::uint32_t>(budget.count()), command.messages,
                                       secret ? secret->bytes() : SecretView{}};
          ProviderQueue queue{stop.get_token()};
          // The secret, request, queue and stop source all outlive this joined worker.
          std::jthread worker{[&] {
            ModelProviderStreamResult result;
            try {
              result = dependencies_.provider.chat_stream(request, queue, stop.get_token());
            } catch (...) {
              result.failure = stream_failure(ErrorCode::Internal);
              result.provider_dispatched = true;
            }
            {
              std::lock_guard lock{queue.mutex};
              queue.result.emplace(std::move(result));
            }
            queue.changed.notify_one();
          }};
          while (true) {
            if (!delivery_failure && SteadyClock::now() >= output_deadline) {
              delivery_failure = stream_failure(ErrorCode::Timeout);
              stop.request_stop();
            }
            if (!delivery_failure && stop.stop_requested()) {
              delivery_failure = stream_failure(ErrorCode::UpstreamUnavailable);
              delivery_cancelled = true;
            }
            if (!delivery_failure && SteadyClock::now() >= next_authorization) {
              delivery_failure = authorize_output();
              next_authorization = SteadyClock::now() + std::chrono::seconds{1};
              if (delivery_failure) {
                stop.request_stop();
              }
            }
            std::optional<ModelStreamDelta> delta;
            bool done = false;
            {
              std::unique_lock lock{queue.mutex};
              provider_contract_invalid = provider_contract_invalid || queue.invalid;
              if (queue.invalid && !delivery_failure) {
                delivery_failure = stream_failure(ErrorCode::UpstreamUnavailable);
                stop.request_stop();
              }
              if (!queue.pending.empty()) {
                delta = std::move(queue.pending.front());
                queue.queued_bytes -= delta->text.size();
                queue.pending.pop_front();
              } else if (queue.result) {
                provider = std::move(*queue.result);
                done = true;
              } else {
                queue.changed.wait_for(lock, std::chrono::milliseconds{10});
              }
            }
            if (done) {
              break;
            }
            if (delta && !delivery_failure) {
              if (emit(*delta)) {
                terminal.emitted_bytes += delta->text.size();
                ++terminal.emitted_delta_count;
              } else {
                delivery_failure = authorization_failure.value_or(stream_failure(
                    SteadyClock::now() >= output_deadline ? ErrorCode::Timeout
                                                          : ErrorCode::UpstreamUnavailable));
                delivery_cancelled = !authorization_failure && SteadyClock::now() < output_deadline;
                stop.request_stop();
              }
            }
          }
          worker.join();
        }
      }
    }

    if (provider_contract_invalid) {
      provider.outcome = ModelProviderOutcome::Unknown;
      provider.failure = stream_failure(ErrorCode::UpstreamUnavailable);
      provider.may_have_billed = true;
    }
    const bool valid_finish = (provider.termination == ModelStreamTermination::Completed &&
                               provider.finish_reason == ModelStreamFinishReason::Stop) ||
                              (provider.termination == ModelStreamTermination::Truncated &&
                               (provider.finish_reason == ModelStreamFinishReason::Length ||
                                provider.finish_reason == ModelStreamFinishReason::ContentFilter));
    ModelCallMutation mutation;
    if (provider.outcome == ModelProviderOutcome::Succeeded && valid_finish &&
        valid_provider_request_id(provider.provider_request_id)) {
      mutation = model_success_mutation(ModelProviderReply{
          {}, provider.provider_request_id, provider.latency_ms, provider.usage});
    } else if (provider.outcome == ModelProviderOutcome::DefiniteFailure &&
               !provider.may_have_billed) {
      mutation = model_definite_failure_mutation(provider.failure ? provider.failure->code
                                                                  : ErrorCode::UpstreamUnavailable);
      if (provider.failure) {
        mutation.retry_after = provider.failure->retry_after;
      }
    } else {
      mutation = model_unknown_mutation(provider.failure ? provider.failure->code
                                                         : ErrorCode::UpstreamUnavailable);
    }
    auto settled = settle_chat(admission, mutation);
    if (const auto* rejected = std::get_if<Failure>(&settled)) {
      terminal.call.reset();
      terminal.failure = *rejected;
      terminal.usage = {};
      terminal.may_have_billed = true;
      terminal.termination = ModelStreamTermination::Failed;
    } else {
      terminal.call = std::get<ModelIntent>(settled);
      terminal.usage = {terminal.call->usage_known, terminal.call->input_tokens,
                        terminal.call->output_tokens};
      terminal.may_have_billed = terminal.call->may_have_billed;
      terminal.finish_reason = provider.finish_reason;
      terminal.termination = provider.termination;
      if (mutation.state != ModelCallState::Succeeded) {
        terminal.termination = provider.termination == ModelStreamTermination::Cancelled
                                   ? ModelStreamTermination::Cancelled
                                   : ModelStreamTermination::Failed;
        terminal.failure =
            provider.failure.value_or(stream_failure(ErrorCode::UpstreamUnavailable));
        terminal.failure->acceptance =
            mutation.state == ModelCallState::Unknown ? Acceptance::Unknown : Acceptance::Accepted;
        terminal.failure->recovery = mutation.state == ModelCallState::Unknown
                                         ? Recovery::QuerySameIdentity
                                         : terminal.failure->recovery;
      }
      // Commit facts survive a later loss of authorization or delivery. Never rewrite success.
      if (auto rejected = authorize_output()) {
        delivery_failure = *rejected;
      }
      if (delivery_failure) {
        terminal.termination =
            delivery_cancelled ? ModelStreamTermination::Cancelled : ModelStreamTermination::Failed;
        terminal.failure = *delivery_failure;
        terminal.failure->acceptance =
            mutation.state == ModelCallState::Unknown ? Acceptance::Unknown : Acceptance::Accepted;
        if (mutation.state == ModelCallState::Unknown) {
          terminal.failure->recovery = Recovery::QuerySameIdentity;
        }
      }
    }
    return finish();
  } catch (...) {
    stop.request_stop();
    terminal.termination = ModelStreamTermination::Failed;
    const bool committed_known =
        terminal.call && (terminal.call->state == ModelCallState::Succeeded ||
                          terminal.call->state == ModelCallState::Failed);
    terminal.failure =
        stream_failure(ErrorCode::Internal, committed_known            ? Acceptance::Accepted
                                            : terminal.may_have_billed ? Acceptance::Unknown
                                                                       : Acceptance::NotAccepted);
    if (!terminal.call && terminal.may_have_billed) {
      terminal.usage = {};
    }
    return finish();
  }
}
} // namespace kiarabot::core::target
