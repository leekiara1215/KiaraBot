#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"
#include "kiarabot/v1/actions.pb.h"

namespace kiarabot::control::target::owner_gateway {
namespace {
Json action_result_json(const wire::ActionResult& result) {
  Json value{{"action_id", result.action_id()}};
  value["status"] = wire::ActionStatus_Name(result.status());
  value["execution_outcome"] = wire::ExecutionOutcome_Name(result.execution_outcome());
  if (!result.platform_message_id().empty()) {
    value["platform_message_id"] = result.platform_message_id();
  }
  if (result.has_error()) {
    value["error"] = {{"code", wire::ErrorCode_Name(result.error().code())},
                      {"message", result.error().message()},
                      {"request_id", result.error().request_id()},
                      {"trace_id", result.error().trace_id()},
                      {"retryable", result.error().retryable()},
                      {"details", Json::object()}};
  }
  if (result.has_completed_at()) {
    value["completed_at"] = json_time(result.completed_at());
  }
  if (!result.dispatch_attempt_id().empty()) {
    value["dispatch_attempt_id"] = result.dispatch_attempt_id();
  }
  return value;
}

} // namespace

std::optional<OwnerHttpResult> handle_actions(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  constexpr std::string_view kActions = "/api/v1/actions/";

  if (!path.starts_with(kActions)) {
    return std::nullopt;
  }
  if (request.method != "GET" && request.method != "HEAD") {
    return context.method_not_allowed();
  }
  auto query = parse_query(request.target);
  if (!query) {
    return context.error(wire::INVALID_ARGUMENT, false);
  }
  auto scope = parse_scope(*query);
  if (!scope) {
    return context.error(wire::INVALID_ARGUMENT, false);
  }
  const auto selector = path.substr(kActions.size());
  wire::RuntimeQueryActionResultRequest input;
  auto* wire_context = input.mutable_context();
  wire_context->set_request_id(request_id);
  wire_context->set_trace_id(trace_id);
  const auto deadline = codec::to_wire(request.deadline);
  if (const auto* wire_deadline = std::get_if<google::protobuf::Timestamp>(&deadline)) {
    *wire_context->mutable_deadline() = *wire_deadline;
  } else {
    return context.error(wire::INVALID_ARGUMENT, false);
  }
  *input.mutable_account_scope() = std::move(*scope);
  if (selector == "by-idempotency-key") {
    const auto key = header(request, "idempotency-key");
    const auto owner = query->find("owner_instance");
    if (!key || owner == query->end() || owner->second.empty()) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    input.set_idempotency_key(*key);
    input.set_owner_instance_id(owner->second);
  } else {
    if (selector.empty() || selector.find('/') != std::string_view::npos) {
      return std::nullopt;
    }
    input.set_action_id(std::string{selector});
  }
  // Exactly one account scope + action.inspect is checked by Identity/Runtime; Control never
  // authorizes from its own cache.
  auto delegation = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_ACTION_INSPECT,
                                     input.account_scope());
  if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
    return context.failure(*failure);
  }
  auto result = ports.runtime_.query_action(input, std::get<std::string>(delegation));
  if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
    return context.failure(*failure);
  }
  const auto& response = std::get<wire::RuntimeQueryActionResultResponse>(result);
  if (!response.has_result()) {
    // NOT_FOUND means absent/expired/invisible, never proof that the action failed.
    return context.error(wire::NOT_FOUND, false);
  }
  return OwnerHttpResult{200, action_result_json(response.result()).dump()};
}

} // namespace kiarabot::control::target::owner_gateway
