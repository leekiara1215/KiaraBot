#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
Json operation_json(const wire::Operation& operation) {
  Json value{{"ref",
              {{"owner_service", operation.ref().owner_service()},
               {"operation_id", operation.ref().operation_id()}}}};
  if (operation.ref().has_resource()) {
    value["ref"]["resource"] = {
        {"kind", wire::ResourceKind_Name(operation.ref().resource().kind())},
        {"id", operation.ref().resource().id()}};
  }
  value["version"] = std::to_string(operation.version());
  value["state"] = wire::OperationState_Name(operation.state());
  value["execution_outcome"] = wire::ExecutionOutcome_Name(operation.execution_outcome());
  value["observed_at"] = json_time(operation.observed_at());
  if (operation.has_reason()) {
    value["reason"] = {{"code", wire::ErrorCode_Name(operation.reason().code())},
                       {"message", operation.reason().message()},
                       {"request_id", operation.reason().request_id()},
                       {"trace_id", operation.reason().trace_id()},
                       {"retryable", operation.reason().retryable()},
                       {"details", Json::object()}};
  }
  Json participants = Json::array();
  for (const auto& participant : operation.participants()) {
    Json item{{"participant", participant.participant()},
              {"state", wire::OperationState_Name(participant.state())}};
    if (participant.has_next_attempt_at()) {
      item["next_attempt_at"] = json_time(participant.next_attempt_at());
    }
    if (participant.has_reason()) {
      item["reason"] = {{"code", wire::ErrorCode_Name(participant.reason().code())}};
    }
    participants.push_back(std::move(item));
  }
  value["participants"] = std::move(participants);
  if (operation.has_next_attempt_at()) {
    value["next_attempt_at"] = json_time(operation.next_attempt_at());
  }
  value["stalled"] = operation.stalled();
  return value;
}

} // namespace

std::optional<OwnerHttpResult> handle_operations(const RequestContext& context,
                                                 const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  constexpr std::string_view kOperations = "/api/v1/operations/";
  if (path.starts_with(kOperations)) {
    if (!ports.operations_) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
    if (method != "GET") {
      return context.method_not_allowed();
    }
    const auto tail = path.substr(kOperations.size());
    const auto slash = tail.find('/');
    if (slash == std::string_view::npos) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    const auto owner = std::string{tail.substr(0, slash)};
    const auto operation_id = std::string{tail.substr(slash + 1)};
    auto query = parse_query(request.target);
    auto scope = query ? parse_scope(*query) : std::nullopt;
    if (operation_id.empty() || operation_id.find('/') != std::string::npos || !scope) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::Permission permission = wire::PERMISSION_UNSPECIFIED;
    if (owner == "runtime" || owner == "model") {
      permission = wire::PERMISSION_RESOURCE_READ;
    } else if (owner == "scheduler") {
      permission = wire::PERMISSION_TASK_READ;
    } else {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::OperationRef reference;
    reference.set_owner_service(owner);
    reference.set_operation_id(operation_id);
    auto delegation = context.delegate(ports.delegation_, owner, permission, *scope);
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result = ports.operations_->get(owner, reference, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    const auto& operation = std::get<wire::Operation>(result);
    if (operation.ref().operation_id().empty()) {
      // Missing/invisible operations are NOT_FOUND; never claim completion.
      return context.error(wire::NOT_FOUND, false);
    }
    return OwnerHttpResult{200, Json{{"operation", operation_json(operation)}}.dump()};
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
