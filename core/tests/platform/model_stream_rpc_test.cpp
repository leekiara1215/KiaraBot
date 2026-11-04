#include <atomic>
#include <chrono>
#include <condition_variable>
#include <stop_token>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "../architecture/model_application_test_support.hpp"
#include "kiarabot/core/runtime/platform/model_rpc.hpp"
#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"

namespace model_application_test {
namespace p = kiarabot::core::platform;
namespace v = kiarabot::v1;
bool wait_flag(const std::atomic<bool>& flag, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!flag.load()) {
    if (std::chrono::steady_clock::now() >= end) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return true;
}
t::TimePoint real_now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
class StreamServer {
public:
  Fixture fixture;
  std::function<void(t::ModelCaller&)> configure_caller;
  StreamServer() {
    fixture.create_model();
    auto built = p::make_model_rpc(
        *fixture.application,
        [this](grpc::ServerContext&, const v::RequestContext&) -> t::Outcome<t::ModelCaller> {
          auto caller = plugin();
          caller.runtime_origin = true;
          caller.delegation_expires_at = real_now() + 10s;
          if (configure_caller) {
            configure_caller(caller);
          }
          return caller;
        });
    require(std::holds_alternative<std::unique_ptr<v::ModelService::Service>>(built), "RPC built");
    service_ = std::move(std::get<std::unique_ptr<v::ModelService::Service>>(built));
    grpc::ServerBuilder builder;
    builder.RegisterService(service_.get());
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    server_ = builder.BuildAndStart();
    require(server_ != nullptr, "server start");
    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_),
                                   grpc::InsecureChannelCredentials());
    stub = v::ModelService::NewStub(channel_);
  }
  ~StreamServer() {
    server_->Shutdown(std::chrono::system_clock::now() + 2s);
    server_->Wait();
  }
  std::unique_ptr<v::ModelService::Stub> stub;

