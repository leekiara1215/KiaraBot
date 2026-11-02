#include "kiarabot/core/runtime/platform/runtime_model_client.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = kiarabot::core::target;
namespace codec = target_codec;
using namespace std::chrono_literals;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose,
                   t::Acceptance acceptance = t::Acceptance::NotAccepted) {
  return {code, acceptance, recovery, {}, {}};
}
t::TimePoint now() {
  return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
}
bool timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  const auto encoded = codec::to_wire(value);
  if (const auto* wire = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *wire;
    return true;
  }
  return false;
}
t::Failure grpc_failure(const grpc::Status& status) {
  switch (status.error_code()) {
  case grpc::StatusCode::UNAUTHENTICATED:
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  case grpc::StatusCode::PERMISSION_DENIED:
    return failure(t::ErrorCode::PermissionDenied, t::Recovery::Authenticate);
  case grpc::StatusCode::INVALID_ARGUMENT:
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  case grpc::StatusCode::UNAVAILABLE:
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  default:
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}
std::string random_uuid() {
  return {}; // trace_id is optional for transport correlation; request_id stays caller-owned.
}
} // namespace

std::function<t::Outcome<std::string>()> unavailable_model_delegation() {
  return []() -> t::Outcome<std::string> {
    return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
  };
}

RuntimeModelClient::RuntimeModelClient(RuntimeModelClientOptions options)
    : options_(std::move(options)),
      channel_(grpc::CreateChannel(options_.endpoint, options_.channel_credentials)),
      stub_(v1::ModelService::NewStub(channel_)) {}

RuntimeModelClient::~RuntimeModelClient() = default;

t::Outcome<std::unique_ptr<RuntimeModelClient>>
RuntimeModelClient::create(RuntimeModelClientOptions options) {
  if (options.endpoint.empty() || !options.channel_credentials || !options.delegation_provider ||
      options.maximum_budget.count() <= 0) {
    return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
  }
  try {
    return std::unique_ptr<RuntimeModelClient>(new RuntimeModelClient(std::move(options)));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

bool RuntimeModelClient::available() const noexcept {
  return channel_ && channel_->GetState(true) == GRPC_CHANNEL_READY;
}

t::Outcome<t::RuntimeModelChatReply>
RuntimeModelClient::chat(const t::RuntimeModelChatRequest& request, t::TimePoint deadline) {
  try {
    if (request.logical_call_id.empty() || request.model.empty() || request.messages.empty()) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    const auto started = now();
    if (deadline <= started) {
      return failure(t::ErrorCode::Timeout, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
    }
    auto delegation = options_.delegation_provider();
    if (const auto* rejected = std::get_if<t::Failure>(&delegation)) {
      return *rejected;
    }
    auto& proof = std::get<std::string>(delegation);
    if (proof.empty() || proof.size() > 32768) {
      return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
    }
    v1::ModelGatewayChatRequest input;
    auto* context = input.mutable_context();
    context->set_request_id(request.request_id.empty() ? request.logical_call_id
                                                       : request.request_id);
    if (!timestamp(std::min(deadline, started + options_.maximum_budget),
                   context->mutable_deadline())) {
      return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
    }
    context->set_delegation(proof);
    input.set_logical_call_id(request.logical_call_id);
    input.set_model_id(request.model);
    for (const auto& message : request.messages) {
      auto* wire = input.add_messages();
      wire->set_role(message.role);
      wire->set_text(message.text);
    }
    v1::ModelGatewayChatResponse output;
    grpc::ClientContext call;
    call.set_deadline(std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::min(deadline, started + options_.maximum_budget).time_since_epoch())});
    const auto status = stub_->Chat(&call, input, &output);
    if (!status.ok()) {
      return grpc_failure(status);
    }
    if (output.disposition() == v1::FRESH_RESULT) {
      if (!output.has_result()) {
        return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity,
                       t::Acceptance::Unknown);
      }
      return t::RuntimeModelChatReply{output.result().text(), output.result().input_tokens(),
                                      output.result().output_tokens()};
    }
    if (output.disposition() == v1::REPLAY_TERMINAL && output.has_result()) {
      return t::RuntimeModelChatReply{output.result().text(), output.result().input_tokens(),
                                      output.result().output_tokens()};
    }
    // REPLAY_IN_PROGRESS and unknown dispositions never fabricate a result; the plugin must
    // re-issue/query with the same logical_call_id (QUERY_SAME_INTENT recovery).
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::QuerySameIdentity,
                   t::Acceptance::Unknown);
  } catch (...) {
    return failure(t::ErrorCode::Internal, t::Recovery::QuerySameIdentity, t::Acceptance::Unknown);
  }
}

} // namespace kiarabot::core::platform
