#include <condition_variable>
#include <stop_token>

#include "model_application_test_support.hpp"

namespace model_application_test {

class RecordingSink final : public t::ModelStreamSink {
public:
  std::vector<t::ModelStreamEvent> events;
  std::function<t::ModelStreamSinkResult(const t::ModelStreamEvent&)> admit;
  t::ModelStreamSinkResult try_emit(const t::ModelStreamEvent& event) override {
    const auto result = admit ? admit(event) : t::ModelStreamSinkResult::Accepted;
    if (result == t::ModelStreamSinkResult::Accepted) {
      events.push_back(event);
    }
    return result;
  }
  void verify(const t::ModelStreamTerminal& terminal) const {
    require(!events.empty(), "writable sink receives terminal");
    std::uint64_t bytes = 0;
    std::uint64_t deltas = 0;
    for (std::size_t index = 0; index < events.size(); ++index) {
      require(events[index].sequence == index, "sequence is consecutive");
      require(events[index].request_id == "request-1" && events[index].logical_call_id == kCall &&
                  events[index].trace_id == "trace-1",
              "attempt correlation");
      if (const auto* delta = std::get_if<t::ModelStreamDelta>(&events[index].body)) {
        bytes += delta->text.size();
        ++deltas;
      }
    }
    require(std::holds_alternative<t::ModelStreamTerminal>(events.back().body), "last is terminal");
    require(terminal.emitted_bytes == bytes && terminal.emitted_delta_count == deltas,
            "terminal counts actual accepted Model emissions");
    require(terminal.partial ==
                (bytes != 0 && terminal.termination != t::ModelStreamTermination::Completed),
            "partial derives from this delivery");
  }
};

t::ModelChatCommand stream_command() {
  return {plugin(), context(), kCall, "default", {message()}};
}

t::ModelProviderStreamResult
success_result(t::ModelStreamFinishReason reason = t::ModelStreamFinishReason::Stop) {
  t::ModelProviderStreamResult result;
  result.outcome = t::ModelProviderOutcome::Succeeded;
  result.termination = reason == t::ModelStreamFinishReason::Stop
                           ? t::ModelStreamTermination::Completed
                           : t::ModelStreamTermination::Truncated;
  result.finish_reason = reason;
  result.provider_dispatched = true;
  result.provider_request_id = "req-stream";
  result.usage = {true, 10, 20};
  return result;
}

void basic_stream(Fixture& fixture,
                  t::ModelStreamFinishReason reason = t::ModelStreamFinishReason::Stop) {
  fixture.provider.stream = [reason](const auto&, auto& sink, auto) {
    require(sink.try_emit({"hello"}) == t::ModelStreamSinkResult::Accepted, "first queued delta");
    require(sink.try_emit({"世界"}) == t::ModelStreamSinkResult::Accepted, "UTF8 queued delta");
    return success_result(reason);
  };
}

void fresh_and_cross_mode_replay() {
  Fixture fixture;
  fixture.create_model();
  basic_stream(fixture);
  RecordingSink sink;
  auto result = fixture.application->chat_stream(stream_command(), sink);
  sink.verify(result);
  require(result.termination == t::ModelStreamTermination::Completed && !result.partial &&
              result.call && result.call->state == t::ModelCallState::Succeeded &&
              result.call->version.value == 2 && result.usage.known && result.may_have_billed,
          "fresh stream commits authoritative success");
  require(sink.events.size() == 4 && fixture.provider.calls == 1, "real incremental event shape");
  require(std::get<t::ModelStreamStarted>(sink.events.front().body).call.may_have_billed,
          "STARTED makes no false unbilled promise");
  auto replay = fixture.take(fixture.chat(context()));
  require(replay.disposition == t::ModelChatDisposition::ReplayTerminal && !replay.result &&
              fixture.provider.calls == 1,
          "stream to unary replay never reinvokes Provider");
  RecordingSink replay_sink;
  result = fixture.application->chat_stream(stream_command(), replay_sink);
  replay_sink.verify(result);
  require(result.termination == t::ModelStreamTermination::Replay &&
              replay_sink.events.size() == 2 && fixture.provider.calls == 1,
          "stream replay carries only metadata");
  auto changed = stream_command();
  changed.messages = {message("different")};
  RecordingSink conflict_sink;
  const auto conflict = fixture.application->chat_stream(changed, conflict_sink);
  conflict_sink.verify(conflict);
  require(conflict.failure && conflict.failure->code == t::ErrorCode::Conflict &&
              conflict_sink.events.size() == 1 && fixture.provider.calls == 1,
          "cross-mode fingerprint conflict");

  Fixture unary;
  unary.create_model();
  unary.take(unary.chat(context()));
  RecordingSink unary_replay;
  const auto from_unary = unary.application->chat_stream(stream_command(), unary_replay);
  require(from_unary.termination == t::ModelStreamTermination::Replay && unary.provider.calls == 1,
          "unary to stream replay works with unsupported streaming Provider");
}

void replay_in_progress_and_shared_concurrency() {
  Fixture fixture;
  fixture.create_model();
  fixture.store.configs.at(kModel).max_concurrency = 1;
  basic_stream(fixture);
  RecordingSink sink;
  bool tested = false;
  sink.admit = [&](const auto& event) {
    if (!tested && std::holds_alternative<t::ModelStreamStarted>(event.body)) {
      tested = true;
      RecordingSink duplicate;
      const auto replay = fixture.application->chat_stream(stream_command(), duplicate);
      require(replay.termination == t::ModelStreamTermination::Replay &&
                  duplicate.events.size() == 2 &&
                  std::get<t::ModelStreamStarted>(duplicate.events.front().body).disposition ==
                      t::ModelStreamDisposition::ReplayInProgress,
              "same identity bypasses fresh slot");
      const auto blocked = fixture.chat(context(), kCall2);
      require(std::holds_alternative<t::Failure>(blocked) &&
                  std::get<t::Failure>(blocked).code == t::ErrorCode::RateLimited,
              "old Chat shares fresh Model concurrency with stream");
      require(fixture.store.calls.size() == 1, "rejected call creates no intent");
    }
    return t::ModelStreamSinkResult::Accepted;
  };
  const auto result = fixture.application->chat_stream(stream_command(), sink);
  require(result.termination == t::ModelStreamTermination::Completed && tested,
          "fresh stream finishes");
  fixture.take(fixture.chat(context(), kCall2));
  require(fixture.provider.calls == 2,
          "concurrency slot releases after worker joins and settlement");
}

void truncation_unknown_usage_and_explicit_rejection() {
  for (const auto reason :
       {t::ModelStreamFinishReason::Length, t::ModelStreamFinishReason::ContentFilter}) {
    Fixture fixture;
    fixture.create_model();
    basic_stream(fixture, reason);
    RecordingSink sink;
    const auto result = fixture.application->chat_stream(stream_command(), sink);
    sink.verify(result);
    require(result.termination == t::ModelStreamTermination::Truncated && result.partial &&
                !result.failure && result.call->state == t::ModelCallState::Succeeded,
            "truncation is known execution success and incomplete delivery");
  }
  Fixture usage;
  usage.create_model();
  usage.provider.stream = [](const auto&, auto& sink, auto) {
    (void)sink.try_emit({"text"});
    auto result = success_result();
    result.usage = {};
    return result;
  };
  RecordingSink usage_sink;
  const auto unknown_usage = usage.application->chat_stream(stream_command(), usage_sink);
  require(unknown_usage.call->state == t::ModelCallState::Succeeded && !unknown_usage.usage.known &&
              unknown_usage.usage.input_tokens == 0 && unknown_usage.may_have_billed,
          "missing usage remains unknown on known success");

  for (const auto code : {t::ErrorCode::InvalidArgument, t::ErrorCode::RateLimited}) {
    Fixture fixture;
    fixture.create_model();
    fixture.provider.stream = [code](const auto&, auto&, auto) {
      t::ModelProviderStreamResult result;
      result.outcome = t::ModelProviderOutcome::DefiniteFailure;
      result.may_have_billed = false;
      result.provider_dispatched = true;
      result.usage.known = true;
      result.failure =
          t::Failure{code, t::Acceptance::NotAccepted, t::Recovery::CorrectInput, {}, 2s};
      return result;
    };
    RecordingSink sink;
    const auto result = fixture.application->chat_stream(stream_command(), sink);
    sink.verify(result);
    require(result.call->state == t::ModelCallState::Failed && !result.may_have_billed &&
                result.usage.known && !result.partial &&
                result.failure->acceptance == t::Acceptance::Accepted,
            "explicit Provider rejection is durable and known unbilled");
    require(result.failure->retry_after == 2s, "bounded retry hint survives settlement");
  }
}

void cancellation_delivery_loss_and_txn2_failure() {
  Fixture before;
  before.create_model();
  basic_stream(before);
  std::stop_source cancelled;
  cancelled.request_stop();
  RecordingSink pre_sink;
  const auto pre =
      before.application->chat_stream(stream_command(), pre_sink, cancelled.get_token());
  pre_sink.verify(pre);
  require(pre.termination == t::ModelStreamTermination::Cancelled && !pre.may_have_billed &&
              before.provider.calls == 0 && before.store.calls.empty(),
          "pre-cancel creates no Provider intent");

  Fixture partial;
  partial.create_model();
  std::stop_source stop;
  partial.provider.stream = [](const auto&, auto& sink, auto cancellation) {
    (void)sink.try_emit({"partial"});
    while (!cancellation.stop_requested()) {
      std::this_thread::yield();
    }
    t::ModelProviderStreamResult result;
    result.provider_dispatched = true;
    result.termination = t::ModelStreamTermination::Cancelled;
    result.failure = t::Failure{t::ErrorCode::UpstreamUnavailable,
                                t::Acceptance::Unknown,
                                t::Recovery::QuerySameIdentity,
                                {},
                                {}};
    return result;
  };
  RecordingSink partial_sink;
  partial_sink.admit = [&](const auto& event) {
    if (std::holds_alternative<t::ModelStreamDelta>(event.body)) {
      stop.request_stop();
    }
    return t::ModelStreamSinkResult::Accepted;
  };
  const auto cancelled_partial =
      partial.application->chat_stream(stream_command(), partial_sink, stop.get_token());
  partial_sink.verify(cancelled_partial);
  require(cancelled_partial.partial &&
              cancelled_partial.call->state == t::ModelCallState::Unknown &&
              cancelled_partial.may_have_billed && !cancelled_partial.usage.known,
          "in-flight cancellation never promises nonexecution");

  Fixture delivery;
  delivery.create_model();
  basic_stream(delivery);
  RecordingSink closed;
  closed.admit = [](const auto& event) {
    return std::holds_alternative<t::ModelStreamDelta>(event.body)
               ? t::ModelStreamSinkResult::Cancelled
               : t::ModelStreamSinkResult::Accepted;
  };
  const auto lost = delivery.application->chat_stream(stream_command(), closed);
  closed.verify(lost);
  require(lost.termination == t::ModelStreamTermination::Cancelled && lost.call &&
              lost.call->state == t::ModelCallState::Succeeded && lost.may_have_billed,
          "known execution survives downstream loss");

  Fixture txn2;
  txn2.create_model();
  basic_stream(txn2);
  txn2.store.skip_commits = 1;
  txn2.store.fail_commits = 1;
  RecordingSink failed;
  const auto unknown = txn2.application->chat_stream(stream_command(), failed);
  failed.verify(unknown);
  require(unknown.termination == t::ModelStreamTermination::Failed && unknown.partial &&
              unknown.failure->acceptance == t::Acceptance::Unknown && unknown.may_have_billed &&
              !unknown.call &&
              txn2.store.calls.at({kOwner, kCall}).state == t::ModelCallState::Started,
          "txn2 failure leaves recoverable STARTED and never completes");
  RecordingSink retry;
  const auto replay = txn2.application->chat_stream(stream_command(), retry);
  require(replay.termination == t::ModelStreamTermination::Replay && txn2.provider.calls == 1,
          "uncertain settlement never reinvokes Provider");
}

void current_authority_and_late_worker_cas() {
  Fixture revoke;
  revoke.create_model();
  basic_stream(revoke);
  revoke.authorization.checks = 0;
  revoke.authorization.on_check = [&] {
    if (revoke.authorization.checks >= 3) {
      revoke.authorization.deny = true;
    }
  };
  RecordingSink sink;
  const auto result = revoke.application->chat_stream(stream_command(), sink);
  sink.verify(result);
  require(result.termination == t::ModelStreamTermination::Failed && result.partial &&
              result.failure->code == t::ErrorCode::PermissionDenied &&
              result.call->state == t::ModelCallState::Succeeded,
          "final authority revocation blocks completion without rewriting committed execution");

  Fixture stale;
  stale.create_model();
  basic_stream(stale);
  RecordingSink recovering;
  recovering.admit = [&](const auto& event) {
    if (std::holds_alternative<t::ModelStreamStarted>(event.body)) {
      stale.factory.sample = now() + 35s;
      require(std::holds_alternative<std::monostate>(stale.application->maintenance()),
              "recovery succeeds");
    }
    return t::ModelStreamSinkResult::Accepted;
  };
  const auto late = stale.application->chat_stream(stream_command(), recovering);
  require(late.termination == t::ModelStreamTermination::Failed &&
              late.failure->acceptance == t::Acceptance::Unknown &&
              stale.store.calls.at({kOwner, kCall}).state == t::ModelCallState::Unknown &&
              stale.store.calls.at({kOwner, kCall}).version.value == 2,
          "late Provider cannot overwrite recovered version");
}

void bounded_queue_and_slow_consumer() {
  Fixture fixture;
  fixture.create_model();
  std::atomic<bool> saw_full{false};
  fixture.provider.stream = [&](const auto&, auto& sink, auto stop) {
    for (int index = 0; index < 1000 && !stop.stop_requested();) {
      switch (sink.try_emit({"x"})) {
      case t::ModelStreamSinkResult::Accepted:
        ++index;
        break;
      case t::ModelStreamSinkResult::Full:
        saw_full = true;
        std::this_thread::yield();
        break;
      case t::ModelStreamSinkResult::Cancelled:
        return t::ModelProviderStreamResult{};
      }
    }
    return success_result();
  };
  RecordingSink sink;
  sink.admit = [&](const auto& event) {
    if (std::holds_alternative<t::ModelStreamDelta>(event.body) && !saw_full.load()) {
      return t::ModelStreamSinkResult::Full;
    }
    return t::ModelStreamSinkResult::Accepted;
  };
  const auto result = fixture.application->chat_stream(stream_command(), sink);
  sink.verify(result);
  require(saw_full && result.termination == t::ModelStreamTermination::Completed &&
              result.emitted_delta_count == 1000,
          "bounded queue returns Full without dropping accepted deltas");

  Fixture slow;
  slow.create_model();
  basic_stream(slow);
  RecordingSink slow_sink;
  slow_sink.admit = [](const auto& event) {
    return std::holds_alternative<t::ModelStreamDelta>(event.body)
               ? t::ModelStreamSinkResult::Full
               : t::ModelStreamSinkResult::Accepted;
  };
  const auto began = std::chrono::steady_clock::now();
  const auto blocked = slow.application->chat_stream(stream_command(), slow_sink);
  require(std::chrono::steady_clock::now() - began < 2s &&
              blocked.termination == t::ModelStreamTermination::Cancelled &&
              blocked.call->state == t::ModelCallState::Succeeded,
          "consumer Full is bounded and cannot rewrite Provider success");
}

void periodic_authorization_deadline_and_port_validation() {
  for (const bool identity_failure : {false, true}) {
    Fixture fixture;
    fixture.create_model();
    fixture.authorization.checks = 0;
    fixture.authorization.on_check = [&] {
      if (fixture.authorization.checks >= 3) {
        if (identity_failure) {
          throw std::runtime_error("Identity offline");
        }
        fixture.authorization.deny = true;
      }
    };
    fixture.provider.stream = [](const auto&, auto& sink, auto stop) {
      (void)sink.try_emit({"before-revocation"});
      while (!stop.stop_requested()) {
        std::this_thread::yield();
      }
      return t::ModelProviderStreamResult{};
    };
    RecordingSink sink;
    const auto began = std::chrono::steady_clock::now();
    const auto result = fixture.application->chat_stream(stream_command(), sink);
    sink.verify(result);
    require(std::chrono::steady_clock::now() - began < 2500ms && result.partial &&
                result.call->state == t::ModelCallState::Unknown && result.may_have_billed &&
                result.failure->code == (identity_failure ? t::ErrorCode::UpstreamUnavailable
                                                          : t::ErrorCode::PermissionDenied),
            "stalled stream rechecks current authority within one second and fails closed");
  }
  Fixture timeout;
  timeout.create_model();
  timeout.store.configs.at(kModel).timeout_ms = 1200;
  timeout.provider.stream = [](const auto&, auto& sink, auto stop) {
    (void)sink.try_emit({"before-deadline"});
    while (!stop.stop_requested()) {
      std::this_thread::yield();
    }
    return t::ModelProviderStreamResult{};
  };
  RecordingSink timed;
  const auto began = std::chrono::steady_clock::now();
  const auto expired = timeout.application->chat_stream(stream_command(), timed);
  require(std::chrono::steady_clock::now() - began < 2500ms && expired.failure &&
              expired.failure->code == t::ErrorCode::Timeout && expired.partial &&
              expired.call->state == t::ModelCallState::Unknown,
          "configured text deadline cancels stalled Provider");

  for (const auto& text :
       {std::string(4097, 'x'), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string{}}) {
    Fixture invalid;
    invalid.create_model();
    invalid.provider.stream = [text](const auto&, auto& sink, auto) {
      require(sink.try_emit({text}) == t::ModelStreamSinkResult::Cancelled,
              "invalid Provider text is rejected before copying");
      return t::ModelProviderStreamResult{};
    };
    RecordingSink sink;
    const auto result = invalid.application->chat_stream(stream_command(), sink);
    sink.verify(result);
    require(result.termination != t::ModelStreamTermination::Completed &&
                result.emitted_bytes == 0 && result.call->state == t::ModelCallState::Unknown &&
                result.may_have_billed,
            "malformed pure Provider cannot bypass bounds or claim known unbilled");
  }
}

void default_unsupported_and_admission_failures() {
  Fixture unsupported;
  unsupported.create_model();
  RecordingSink sink;
  const auto result = unsupported.application->chat_stream(stream_command(), sink);
  sink.verify(result);
  require(result.failure && result.failure->code == t::ErrorCode::CapabilityNotSupported &&
              !result.may_have_billed && result.usage.known &&
              result.call->state == t::ModelCallState::Failed && unsupported.provider.calls == 0 &&
              sink.events.size() == 1,
          "unsupported never falls back to unary or fakes deltas");

  Fixture denied;
  denied.create_model();
  basic_stream(denied);
  denied.authorization.deny = true;
  RecordingSink rejection;
  const auto failure = denied.application->chat_stream(stream_command(), rejection);
  rejection.verify(failure);
  require(failure.failure->code == t::ErrorCode::PermissionDenied && !failure.may_have_billed &&
              denied.provider.calls == 0 && denied.store.calls.empty(),
          "pre-admission denial is known unbilled");

  Fixture txn1;
  txn1.create_model();
  basic_stream(txn1);
  txn1.store.fail_commits = 1;
  RecordingSink rollback;
  const auto rolled_back = txn1.application->chat_stream(stream_command(), rollback);
  rollback.verify(rolled_back);
  require(txn1.store.calls.empty() && txn1.provider.calls == 0 && !rolled_back.may_have_billed &&
              rolled_back.failure->acceptance == t::Acceptance::NotAccepted,
          "txn1 rollback cannot execute Provider");

  Fixture oversized;
  oversized.create_model();
  basic_stream(oversized);
  for (const auto& messages :
       {std::vector<t::ModelMessage>{message(std::string(2 * t::kModelMaximumRequestBytes, 'x'))},
        std::vector<t::ModelMessage>{message(std::string(40000, 'x')),
                                     message(std::string(40000, 'y'))},
        std::vector<t::ModelMessage>{message(std::string(t::kModelMaximumRequestBytes, 'x'))}}) {
    auto command = stream_command();
    command.messages = messages;
    RecordingSink invalid_sink;
    const auto rejected = oversized.application->chat_stream(command, invalid_sink);
    invalid_sink.verify(rejected);
    require(rejected.failure && rejected.failure->code == t::ErrorCode::InvalidArgument &&
                !rejected.may_have_billed && oversized.provider.calls == 0 &&
                oversized.store.calls.empty(),
            "oversized cumulative text and canonical encoding refuse before Provider intent");
    const auto unary_rejected = oversized.application->chat(command);
    require(std::holds_alternative<t::Failure>(unary_rejected) &&
                std::get<t::Failure>(unary_rejected).code == t::ErrorCode::InvalidArgument &&
                oversized.provider.calls == 0,
            "old Chat shares bounded canonical-input preflight");
  }

  Fixture unary;
  unary.create_model();
  std::optional<t::ModelIntent> in_flight;
  // At Provider entry, verify txn1 already persisted the shared conservative STARTED record.
  unary.provider.on_chat = [&] {
    const auto found = unary.store.calls.find({kOwner, kCall});
    if (found != unary.store.calls.end() && found->second.state == t::ModelCallState::Started) {
      in_flight = found->second;
    }
  };
  unary.take(unary.chat(context()));
  require(in_flight && in_flight->may_have_billed && !in_flight->usage_known,
          "old Chat shares conservative in-flight billing projection");
}

void recovery_time_boundaries() {
  Fixture fixture;
  fixture.create_model();
  auto seed = [&](const char* id, t::ModelCallState state, t::TimePoint created) {
    t::ModelIntent call;
    call.owner = {kOwner};
    call.logical_call_id = id;
    call.model = {kModel};
    call.state = state;
    call.version = {1};
    call.created_at = created;
    call.observed_at = created;
    call.secret_version = {1};
    call.config_version = {1};
    call.may_have_billed = true;
    fixture.store.calls[{kOwner, id}] = call;
  };
  seed(kCall, t::ModelCallState::Started, now() - 34s);
  seed(kCall2, t::ModelCallState::Unknown, now() - 29 * 24h);
  require(std::holds_alternative<std::monostate>(fixture.application->maintenance()),
          "maintenance before boundaries");
  require(fixture.store.calls.at({kOwner, kCall}).state == t::ModelCallState::Started &&
              fixture.store.calls.at({kOwner, kCall2}).state == t::ModelCallState::Unknown,
          "34 seconds STARTED and 29 days UNKNOWN remain unresolved");
  fixture.factory.sample = now() + 1s;
  require(std::holds_alternative<std::monostate>(fixture.application->maintenance()),
          "35 second recovery");
  require(fixture.store.calls.at({kOwner, kCall}).state == t::ModelCallState::Unknown &&
              fixture.store.calls.at({kOwner, kCall}).version.value == 2 &&
              fixture.store.calls.at({kOwner, kCall}).may_have_billed &&
              fixture.store.calls.at({kOwner, kCall}).disposition ==
                  t::ModelCallDisposition::Unspecified,
          "35 second STARTED becomes UNKNOWN without immediate abandonment");
  fixture.factory.sample = now() + 24h;
  require(std::holds_alternative<std::monostate>(fixture.application->maintenance()),
          "30 day disposition");
  require(fixture.store.calls.at({kOwner, kCall2}).disposition ==
              t::ModelCallDisposition::Abandoned,
          "UNKNOWN reaches rule-based disposition exactly at 30 days");
}
} // namespace model_application_test

int main() {
  try {
    model_application_test::fresh_and_cross_mode_replay();
    model_application_test::replay_in_progress_and_shared_concurrency();
    model_application_test::truncation_unknown_usage_and_explicit_rejection();
    model_application_test::cancellation_delivery_loss_and_txn2_failure();
    model_application_test::current_authority_and_late_worker_cas();
    model_application_test::bounded_queue_and_slow_consumer();
    model_application_test::periodic_authorization_deadline_and_port_validation();
    model_application_test::default_unsupported_and_admission_failures();
    model_application_test::recovery_time_boundaries();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
