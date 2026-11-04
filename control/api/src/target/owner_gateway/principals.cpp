#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {

std::optional<OwnerHttpResult> handle_principals(const RequestContext& context,
                                                 const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  if (path == "/api/v1/principals") {
    if (request.method != "GET" && request.method != "HEAD") {
      return context.method_not_allowed();
    }
    if (request.access_token.empty()) {
      return context.error(wire::UNAUTHENTICATED, false);
    }
    if (!ports.principals_) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    auto query = parse_query(request.target);
    if (!query) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::IdentityListPrincipalsRequest input;
    auto* wire_context = input.mutable_context();
    wire_context->set_request_id(request_id);
    wire_context->set_trace_id(trace_id);
    const auto deadline = codec::to_wire(request.deadline);
    if (const auto* wire_deadline = std::get_if<google::protobuf::Timestamp>(&deadline)) {
      *wire_context->mutable_deadline() = *wire_deadline;
    }
    if (!fill_page(input, *query, 1)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation =
        context.delegate(ports.delegation_, "identity", wire::PERMISSION_IDENTITY_PRINCIPAL_READ,
                         control_catalog_scope());
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result = ports.principals_->list_principals(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    google::protobuf::util::JsonPrintOptions options;
    options.preserve_proto_field_names = true;
    std::string body;
    if (!google::protobuf::util::MessageToJsonString(
             std::get<wire::IdentityListPrincipalsResponse>(result), &body, options)
             .ok()) {
      return context.error(wire::INTERNAL, false);
    }
    return OwnerHttpResult{200, body};
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
