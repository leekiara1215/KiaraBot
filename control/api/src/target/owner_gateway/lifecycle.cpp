#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {

std::optional<OwnerHttpResult> handle_lifecycle(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  constexpr std::string_view kResources = "/api/v1/resources/";
  if (path.starts_with(kResources)) {
    if (request.method != "POST") {
      return context.method_not_allowed();
    }
    const auto tail = path.substr(kResources.size());
    const auto first = tail.find('/');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : tail.find('/', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) {
      return context.error(wire::NOT_FOUND, false);
    }
    const auto kind_name = tail.substr(0, first);
    const auto resource_id = std::string{tail.substr(first + 1, second - first - 1)};
    const auto action = tail.substr(second + 1);
    if (!action.empty() && action.find('/') != std::string_view::npos) {
      return context.error(wire::NOT_FOUND, false);
    }
    wire::ResourceKind kind = wire::RESOURCE_KIND_UNSPECIFIED;
    if (!wire::ResourceKind_Parse(std::string{kind_name}, &kind) ||
        kind == wire::RESOURCE_KIND_UNSPECIFIED || resource_id.empty() ||
        (action != "delete" && action != "restore")) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    if (!ports.resources_) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    if (request.access_token.empty()) {
      return context.error(wire::UNAUTHENTICATED, false);
    }
    auto query = parse_query(request.target);
    if (!query || !query->empty()) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    const auto parsed = Json::parse(request.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("resource") ||
        !parsed["resource"].is_object() || !parsed.contains("expected_version") ||
        !parsed["expected_version"].is_string()) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::ResourceRef resource;
    if (!google::protobuf::util::JsonStringToMessage(parsed["resource"].dump(), &resource).ok() ||
        resource.kind() != kind || resource.id() != resource_id) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    std::uint64_t expected_version = 0;
    const auto& version_text = parsed["expected_version"].get_ref<const std::string&>();
    const auto converted = std::from_chars(
        version_text.data(), version_text.data() + version_text.size(), expected_version);
    if (converted.ec != std::errc{} || converted.ptr != version_text.data() + version_text.size() ||
        expected_version == 0) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    const auto idempotency = header(request, "idempotency-key");
    if (!idempotency || idempotency->size() < 16 || idempotency->size() > 128) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::ResourceScope scope;
    *scope.add_resources() = resource;
    const auto permission = kind == wire::RESOURCE_ACCOUNT ? wire::PERMISSION_ACCOUNT_MANAGE
                                                           : wire::PERMISSION_RESOURCE_MANAGE;
    auto delegation = context.delegate(ports.delegation_, "runtime", permission, scope);
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    const auto wire_deadline = codec::to_wire(request.deadline);
    const auto* deadline = std::get_if<google::protobuf::Timestamp>(&wire_deadline);
    if (deadline == nullptr) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::OperationRef operation;
    if (action == "delete") {
      wire::RuntimeBeginDeleteRequest input;
      auto* wire_context = input.mutable_context();
      wire_context->set_request_id(request_id);
      wire_context->set_trace_id(trace_id);
      wire_context->set_idempotency_key(*idempotency);
      *wire_context->mutable_deadline() = *deadline;
      *input.mutable_resource() = resource;
      input.set_expected_version(expected_version);
      auto result = ports.resources_->begin_delete(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      operation = std::get<wire::RuntimeBeginDeleteResponse>(result).operation();
    } else {
      wire::RuntimeRestoreResourceRequest input;
      auto* wire_context = input.mutable_context();
      wire_context->set_request_id(request_id);
      wire_context->set_trace_id(trace_id);
      wire_context->set_idempotency_key(*idempotency);
      *wire_context->mutable_deadline() = *deadline;
      *input.mutable_resource() = resource;
      input.set_expected_version(expected_version);
      auto result = ports.resources_->restore_resource(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      operation = std::get<wire::RuntimeRestoreResourceResponse>(result).operation();
    }
    if (operation.operation_id().empty()) {
      return context.error(wire::INTERNAL, false);
    }
    // 202 means durably accepted; a BLOCKED participant is an accepted operation, not a failed
    // command. Clients poll GET /api/v1/operations/runtime/{operation_id} for progress.
    Json body{{"operation",
               {{"owner_service",
                 operation.owner_service().empty() ? "runtime" : operation.owner_service()},
                {"operation_id", operation.operation_id()}}}};
    if (operation.has_resource()) {
      body["operation"]["resource"] = {
          {"kind", wire::ResourceKind_Name(operation.resource().kind())},
          {"id", operation.resource().id()}};
    }
    return OwnerHttpResult{202, body.dump()};
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
