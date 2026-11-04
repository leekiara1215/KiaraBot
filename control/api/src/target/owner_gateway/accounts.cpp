#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {

std::optional<OwnerHttpResult> handle_accounts(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  constexpr std::string_view kBind = "/api/v1/adapter-bindings/";
  constexpr std::string_view kPolicy = "/api/v1/accounts/";
  const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
  if (!ports.resources_ &&
      (path.starts_with(kBind) || path == "/api/v1/accounts" || path.starts_with(kPolicy))) {
    return context.error(wire::UPSTREAM_UNAVAILABLE);
  }
  if (path.starts_with(kBind)) {
    const auto account = std::string{path.substr(kBind.size())};
    if (account.empty() || account.find('/') != std::string::npos) {
      return context.error(wire::NOT_FOUND);
    }
    if (method == "PUT") {
      wire::RuntimeBindAdapterRequest input;
      if (!json_request(request.body, input) || !input.has_binding() ||
          input.binding().account_id() != account || !input.has_expected_version()) {
        return context.error(wire::INVALID_ARGUMENT);
      }
      if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
        return context.error(wire::INVALID_ARGUMENT);
      }
      if (!input.bootstrap_recipient_id().empty()) {
        wire::ResourceRef resource;
        resource.set_kind(wire::RESOURCE_ACCOUNT);
        resource.set_id(account);
        // Fixed offline recipient validation happens before the owner mutation so an unknown or
        // wrong-resource recipient never leaves a committed credential without a delivery target.
        if (ports.bootstrap_recipients_ == nullptr ||
            !ports.bootstrap_recipients_->known(input.bootstrap_recipient_id(), resource)) {
          return context.error(wire::INVALID_ARGUMENT);
        }
      }
      auto delegation = context.delegate(ports.delegation_, "runtime",
                                         wire::PERMISSION_ACCOUNT_MANAGE, account_scope(account));
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.error(failure->code);
      }
      auto result = ports.resources_->bind_adapter(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.error(failure->code);
      }
      auto value =
          json_request_value(json_response(std::get<wire::RuntimeBindAdapterResponse>(result)));
      return deliver_bootstrap_response(context, ports, std::get<std::string>(delegation),
                                        /*adapter=*/true, value);
    }
    if (method == "DELETE") {
      wire::RuntimeUnbindAdapterRequest input;
      if (!json_request(request.body, input) || !input.has_binding() ||
          input.binding().account_id() != account || !input.has_expected_version()) {
        return context.error(wire::INVALID_ARGUMENT);
      }
      if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
        return context.error(wire::INVALID_ARGUMENT);
      }
      auto delegation = context.delegate(ports.delegation_, "runtime",
                                         wire::PERMISSION_ACCOUNT_MANAGE, account_scope(account));
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.error(failure->code);
      }
      auto result = ports.resources_->unbind_adapter(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.error(failure->code);
      }
      return OwnerHttpResult{200,
                             json_response(std::get<wire::RuntimeUnbindAdapterResponse>(result))};
    }
    return context.error(wire::INVALID_ARGUMENT);
  }
  if (path == "/api/v1/accounts" && method == "POST") {
    wire::RuntimeCreateAccountRequest input;
    if (!json_request(request.body, input) || !input.has_catalog() ||
        !valid_catalog(input.catalog()) ||
        !fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation =
        context.delegate(ports.delegation_, "runtime", wire::PERMISSION_ACCOUNT_MANAGE,
                         exact_scope(input.catalog()));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code);
    }
    auto result = ports.resources_->create_account(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code);
    }
    return OwnerHttpResult{200,
                           json_response(std::get<wire::RuntimeCreateAccountResponse>(result))};
  }
  if (path.starts_with(kPolicy) && path.ends_with("/action-policy")) {
    const auto prefix = std::string{kPolicy};
    const auto account = std::string{path.substr(
        prefix.size(), path.size() - prefix.size() - std::string{"/action-policy"}.size())};
    if (account.empty() || account.find('/') != std::string::npos) {
      return context.error(wire::NOT_FOUND);
    }
    const auto scope = account_scope(account);
    if (method == "GET") {
      wire::RuntimeGetActionPolicyRequest input;
      input.set_account_id(account);
      if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
        return context.error(wire::INVALID_ARGUMENT);
      }
      auto delegation = context.delegate(ports.delegation_, "runtime",
                                         wire::PERMISSION_ACCOUNT_POLICY_READ, scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.error(failure->code);
      }
      auto result = ports.resources_->get_action_policy(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.error(failure->code);
      }
      return OwnerHttpResult{200,
                             json_response(std::get<wire::RuntimeGetActionPolicyResponse>(result))};
    }
    if (method == "PUT") {
      wire::RuntimeUpdateActionPolicyRequest input;
      if (!json_request(request.body, input) || !input.has_policy() ||
          input.policy().account_id() != account || !input.has_expected_version() ||
          !fill_context(input.mutable_context(), request_id, trace_id, request)) {
        return context.error(wire::INVALID_ARGUMENT);
      }
      auto delegation = context.delegate(ports.delegation_, "runtime",
                                         wire::PERMISSION_ACCOUNT_POLICY_MANAGE, scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.error(failure->code);
      }
      auto result =
          ports.resources_->update_action_policy(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.error(failure->code);
      }
      return OwnerHttpResult{
          200, json_response(std::get<wire::RuntimeUpdateActionPolicyResponse>(result))};
    }
    return context.error(wire::INVALID_ARGUMENT);
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