private:
  int port_ = 0;
  std::unique_ptr<v::ModelService::Service> service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
};
v::ModelGatewayChatRequest request() {
  v::ModelGatewayChatRequest out;
  out.mutable_context()->set_request_id(kSubject);
  out.mutable_context()->set_trace_id(kOtherOwner);
  out.mutable_context()->set_delegation("proof");
  const auto deadline = real_now() + 8s;
  auto stamp = std::chrono::duration_cast<std::chrono::seconds>(deadline.time_since_epoch());
  out.mutable_context()->mutable_deadline()->set_seconds(stamp.count());
  out.set_logical_call_id(kCall);
  out.set_model_id("default");
  out.add_messages()->set_role("user");
  out.mutable_messages(0)->set_text("hello");
  return out;
}
t::ModelProviderStreamResult success() {
  t::ModelProviderStreamResult out;
  out.outcome = t::ModelProviderOutcome::Succeeded;
  out.termination = t::ModelStreamTermination::Completed;
  out.finish_reason = t::ModelStreamFinishReason::Stop;
  out.provider_dispatched = true;
  out.usage = {true, 1, 2};
  return out;
}
void streaming_rpc() {
  StreamServer server;
  std::mutex mutex;
  std::condition_variable changed;
  bool release = false;
  server.fixture.provider.stream = [&](const auto&, auto& sink, auto stop) {
    require(sink.try_emit({"increment"}) == t::ModelStreamSinkResult::Accepted, "queued delta");
    std::unique_lock lock{mutex};
    changed.wait_for(lock, 2s, [&] { return release || stop.stop_requested(); });
    return success();
  };
  auto input = request();
  grpc::ClientContext transport;
  transport.set_deadline(std::chrono::system_clock::now() + 5s);
  auto reader = server.stub->ChatStream(&transport, input);
  p::ModelStreamDecoder decoder{input.context().request_id(), input.logical_call_id(),
                                input.context().trace_id()};
  v::ModelStreamEvent frame;
  int frames = 0;
  while (reader->Read(&frame)) {
    require(std::holds_alternative<t::ModelStreamEvent>(decoder.decode(frame)),
            "strict wire prefix");
    ++frames;
    if (frame.has_delta()) {
      std::lock_guard lock{mutex};
      release = true;
      changed.notify_all();
    }
  }
  require(reader->Finish().ok() && decoder.terminal_received() && frames == 3 &&
              server.fixture.provider.calls == 1,
          "delta crosses RPC before Provider completion");
  grpc::ClientContext again;
  auto replay = server.stub->ChatStream(&again, input);
  p::ModelStreamDecoder replay_decoder{input.context().request_id(), input.logical_call_id(),
                                       input.context().trace_id()};
  frames = 0;
  v::ModelStreamEvent replay_started;
  v::ModelStreamEvent replay_terminal;
  while (replay->Read(&frame)) {
    require(std::holds_alternative<t::ModelStreamEvent>(replay_decoder.decode(frame)),
            "replay wire prefix");
    ++frames;
    require(!frame.has_delta(), "replay has no text");
    if (frame.has_started()) {
      replay_started = frame;
    }
    if (frame.has_terminal()) {
      replay_terminal = frame;
    }
  }
  require(replay->Finish().ok() && frames == 2 && server.fixture.provider.calls == 1 &&
              replay_decoder.terminal_received() &&
              replay_started.started().disposition() == v::MODEL_STREAM_REPLAY_TERMINAL &&
              replay_terminal.terminal().termination() == v::MODEL_STREAM_REPLAY &&
              !replay_terminal.terminal().has_failure(),
          "RPC replay is started(REPLAY)+terminal(REPLAY), zero delta, no Provider");
}
void rejection_and_cancel() {
  StreamServer server;
  std::atomic<bool> provider_returned{false};
  server.fixture.provider.stream = [&](const auto&, auto& sink, auto stop) {
    (void)sink.try_emit({"partial"});
    while (!stop.stop_requested()) {
      std::this_thread::yield();
    }
    provider_returned = true;
    return t::ModelProviderStreamResult{};
  };
  auto invalid = request();
  invalid.mutable_context()->set_idempotency_key("forbidden");
  grpc::ClientContext malformed;
  auto rejected = server.stub->ChatStream(&malformed, invalid);
  v::ModelStreamEvent frame;
  require(!rejected->Read(&frame) &&
              rejected->Finish().error_code() == grpc::StatusCode::INVALID_ARGUMENT &&
              server.fixture.provider.calls == 0,
          "strict stream request shape");
  server.configure_caller = [](auto& caller) {
    caller.runtime_origin = false;
    caller.owner.reset();
  };
  grpc::ClientContext control;
  auto denied = server.stub->ChatStream(&control, request());
  require(!denied->Read(&frame) &&
              denied->Finish().error_code() == grpc::StatusCode::PERMISSION_DENIED &&
              server.fixture.provider.calls == 0,
          "Control never starts plugin text");
  server.configure_caller = {};
  grpc::ClientContext cancellation;
  cancellation.set_deadline(std::chrono::system_clock::now() + 5s);
  auto reader = server.stub->ChatStream(&cancellation, request());
  while (reader->Read(&frame)) {
    if (frame.has_delta()) {
      cancellation.TryCancel();
      break;
    }
  }
  const auto cancelled_status = reader->Finish();
  require(!cancelled_status.ok(), "hard transport cancellation is not completion");
  // Server-side cleanup (stop, wake, join) can trail the client-visible cancellation status.
  require(wait_flag(provider_returned, 2s), "cancellation stops and joins the Provider worker");
}
void proof_expiry_budget() {
  StreamServer server;
  server.configure_caller = [](auto& caller) {
    caller.delegation_expires_at = real_now() + 1300ms;
  };
  std::atomic<std::uint32_t> budget{0};
  server.fixture.provider.stream = [&](const auto& request, auto& sink, auto stop) {
    budget = request.timeout_ms;
    (void)sink.try_emit({"before expiry"});
    while (!stop.stop_requested()) {
      std::this_thread::yield();
    }
    return t::ModelProviderStreamResult{};
  };
  grpc::ClientContext transport;
  auto reader = server.stub->ChatStream(&transport, request());
  v::ModelStreamEvent frame;
  bool completed = false;
  const auto began = std::chrono::steady_clock::now();
  while (reader->Read(&frame)) {
    if (frame.has_terminal()) {
      completed = frame.terminal().termination() == v::MODEL_STREAM_COMPLETED;
    }
  }
  (void)reader->Finish();
  require(!completed && budget > 0 && budget <= 1300 &&
              std::chrono::steady_clock::now() - began < 3s,
          "certificate-bound proof expiry clips Provider and text delivery budget");
}
void replay_authorization_boundary() {
  StreamServer server;
  server.fixture.provider.stream = [](const auto&, auto& sink, auto stop) {
    (void)stop;
    require(sink.try_emit({"increment"}) == t::ModelStreamSinkResult::Accepted, "queued delta");
    return success();
  };
  auto input = request();
  grpc::ClientContext baseline;
  baseline.set_deadline(std::chrono::system_clock::now() + 5s);
  auto first = server.stub->ChatStream(&baseline, input);
  p::ModelStreamDecoder first_decoder{input.context().request_id(), input.logical_call_id(),
                                      input.context().trace_id()};
  v::ModelStreamEvent frame;
  int frames = 0;
  while (first->Read(&frame)) {
    require(std::holds_alternative<t::ModelStreamEvent>(first_decoder.decode(frame)),
            "fresh prefix");
    ++frames;
  }
  require(first->Finish().ok() && first_decoder.terminal_received() && frames == 3,
          "fresh baseline persisted the call");
  const auto calls = server.fixture.provider.calls;
  const auto base = server.fixture.authorization.checks;
  // A replay attempt performs exactly two current-grant checks: the admit-time replay authorization
  // and the pre-event replay admission check. Revoking the grant at that boundary is a pre-start
  // rejection: the attempt may only report terminal(sequence=0, FAILED, failure) with no started
  // event, no delta and no Provider re-invocation. After a replay started frame the frozen oracle
  // only allows an unchanged REPLAY terminal, which this path never puts at risk.
  server.fixture.authorization.on_check = [&] {
    if (server.fixture.authorization.checks >= base + 2) {
      server.fixture.authorization.deny = true;
    }
  };
  grpc::ClientContext replay_transport;
  replay_transport.set_deadline(std::chrono::system_clock::now() + 5s);
  auto replay = server.stub->ChatStream(&replay_transport, input);
  p::ModelStreamDecoder replay_decoder{input.context().request_id(), input.logical_call_id(),
                                       input.context().trace_id()};
  std::optional<t::ModelStreamTerminal> terminal;
  bool saw_started = false;
  bool saw_delta = false;
  frames = 0;
  while (replay->Read(&frame)) {
    auto decoded = replay_decoder.decode(frame);
    require(std::holds_alternative<t::ModelStreamEvent>(decoded),
            "pre-start replay terminal is a valid wire prefix");
    ++frames;
    saw_started = saw_started || frame.has_started();
    saw_delta = saw_delta || frame.has_delta();
    const auto& event = std::get<t::ModelStreamEvent>(decoded);
    if (const auto* value = std::get_if<t::ModelStreamTerminal>(&event.body)) {
      terminal = *value;
    }
  }
  require(replay->Finish().ok() && frames == 1 && !saw_started && !saw_delta &&
              replay_decoder.terminal_received() && server.fixture.provider.calls == calls,
          "revoked replay admission rejects before any event without Provider work");
  require(terminal.has_value() && terminal->termination == t::ModelStreamTermination::Failed &&
              terminal->failure && terminal->failure->code == t::ErrorCode::PermissionDenied,
          "pre-start replay rejection keeps the structured failure");
  server.fixture.authorization.on_check = {};
  server.fixture.authorization.deny = false;
}
void cancellation_joins_transport_workers() {
  StreamServer server;
  std::atomic<bool> provider_returned{false};
  server.fixture.provider.stream = [&](const auto&, auto& sink, auto stop) {
    (void)sink.try_emit({"partial"});
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    provider_returned = true;
    return t::ModelProviderStreamResult{};
  };
  grpc::ClientContext transport;
  transport.set_deadline(std::chrono::system_clock::now() + 5s);
  auto reader = server.stub->ChatStream(&transport, request());
  v::ModelStreamEvent frame;
  while (reader->Read(&frame)) {
    if (frame.has_delta()) {
      transport.TryCancel();
      break;
    }
  }
  const auto status = reader->Finish();
  require(!status.ok() && wait_flag(provider_returned, 2s),
          "cancellation stops and joins transport workers");
}
void concurrent_streams_do_not_cross() {
  StreamServer server;
  auto base_calls = server.fixture.provider.calls;
  server.fixture.provider.stream = [](const auto& provider_request, auto& sink, auto stop) {
    (void)stop;
    require(!provider_request.messages.empty(), "provider request retained messages");
    require(sink.try_emit({provider_request.messages.front().text}) ==
                t::ModelStreamSinkResult::Accepted,
            "queued per-request delta");
    return success();
  };
  auto first_input = request();
  auto second_input = request();
  second_input.set_logical_call_id(kCall2);
  second_input.mutable_context()->set_request_id(kCall);
  second_input.mutable_context()->set_trace_id(kCall2);
  second_input.mutable_messages(0)->set_text("second");
  grpc::ClientContext first_transport;
  grpc::ClientContext second_transport;
  first_transport.set_deadline(std::chrono::system_clock::now() + 5s);
  second_transport.set_deadline(std::chrono::system_clock::now() + 5s);
  auto first = server.stub->ChatStream(&first_transport, first_input);
  auto second = server.stub->ChatStream(&second_transport, second_input);
  v::ModelStreamEvent frame;
  std::string first_text;
  std::string second_text;
  p::ModelStreamDecoder first_decoder{first_input.context().request_id(),
                                      first_input.logical_call_id(),
                                      first_input.context().trace_id()};
  p::ModelStreamDecoder second_decoder{second_input.context().request_id(),
                                       second_input.logical_call_id(),
                                       second_input.context().trace_id()};
  while (first->Read(&frame)) {
    require(std::holds_alternative<t::ModelStreamEvent>(first_decoder.decode(frame)),
            "first stream prefix");
    if (frame.has_delta()) {
      first_text += frame.delta().text();
    }
  }
  while (second->Read(&frame)) {
    require(std::holds_alternative<t::ModelStreamEvent>(second_decoder.decode(frame)),
            "second stream prefix");
    if (frame.has_delta()) {
      second_text += frame.delta().text();
    }
  }
  require(first->Finish().ok() && second->Finish().ok() && first_decoder.terminal_received() &&
              second_decoder.terminal_received() && first_text == "hello" &&
              second_text == "second" && server.fixture.provider.calls == base_calls + 2,
          "two concurrent attempts never cross frames");
}
} // namespace model_application_test
int main() {
  try {
    model_application_test::streaming_rpc();
    model_application_test::rejection_and_cancel();
    model_application_test::proof_expiry_budget();
    model_application_test::replay_authorization_boundary();
    model_application_test::cancellation_joins_transport_workers();
    model_application_test::concurrent_streams_do_not_cross();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
