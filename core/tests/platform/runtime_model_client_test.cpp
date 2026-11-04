// P11c Runtime -> Model typed client tests against a loopback fake ModelService.
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"
#include "kiarabot/core/runtime/platform/runtime_model_client.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/model.grpc.pb.h"
#include "kiarabot/v1/model_stream.pb.h"

namespace {
namespace p = kiarabot::core::platform;
namespace codec = kiarabot::core::platform::target_codec;
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error("P11c runtime model assertion: " + std::string{message});
  }
}
t::TimePoint later(std::chrono::seconds seconds) {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch()) +
                      seconds};
}
class FakeModel final : public kiarabot::v1::ModelService::Service {
public:
  grpc::Status Chat(grpc::ServerContext*, const kiarabot::v1::ModelGatewayChatRequest* request,
                    kiarabot::v1::ModelGatewayChatResponse* response) override {
    std::lock_guard lock(mutex_);
    calls_++;
    logical_call_id_ = request->logical_call_id();
    delegation_ = request->context().delegation();
    model_id_ = request->model_id();
    if (status_ != grpc::StatusCode::OK) {
      return grpc::Status(status_, "");
    }
    response->set_disposition(disposition_);
    if (with_result_) {
      auto* result = response->mutable_result();
      result->set_text("reply");
      result->set_input_tokens(2);
      result->set_output_tokens(3);
    }
    return grpc::Status::OK;
  }
  grpc::Status ChatStream(grpc::ServerContext*,
                          const kiarabot::v1::ModelGatewayChatRequest* request,
                          grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>* writer) override {
    std::lock_guard lock(mutex_);
    calls_++;
    logical_call_id_ = request->logical_call_id();
    delegation_ = request->context().delegation();
    idempotency_key_ = request->context().idempotency_key();
    if (stream) {
      return stream(*writer);
    }
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "");
  }
  int calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }
  void set(kiarabot::v1::ModelChatDisposition disposition, bool with_result,
           grpc::StatusCode status = grpc::StatusCode::OK) {
    std::lock_guard lock(mutex_);
    disposition_ = disposition;
    with_result_ = with_result;
    status_ = status;
  }

private:
  mutable std::mutex mutex_;
  int calls_ = 0;
  kiarabot::v1::ModelChatDisposition disposition_ = kiarabot::v1::FRESH_RESULT;
  bool with_result_ = true;
  grpc::StatusCode status_ = grpc::StatusCode::OK;
  std::string logical_call_id_;
  std::string delegation_;
  std::string idempotency_key_;
  std::string model_id_;

public:
  std::function<grpc::Status(grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>&)> stream;
  std::string last_idempotency_key() const {
    std::lock_guard lock(mutex_);
    return idempotency_key_;
  }
};
struct Server {
  explicit Server(FakeModel& fake) {
    builder_.RegisterService(&fake);
    builder_.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    server_ = builder_.BuildAndStart();
    require(server_ != nullptr && port_ > 0, "fake model start");
  }
  ~Server() {
    server_->Shutdown(std::chrono::system_clock::now() + 2s);
    server_->Wait();
  }
  std::string endpoint() const {
    return "127.0.0.1:" + std::to_string(port_);
  }
  grpc::ServerBuilder builder_;
  std::unique_ptr<grpc::Server> server_;
  int port_ = 0;
};
t::RuntimeModelChatRequest request() {
  t::RuntimeModelChatRequest value;
  value.instance = t::InstanceId{"22222222-2222-4222-8222-222222222222"};
  value.request_id = "request-1";
  value.logical_call_id = "44444444-4444-4444-8444-444444444444";
  value.model = "default";
  value.messages = {{"user", "hello"}};
  return value;
}
std::unique_ptr<p::RuntimeModelClient> client_for(const std::string& endpoint,
                                                  bool delegation_available = true) {
  auto created = p::RuntimeModelClient::create(
      {endpoint, grpc::InsecureChannelCredentials(),
       delegation_available ? p::ModelDelegationProvider{[](const t::RuntimeModelChatRequest&)
                                                             -> t::Outcome<std::string> {
         return std::string{"delegation-proof"};
       }}
                            : p::unavailable_model_delegation(),
       30000ms});
  require(std::holds_alternative<std::unique_ptr<p::RuntimeModelClient>>(created), "client create");
  return std::move(std::get<std::unique_ptr<p::RuntimeModelClient>>(created));
}

