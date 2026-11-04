#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"

namespace {
namespace t = kiarabot::core::target;
namespace p = kiarabot::core::platform;
namespace v = kiarabot::v1;
constexpr char kRequest[] = "11111111-1111-4111-8111-111111111111";
constexpr char kLogical[] = "22222222-2222-4222-8222-222222222222";
constexpr char kTrace[] = "33333333-3333-4333-8333-333333333333";
void require(bool value) {
  if (!value) {
    throw std::runtime_error("stream codec assertion");
  }
}
v::ModelStreamEvent encode(t::ModelStreamEvent event) {
  auto result = p::encode_model_stream_event(event);
  require(std::holds_alternative<v::ModelStreamEvent>(result));
  return std::get<v::ModelStreamEvent>(std::move(result));
}
t::ModelIntent call() {
  t::ModelIntent out;
  out.owner = {"44444444-4444-4444-8444-444444444444"};
  out.logical_call_id = kLogical;
  out.model = {"55555555-5555-4555-8555-555555555555"};
  out.config_version = {1};
  out.secret_version = {1};
  out.state = t::ModelCallState::Started;
  out.may_have_billed = true;
  out.observed_at = t::TimePoint{std::chrono::seconds{1800000000}};
  return out;
}
std::vector<v::ModelStreamEvent> fresh() {
  auto running = call();
  auto success = running;
  success.state = t::ModelCallState::Succeeded;
  success.usage_known = true;
  success.input_tokens = 2;
  success.output_tokens = 3;
  t::ModelStreamTerminal terminal;
  terminal.termination = t::ModelStreamTermination::Completed;
  terminal.finish_reason = t::ModelStreamFinishReason::Stop;
  terminal.emitted_bytes = 6;
  terminal.emitted_delta_count = 1;
  terminal.call = success;
  terminal.usage = {true, 2, 3};
  return {encode({kRequest, kLogical, 0, kTrace,
                  t::ModelStreamStarted{t::ModelStreamDisposition::Fresh, running}}),
          encode({kRequest, kLogical, 1, kTrace, t::ModelStreamDelta{"世界"}}),
          encode({kRequest, kLogical, 2, kTrace, terminal})};
}
bool accepted(const std::vector<v::ModelStreamEvent>& events, bool terminal = true) {
  p::ModelStreamDecoder decoder{kRequest, kLogical, kTrace};
  for (const auto& event : events) {
    if (!std::holds_alternative<t::ModelStreamEvent>(decoder.decode(event))) {
      return false;
    }
  }
  return decoder.terminal_received() == terminal;
}
std::optional<t::ModelStreamTerminal>
decoded_terminal(const std::vector<v::ModelStreamEvent>& events) {
  p::ModelStreamDecoder decoder{kRequest, kLogical, kTrace};
  std::optional<t::ModelStreamTerminal> result;
  for (const auto& event : events) {
    auto decoded = decoder.decode(event);
    if (!std::holds_alternative<t::ModelStreamEvent>(decoded)) {
      return std::nullopt;
    }
    const auto& value = std::get<t::ModelStreamEvent>(decoded);
    if (const auto* terminal = std::get_if<t::ModelStreamTerminal>(&value.body)) {
      result = *terminal;
    }
  }
  if (!decoder.terminal_received()) {
    return std::nullopt;
  }
  return result;
}
// Sizes drive one ordered attempt: started=0, deltas=1..N, terminal=N+1.
std::vector<v::ModelStreamEvent> sized_stream(const std::vector<std::size_t>& sizes) {
  auto events = fresh();
  auto terminal = events.back();
  events.resize(1); // keep only the started frame; terminal is rebuilt from the given sizes
  std::uint64_t bytes = 0;
  for (std::size_t index = 0; index < sizes.size(); ++index) {
    events.push_back(encode({kRequest, kLogical, 1 + index, kTrace,
                             t::ModelStreamDelta{std::string(sizes[index], 'a')}}));
    bytes += sizes[index];
  }
  terminal.set_sequence(sizes.size() + 1);
  terminal.mutable_terminal()->set_emitted_bytes(bytes);
  terminal.mutable_terminal()->set_emitted_delta_count(sizes.size());
  events.push_back(terminal);
  return events;
}
std::vector<v::ModelStreamEvent> replay_stream() {
  auto events = fresh();
  events.erase(events.begin() + 1);
  *events.front().mutable_started()->mutable_call() = events.back().terminal().call();
  events.front().mutable_started()->set_disposition(v::MODEL_STREAM_REPLAY_TERMINAL);
  auto* terminal = events.back().mutable_terminal();
  events.back().set_sequence(1);
  terminal->set_termination(v::MODEL_STREAM_REPLAY);
  terminal->set_emitted_bytes(0);
  terminal->set_emitted_delta_count(0);
  return events;
}
v::ModelStreamEvent wire_delta(std::uint64_t sequence, std::string text) {
  v::ModelStreamEvent event;
  event.set_request_id(kRequest);
  event.set_logical_call_id(kLogical);
  event.set_trace_id(kTrace);
  event.set_sequence(sequence);
  event.mutable_delta()->set_text(std::move(text));
  return event;
}
bool accepts_delta(std::string text) {
  std::vector<v::ModelStreamEvent> events{fresh()[0], wire_delta(1, std::move(text))};
  return accepted(events, false);
}
void cases() {
  require(accepted(fresh()));
  auto truncated = fresh();
  truncated.back().mutable_terminal()->set_termination(v::MODEL_STREAM_TRUNCATED);
  truncated.back().mutable_terminal()->set_finish_reason(v::MODEL_STREAM_FINISH_LENGTH);
  truncated.back().mutable_terminal()->set_partial(true);
  require(accepted(truncated));
  auto replay = replay_stream();
  require(accepted(replay));
  const auto failure = t::Failure{t::ErrorCode::PermissionDenied,
                                  t::Acceptance::NotAccepted,
                                  t::Recovery::Authenticate,
                                  {},
                                  {}};
  t::ModelStreamTerminal denied;
  denied.failure = failure;
  denied.may_have_billed = false;
  denied.usage.known = true;
  require(accepted({encode({kRequest, kLogical, 0, kTrace, denied})}));
  auto prefix = fresh();
  prefix.pop_back();
  require(accepted(prefix, false));
  const std::vector<std::function<void(std::vector<v::ModelStreamEvent>&)>> mutate{
      [](auto& x) { x[1].set_request_id(kTrace); },
      [](auto& x) { x[1].set_trace_id(kLogical); },
      [](auto& x) { x[1].set_logical_call_id(kRequest); },
      [](auto& x) { x[1].set_sequence(2); },
      [](auto& x) {
        x[0].mutable_started()->set_disposition(v::MODEL_STREAM_DISPOSITION_UNSPECIFIED);
      },
      [](auto& x) { x[0].mutable_started()->mutable_call()->clear_may_have_billed(); },
      [](auto& x) { x[0].mutable_started()->mutable_call()->set_may_have_billed(false); },
      [](auto& x) { x[0].mutable_started()->mutable_call()->clear_usage(); },
      [](auto& x) { x[0].mutable_started()->mutable_call()->mutable_usage()->set_input_tokens(1); },
      [](auto& x) { x[1].mutable_delta()->set_text(""); },
      [](auto& x) { x[1].mutable_delta()->set_text(std::string(4097, 'x')); },
      [](auto& x) { x[1].mutable_delta()->set_text(std::string("\xc0\xaf", 2)); },
      [](auto& x) { x.back().mutable_terminal()->clear_usage(); },
      [](auto& x) { x.back().mutable_terminal()->clear_may_have_billed(); },
      [](auto& x) { x.back().mutable_terminal()->set_emitted_bytes(5); },
      [](auto& x) { x.back().mutable_terminal()->set_emitted_delta_count(2); },
      [](auto& x) { x.back().mutable_terminal()->set_partial(true); },
      [](auto& x) { x.back().mutable_terminal()->mutable_call()->set_secret_version(2); },
      [](auto& x) {
        x.back().mutable_terminal()->set_finish_reason(v::MODEL_STREAM_FINISH_LENGTH);
      },
      [](auto& x) { x.back().mutable_terminal()->set_termination(v::MODEL_STREAM_FAILED); },
      [](auto& x) { x.back().mutable_terminal()->set_may_have_billed(false); },
      [](auto& x) { x.push_back(x.back()); },
      [](auto& x) { x.erase(x.begin()); },
      [](auto& x) { x[0].clear_started(); }};
  for (const auto& mutation : mutate) {
    auto invalid = fresh();
    mutation(invalid);
    require(!accepted(invalid));
  }
  auto correlated = fresh();
  auto* failed = correlated.back().mutable_terminal();
  failed->set_termination(v::MODEL_STREAM_FAILED);
  failed->set_partial(true);
  auto encoded_failure = p::encode_model_stream_failure(failure, kRequest, kTrace);
  require(std::holds_alternative<v::RpcFailure>(encoded_failure));
  *failed->mutable_failure() = std::get<v::RpcFailure>(encoded_failure);
  require(accepted(correlated));
  failed->mutable_failure()->mutable_error()->set_trace_id(kLogical);
  require(!accepted(correlated));
  p::ModelStreamDecoder poisoned{kRequest, kLogical, kTrace};
  auto invalid = fresh()[0];
  invalid.set_sequence(3);
  require(std::holds_alternative<t::Failure>(poisoned.decode(invalid)));
  require(std::holds_alternative<t::Failure>(poisoned.decode(fresh()[0])));
}
t::Failure failure_denied() {
  return {t::ErrorCode::PermissionDenied,
          t::Acceptance::NotAccepted,
          t::Recovery::Authenticate,
          {},
          {}};
}

void boundaries() {
  // Exact 4 KiB delta and the 64 KiB cumulative text bound are inclusive; one byte more is not.
  require(accepted(sized_stream({4096})));
  auto maximum = std::vector<std::size_t>(16, 4096);
  require(accepted(sized_stream(maximum)));
  maximum.push_back(1);
  auto over = sized_stream(maximum);
  require(!accepted(over));
  // Strict UTF-8 at the delta boundary: lone/rejecting code units fail, 4-byte text is valid.
  require(accepts_delta("\xe4\xb8\x96\xe7\x95\x8c"));
  require(accepts_delta("\xf0\x9f\x98\x80"));
  require(!accepts_delta(std::string{"\x80", 1}));
  require(!accepts_delta(std::string{"\xc0\xaf", 2}));
  require(!accepts_delta(std::string{"\xed\xa0\x80", 3}));
  require(!accepts_delta(std::string{"\xe4\xb8", 2}));
  require(!accepts_delta(std::string{"\xf5\x80\x80\x80", 4}));
  require(!accepts_delta(std::string{"a\xf0\x9f", 3}));
  require(!accepts_delta(std::string(4097, 'a')));
  // started is exactly sequence 0 and the terminal follows the deltas; pre-start terminal is 0.
  auto late_started = fresh();
  late_started[0].set_sequence(1);
  require(!accepted(late_started));
  for (const auto termination :
       {v::MODEL_STREAM_COMPLETED, v::MODEL_STREAM_TRUNCATED, v::MODEL_STREAM_REPLAY}) {
    t::ModelStreamTerminal prestart;
    prestart.usage.known = true;
    prestart.may_have_billed = true;
    auto encoded = encode({kRequest, kLogical, 0, kTrace, prestart});
    encoded.mutable_terminal()->set_termination(termination);
    require(!accepted({encoded}));
  }
  t::ModelStreamTerminal cancelled;
  cancelled.termination = t::ModelStreamTermination::Cancelled;
  cancelled.usage.known = true;
  cancelled.may_have_billed = true;
  require(accepted({encode({kRequest, kLogical, 0, kTrace, cancelled})}));
  auto prestart_sequence = encode({kRequest, kLogical, 0, kTrace, cancelled});
  prestart_sequence.set_sequence(1);
  require(!accepted({prestart_sequence}));
  // FAILED requires a structured RpcFailure; COMPLETED/TRUNCATED/REPLAY must not carry one.
  auto failed_without_failure = fresh();
  failed_without_failure.back().mutable_terminal()->set_termination(v::MODEL_STREAM_FAILED);
  failed_without_failure.back().mutable_terminal()->set_partial(true);
  require(!accepted(failed_without_failure));
  // Replay admission never carries text and keeps the unchanged started snapshot.
  auto replay_with_delta = replay_stream();
  replay_with_delta.insert(replay_with_delta.begin() + 1, wire_delta(1, std::string{"x"}));
  replay_with_delta.back().set_sequence(2);
  require(!accepted(replay_with_delta));
  auto replay_running = replay_stream();
  auto* running_call = replay_running.front().mutable_started()->mutable_call();
  running_call->set_state(v::OPERATION_RUNNING);
  running_call->set_execution_outcome(v::EXECUTION_UNKNOWN);
  running_call->set_may_have_billed(true);
  require(!accepted(replay_running));
  // No event may follow a terminal; a trailing delta is not silently ignored.
  auto trailing = fresh();
  trailing.push_back(wire_delta(3, std::string{"x"}));
  require(!accepted(trailing));
  // Bounded RATE_LIMITED Retry-After survives the failure codec; zero means absent, >60s invalid.
  t::Failure rate{t::ErrorCode::RateLimited,
                  t::Acceptance::NotAccepted,
                  t::Recovery::Backoff,
                  {},
                  std::chrono::milliseconds{1000}};
  auto encoded_rate = p::encode_model_stream_failure(rate, kRequest, kTrace);
  require(std::holds_alternative<v::RpcFailure>(encoded_rate));
  require(std::get<v::RpcFailure>(encoded_rate).context().retry_after_ms() == 1000);
  auto correlated = fresh();
  auto* correlated_terminal = correlated.back().mutable_terminal();
  correlated_terminal->set_termination(v::MODEL_STREAM_FAILED);
  correlated_terminal->set_partial(true);
  *correlated_terminal->mutable_failure() = std::get<v::RpcFailure>(encoded_rate);
  auto decoded = decoded_terminal(correlated);
  require(decoded && decoded->failure && decoded->failure->retry_after &&
          decoded->failure->retry_after->count() == 1000);
  rate.retry_after = std::chrono::milliseconds{60001};
  require(
      std::holds_alternative<t::Failure>(p::encode_model_stream_failure(rate, kRequest, kTrace)));
  auto no_retry = fresh();
  auto* no_retry_terminal = no_retry.back().mutable_terminal();
  no_retry_terminal->set_termination(v::MODEL_STREAM_FAILED);
  no_retry_terminal->set_partial(true);
  *no_retry_terminal->mutable_failure() =
      std::get<v::RpcFailure>(p::encode_model_stream_failure(failure_denied(), kRequest, kTrace));
  auto decoded_no_retry = decoded_terminal(no_retry);
  require(decoded_no_retry && decoded_no_retry->failure && !decoded_no_retry->failure->retry_after);
}
} // namespace
int main() {
  try {
    cases();
    boundaries();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
