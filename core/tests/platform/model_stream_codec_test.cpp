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
void cases() {
  require(accepted(fresh()));
  auto truncated = fresh();
  truncated.back().mutable_terminal()->set_termination(v::MODEL_STREAM_TRUNCATED);
  truncated.back().mutable_terminal()->set_finish_reason(v::MODEL_STREAM_FINISH_LENGTH);
  truncated.back().mutable_terminal()->set_partial(true);
  require(accepted(truncated));
  auto replay = fresh();
  replay.erase(replay.begin() + 1);
  *replay.front().mutable_started()->mutable_call() = replay.back().terminal().call();
  replay.front().mutable_started()->set_disposition(v::MODEL_STREAM_REPLAY_TERMINAL);
  auto* terminal = replay.back().mutable_terminal();
  replay.back().set_sequence(1);
  terminal->set_termination(v::MODEL_STREAM_REPLAY);
  terminal->set_emitted_bytes(0);
  terminal->set_emitted_delta_count(0);
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
} // namespace
int main() {
  try {
    cases();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