void success_and_replay() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  auto reply = client->chat(request(), later(5s));
  require(std::holds_alternative<t::RuntimeModelChatReply>(reply), "fresh success");
  require(std::get<t::RuntimeModelChatReply>(reply).text == "reply" &&
              std::get<t::RuntimeModelChatReply>(reply).input_tokens == 2 &&
              std::get<t::RuntimeModelChatReply>(reply).output_tokens == 3,
          "fresh result");
  require(fake.calls() == 1, "one model call");
  fake.set(kiarabot::v1::REPLAY_IN_PROGRESS, false);
  auto replay = client->chat(request(), later(5s));
  require(std::holds_alternative<t::Failure>(replay), "replay pending failure");
  const auto& failure = std::get<t::Failure>(replay);
  require(failure.code == t::ErrorCode::UpstreamUnavailable &&
              failure.recovery == t::Recovery::QuerySameIdentity &&
              failure.acceptance == t::Acceptance::Unknown,
          "replay pending maps to QUERY_SAME_INTENT, never a fabricated result");
  fake.set(kiarabot::v1::REPLAY_TERMINAL, true);
  auto terminal = client->chat(request(), later(5s));
  require(std::holds_alternative<t::RuntimeModelChatReply>(terminal), "terminal replay result");
}

void failure_mapping() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  fake.set(kiarabot::v1::FRESH_RESULT, false, grpc::StatusCode::UNAVAILABLE);
  auto unavailable = client->chat(request(), later(5s));
  require(std::holds_alternative<t::Failure>(unavailable) &&
              std::get<t::Failure>(unavailable).code == t::ErrorCode::UpstreamUnavailable,
          "transport unavailable");
  fake.set(kiarabot::v1::FRESH_RESULT, false, grpc::StatusCode::UNAUTHENTICATED);
  auto unauthenticated = client->chat(request(), later(5s));
  require(std::holds_alternative<t::Failure>(unauthenticated) &&
              std::get<t::Failure>(unauthenticated).code == t::ErrorCode::Unauthenticated,
          "unauthenticated");
  fake.set(kiarabot::v1::FRESH_RESULT, false, grpc::StatusCode::PERMISSION_DENIED);
  auto denied = client->chat(request(), later(5s));
  require(std::holds_alternative<t::Failure>(denied) &&
              std::get<t::Failure>(denied).code == t::ErrorCode::PermissionDenied,
          "permission denied");
  // No delegation proof means no call at all.
  auto without = client_for(server.endpoint(), false);
  const auto calls = fake.calls();
  auto missing = without->chat(request(), later(5s));
  require(std::holds_alternative<t::Failure>(missing) &&
              std::get<t::Failure>(missing).code == t::ErrorCode::Unauthenticated &&
              fake.calls() == calls,
          "delegation provider fails closed before RPC");
  // Expired budget is unknown, never a safe retry.
  auto expired = client->chat(request(), later(-1s));
  require(std::holds_alternative<t::Failure>(expired) &&
              std::get<t::Failure>(expired).code == t::ErrorCode::Timeout &&
              std::get<t::Failure>(expired).acceptance == t::Acceptance::Unknown,
          "expired budget unknown");
}

