#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/ports/target/runtime_model.hpp"
#include "kiarabot/v1/model.grpc.pb.h"

namespace kiarabot::core::platform {

struct RuntimeModelClientOptions {
  std::string endpoint;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  // Runtime's current delegation proof for audience=model. P05 delegation acquisition is not yet
  // wired; the production composition installs a fail-closed provider until it is.
  std::function<target::Outcome<std::string>()> delegation_provider;
  std::chrono::milliseconds maximum_budget{30000};
};

// Outbound Runtime -> Model typed client. No automatic retry: a call may have been billed, so the
// only continuation for an unknown outcome is a query with the same plugin logical_call_id.
class RuntimeModelClient final : public target::RuntimeModelPort {
public:
  static target::Outcome<std::unique_ptr<RuntimeModelClient>>
  create(RuntimeModelClientOptions options);
  ~RuntimeModelClient() override;
  RuntimeModelClient(const RuntimeModelClient&) = delete;
  RuntimeModelClient& operator=(const RuntimeModelClient&) = delete;

  bool available() const noexcept override;
  target::Outcome<target::RuntimeModelChatReply>
  chat(const target::RuntimeModelChatRequest& request, target::TimePoint deadline) override;

private:
  explicit RuntimeModelClient(RuntimeModelClientOptions options);
  RuntimeModelClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::ModelService::Stub> stub_;
};

// Fail-closed provider used until P05 delegation acquisition exists: no proof means no call.
std::function<target::Outcome<std::string>()> unavailable_model_delegation();

} // namespace kiarabot::core::platform
