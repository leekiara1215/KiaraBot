// P11c Runtime -> Model typed client tests against a loopback fake ModelService.
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/runtime/platform/runtime_model_client.hpp"
#include "kiarabot/v1/model.grpc.pb.h"

namespace {
namespace p = kiarabot::core::platform;
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
  std::string model_id_;
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
       delegation_available
           ? std::function<t::Outcome<std::string>()>{[]() -> t::Outcome<std::string> {
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

} // namespace

int main() {
  try {
    success_and_replay();
    failure_mapping();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "runtime model client: ok\n";
  return 0;
}
