#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
Json task_page_json(const wire::SchedulerListTasksResponse& response) {
  // OpenAPI TaskPage(GenericResource items + PageInfo) instead of the raw proto JSON shape.
  Json items = Json::array();
  for (const auto& task : response.tasks()) {
    Json item{{"id", task.task_id()}};
    item["name"] = task.target().id().empty() ? task.task_id() : task.target().id();
    item["state"] = wire::ScheduledTaskState_Name(task.state());
    if (task.has_created_at()) {
      item["created_at"] = json_time(task.created_at());
    }
    items.push_back(std::move(item));
  }
  Json page{{"page_size", response.page().page_size()}};
  if (!response.page().next_page_token().empty()) {
    page["next_page_token"] = response.page().next_page_token();
  }
  return {{"items", std::move(items)}, {"page", std::move(page)}};
}

} // namespace

std::optional<OwnerHttpResult> handle_tasks(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  constexpr std::string_view kTasks = "/api/v1/tasks";
  const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
  if (!ports.tasks_ && path.starts_with(kTasks)) {
    return context.error(wire::UPSTREAM_UNAVAILABLE);
  }
  if (path == kTasks && method == "GET") {
    wire::SchedulerListTasksRequest input;
    auto query = parse_query(request.target);
    if (!query || !fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto scope = optional_scope(*query);
    if (!scope) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    *input.mutable_scope() = std::move(*scope);
    if (!fill_page(input, *query, 0)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation =
        context.delegate(ports.delegation_, "scheduler", wire::PERMISSION_TASK_READ, input.scope());
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code);
    }
    auto result = ports.tasks_->list_tasks(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code);
    }
    return OwnerHttpResult{
        200, task_page_json(std::get<wire::SchedulerListTasksResponse>(result)).dump()};
  }
  if (path.starts_with(kTasks) && path.ends_with("/cancel") && method == "POST") {
    const auto prefix = std::string{kTasks} + "/";
    const auto task = std::string{
        path.substr(prefix.size(), path.size() - prefix.size() - std::string{"/cancel"}.size())};
    auto query = parse_query(request.target);
    if (task.empty() || task.find('/') != std::string::npos || !query) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto scope = optional_scope(*query);
    if (!scope) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    wire::SchedulerCancelTaskRequest input;
    input.set_task_id(task);
    *input.mutable_scope() = std::move(*scope);
    if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation = context.delegate(ports.delegation_, "scheduler",
                                       wire::PERMISSION_TASK_SCHEDULE, input.scope());
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code);
    }
    auto result = ports.tasks_->cancel_task(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code);
    }
    return OwnerHttpResult{200, json_response(std::get<wire::SchedulerCancelTaskResponse>(result))};
  }
  if (path.starts_with(kTasks) && method == "GET") {
    const auto prefix = std::string{kTasks} + "/";
    const auto task = std::string{path.substr(prefix.size())};
    auto query = parse_query(request.target);
    if (task.empty() || task.find('/') != std::string::npos || !query) {
      return context.error(wire::NOT_FOUND);
    }
    auto scope = optional_scope(*query);
    if (!scope) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    wire::SchedulerGetTaskRequest input;
    input.set_task_id(task);
    *input.mutable_scope() = std::move(*scope);
    if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation =
        context.delegate(ports.delegation_, "scheduler", wire::PERMISSION_TASK_READ, input.scope());
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code);
    }
    auto result = ports.tasks_->get_task(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code);
    }
    return OwnerHttpResult{200, json_response(std::get<wire::SchedulerGetTaskResponse>(result))};
  }
  constexpr std::string_view kOccurrences = "/api/v1/occurrences/";
  if (path.starts_with(kOccurrences)) {
    if (request.method != "GET" && request.method != "HEAD") {
      return context.method_not_allowed();
    }
    if (request.access_token.empty()) {
      return context.error(wire::UNAUTHENTICATED, false);
    }
    if (!ports.tasks_) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    const auto occurrence = std::string{path.substr(kOccurrences.size())};
    auto query = parse_query(request.target);
    if (occurrence.empty() || occurrence.find('/') != std::string::npos || !query) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    auto scope = parse_scope(*query);
    if (!scope) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::SchedulerGetOccurrenceRequest input;
    input.set_occurrence_id(occurrence);
    *input.mutable_scope() = std::move(*scope);
    if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    auto delegation =
        context.delegate(ports.delegation_, "scheduler", wire::PERMISSION_TASK_READ, input.scope());
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result = ports.tasks_->get_occurrence(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    return OwnerHttpResult{200,
                           json_response(std::get<wire::SchedulerGetOccurrenceResponse>(result))};
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
