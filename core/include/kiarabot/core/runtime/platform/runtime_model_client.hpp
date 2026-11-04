#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/ports/target/runtime_model.hpp"
#include "kiarabot/v1/model.grpc.pb.h"

namespace kiarabot::core::platform {

// Per-request Model delegation proof for audience=model (hops=2 instance-subject child). The
// request carries the authenticated instance id, so acquisition can be keyed by the real caller.
using ModelDelegationProvider =
    std::function<target::Outcome<std::string>(const target::RuntimeModelChatRequest&)>;

struct RuntimeModelClientOptions {
  std::string endpoint;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  // P05b-4: Runtime instance-parent acquisition + ExchangeDelegation provider. Missing/absent
  // configuration keeps the fail-closed default; no proof means no Model call.
  ModelDelegationProvider delegation_provider;
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
  target::Outcome<target::RuntimeModelStreamResult>
  chat_stream(const target::RuntimeModelStreamRequest& request,
              target::RuntimeModelStreamSink& sink, std::stop_token cancellation,
              target::TimePoint deadline) override;

private:
  explicit RuntimeModelClient(RuntimeModelClientOptions options);
  RuntimeModelClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::ModelService::Stub> stub_;
};

// Fail-closed provider used until/unless P05 instance delegation acquisition is configured.
ModelDelegationProvider unavailable_model_delegation();

} // namespace kiarabot::core::platform
