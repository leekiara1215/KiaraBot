#include <condition_variable>
#include <stop_token>

#include <grpcpp/grpcpp.h>

#include "../architecture/model_application_test_support.hpp"
#include "kiarabot/core/runtime/platform/model_rpc.hpp"
#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"

namespace model_application_test {
namespace p = kiarabot::core::platform;
namespace v = kiarabot::v1;
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
  frames = 0;
  while (replay->Read(&frame)) {
    ++frames;
    require(!frame.has_delta(), "replay has no text");
  }
  require(replay->Finish().ok() && frames == 2 && server.fixture.provider.calls == 1,
          "RPC replay no Provider");
}
void rejection_and_cancel() {
  StreamServer server;
  server.fixture.provider.stream = [](const auto&, auto& sink, auto stop) {
    (void)sink.try_emit({"partial"});
    while (!stop.stop_requested()) {
      std::this_thread::yield();
    }
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
  require(!reader->Finish().ok(), "hard transport cancellation is not completion");
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
} // namespace model_application_test
int main() {
  try {
    model_application_test::streaming_rpc();
    model_application_test::rejection_and_cancel();
    model_application_test::proof_expiry_budget();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