constexpr char kStreamRequest[] = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr char kStreamLogical[] = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
constexpr char kStreamTrace[] = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
t::ModelIntent stream_call() {
  t::ModelIntent value;
  value.owner = {t::InstanceId{"22222222-2222-4222-8222-222222222222"}.value};
  value.logical_call_id = kStreamLogical;
  value.model = {"11111111-1111-4111-8111-111111111111"};
  value.config_version = {1};
  value.secret_version = {1};
  value.state = t::ModelCallState::Started;
  value.may_have_billed = true;
  value.observed_at = t::TimePoint{std::chrono::seconds{1800000000}};
  return value;
}
kiarabot::v1::ModelStreamEvent encoded(const t::ModelStreamEvent& event) {
  auto value = p::encode_model_stream_event(event);
  require(std::holds_alternative<kiarabot::v1::ModelStreamEvent>(value), "encode stream event");
  return std::get<kiarabot::v1::ModelStreamEvent>(value);
}
std::vector<kiarabot::v1::ModelStreamEvent> completed_frames(std::string text = "reply") {
  auto running = stream_call();
  auto success = running;
  success.state = t::ModelCallState::Succeeded;
  success.usage_known = true;
  success.input_tokens = 2;
  success.output_tokens = 3;
  t::ModelStreamTerminal terminal;
  terminal.termination = t::ModelStreamTermination::Completed;
  terminal.finish_reason = t::ModelStreamFinishReason::Stop;
  terminal.emitted_bytes = text.size();
  terminal.emitted_delta_count = 1;
  terminal.call = success;
  terminal.usage = {true, 2, 3};
  return {encoded({kStreamRequest, kStreamLogical, 0, kStreamTrace,
                   t::ModelStreamStarted{t::ModelStreamDisposition::Fresh, running}}),
          encoded({kStreamRequest, kStreamLogical, 1, kStreamTrace, t::ModelStreamDelta{text}}),
          encoded({kStreamRequest, kStreamLogical, 2, kStreamTrace, terminal})};
}
struct CollectingSink final : t::RuntimeModelStreamSink {
  std::vector<t::ModelStreamEvent> events;
  t::ModelStreamSinkResult next = t::ModelStreamSinkResult::Accepted;
  t::ModelStreamSinkResult try_emit(const t::ModelStreamEvent& event) override {
    if (next == t::ModelStreamSinkResult::Accepted) {
      events.push_back(event);
    }
    return next;
  }
};
t::RuntimeModelStreamRequest stream_request() {
  t::RuntimeModelStreamRequest value;
  value.instance = t::InstanceId{"22222222-2222-4222-8222-222222222222"};
  value.request_id = kStreamRequest;
  value.trace_id = kStreamTrace;
  value.logical_call_id = kStreamLogical;
  value.model = "default";
  value.messages = {{"user", "hello"}};
  return value;
}
void stream_success() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  fake.stream = [](grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>& writer) {
    for (const auto& frame : completed_frames()) {
      if (!writer.Write(frame)) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "");
      }
    }
    return grpc::Status::OK;
  };
  CollectingSink sink;
  std::stop_source cancellation;
  auto result = client->chat_stream(stream_request(), sink, cancellation.get_token(), later(5s));
  require(std::holds_alternative<t::RuntimeModelStreamResult>(result), "stream success");
  const auto& value = std::get<t::RuntimeModelStreamResult>(result);
  require(value.terminal && value.terminal->termination == t::ModelStreamTermination::Completed &&
              sink.events.size() == 3 && value.received_bytes == 5 &&
              value.received_delta_count == 1 && !value.cancelled_by_consumer,
          "ordered terminal and local counters");
  require(fake.last_idempotency_key().empty(), "stream attempt carries no unary idempotency key");
}
void stream_pre_start_rejection() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  fake.stream = [](grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>&) {
    const t::Failure failure_value{t::ErrorCode::PermissionDenied,
                                   t::Acceptance::NotAccepted,
                                   t::Recovery::Authenticate,
                                   {},
                                   {}};
    auto rpc = p::encode_model_stream_failure(failure_value, kStreamRequest, kStreamTrace);
    auto status = codec::to_grpc_failure(std::get<kiarabot::v1::RpcFailure>(rpc));
    return std::get<grpc::Status>(status);
  };
  CollectingSink sink;
  std::stop_source cancellation;
  auto result = client->chat_stream(stream_request(), sink, cancellation.get_token(), later(5s));
  require(std::holds_alternative<t::Failure>(result), "pre-start rejection is a failure");
  const auto& failure_value = std::get<t::Failure>(result);
  require(failure_value.code == t::ErrorCode::PermissionDenied &&
              failure_value.acceptance == t::Acceptance::NotAccepted &&
              failure_value.recovery == t::Recovery::Authenticate && sink.events.empty(),
          "structured trailer preserves acceptance/recovery");
}
void stream_lost_terminal_is_unknown() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  fake.stream = [](grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>& writer) {
    const auto frames = completed_frames();
    (void)writer.Write(frames[0]);
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "");
  };
  CollectingSink sink;
  std::stop_source cancellation;
  auto result = client->chat_stream(stream_request(), sink, cancellation.get_token(), later(5s));
  require(std::holds_alternative<t::Failure>(result), "missing terminal is not completion");
  const auto& failure_value = std::get<t::Failure>(result);
  require(failure_value.acceptance == t::Acceptance::Unknown &&
              failure_value.recovery == t::Recovery::QuerySameIdentity &&
              failure_value.code == t::ErrorCode::UpstreamUnavailable && !sink.events.empty(),
          "unknown delivery with conservative recovery");
}
void stream_malformed_prefix_is_unknown() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  fake.stream = [](grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>& writer) {
    auto frames = completed_frames();
    frames[1].set_sequence(3);
    (void)writer.Write(frames[0]);
    (void)writer.Write(frames[1]);
    return grpc::Status::OK;
  };
  CollectingSink sink;
  std::stop_source cancellation;
  auto result = client->chat_stream(stream_request(), sink, cancellation.get_token(), later(5s));
  require(std::holds_alternative<t::Failure>(result) &&
              std::get<t::Failure>(result).acceptance == t::Acceptance::Unknown,
          "invalid sequence never fabricates success");
}
void stream_consumer_cancellation() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  std::atomic<bool> release{false};
  fake.stream = [&release](grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>& writer) {
    const auto frames = completed_frames();
    (void)writer.Write(frames[0]);
    while (!release.load()) {
      std::this_thread::sleep_for(5ms);
    }
    return grpc::Status(grpc::StatusCode::OK, "");
  };
  CollectingSink sink;
  std::stop_source cancellation;
  std::thread stopper([&cancellation] {
    while (!cancellation.stop_requested()) {
      std::this_thread::sleep_for(20ms);
      cancellation.request_stop();
    }
  });
  auto result = client->chat_stream(stream_request(), sink, cancellation.get_token(), later(5s));
  release.store(true);
  stopper.join();
  require(std::holds_alternative<t::RuntimeModelStreamResult>(result), "cancel is a delivery stop");
  const auto& value = std::get<t::RuntimeModelStreamResult>(result);
  require(value.cancelled_by_consumer && !value.terminal, "consumer cancel yields no terminal");
}
void stream_sink_cancelled() {
  FakeModel fake;
  Server server(fake);
  auto client = client_for(server.endpoint());
  fake.stream = [](grpc::ServerWriter<kiarabot::v1::ModelStreamEvent>& writer) {
    for (const auto& frame : completed_frames()) {
      if (!writer.Write(frame)) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "");
      }
    }
    return grpc::Status::OK;
  };
  CollectingSink sink;
  sink.next = t::ModelStreamSinkResult::Cancelled;
  std::stop_source cancellation;
  auto result = client->chat_stream(stream_request(), sink, cancellation.get_token(), later(5s));
  require(std::holds_alternative<t::RuntimeModelStreamResult>(result) &&
              std::get<t::RuntimeModelStreamResult>(result).cancelled_by_consumer &&
              sink.events.empty(),
          "slow/cancelled sink stops the attempt without a terminal");
}

} // namespace

int main() {
  try {
    success_and_replay();
    failure_mapping();
    stream_success();
    stream_pre_start_rejection();
    stream_lost_terminal_is_unknown();
    stream_malformed_prefix_is_unknown();
    stream_consumer_cancellation();
    stream_sink_cancelled();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "runtime model client: ok\n";
  return 0;
}
