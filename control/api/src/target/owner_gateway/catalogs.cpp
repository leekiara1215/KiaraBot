#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {

std::optional<OwnerHttpResult> handle_catalogs(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  // A snapshot is deliberately limited to the exact current child grants for one permission.
  // The owner validates the registered catalog family and rechecks each child on every page.
  if ((request.method == "GET" || request.method == "HEAD") &&
      (path == "/api/v1/accounts" || path == "/api/v1/installations" ||
       path == "/api/v1/instances" || path == "/api/v1/adapter-instances" ||
       path == "/api/v1/hosts")) {
    if (!ports.resources_) {
      return context.failure({wire::UPSTREAM_UNAVAILABLE, true});
    }
    const auto query = parse_query(request.target);
    wire::ResourceRef catalog;
    if (!query || !query->contains("catalog") ||
        !google::protobuf::util::JsonStringToMessage(query->at("catalog"), &catalog).ok() ||
        !valid_catalog(catalog)) {
      return context.failure({wire::INVALID_ARGUMENT, false});
    }
    for (const auto& [key, value] : *query) {
      if (key != "catalog" && key != "scope" && key != "page_size" && key != "page_token") {
        return context.failure({wire::INVALID_ARGUMENT, false});
      }
    }
    auto permission = wire::PERMISSION_ACCOUNT_READ;
    auto child_kind = wire::RESOURCE_ACCOUNT;
    if (path == "/api/v1/installations" || path == "/api/v1/instances") {
      permission = wire::PERMISSION_PLUGIN_READ;
      child_kind =
          path == "/api/v1/installations" ? wire::RESOURCE_INSTALLATION : wire::RESOURCE_INSTANCE;
    } else if (path == "/api/v1/adapter-instances") {
      permission = wire::PERMISSION_ADAPTER_READ;
      child_kind = wire::RESOURCE_ADAPTER;
    } else if (path == "/api/v1/hosts") {
      permission = wire::PERMISSION_HOST_READ;
      child_kind = wire::RESOURCE_HOST;
    }
    std::optional<wire::ResourceScope> selection;
    if (query->contains("scope")) {
      selection.emplace();
      if (!google::protobuf::util::JsonStringToMessage(query->at("scope"), &*selection).ok()) {
        return context.failure({wire::INVALID_ARGUMENT, false});
      }
      std::set<std::string> unique;
      for (const auto& resource : selection->resources()) {
        if (resource.kind() != child_kind || !catalog_uuid(resource.id()) ||
            !unique.insert(resource.id()).second) {
          return context.failure({wire::INVALID_ARGUMENT, false});
        }
      }
      if (selection->resources_size() > 127) {
        return context.failure({wire::RATE_LIMITED, true});
      }
    }
    wire::PageRequest page;
    page.set_page_size(20);
    if (query->contains("page_size")) {
      const auto& raw = query->at("page_size");
      std::uint32_t size = 0;
      const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), size);
      if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() || size == 0 ||
          size > 100) {
        return context.failure({wire::INVALID_ARGUMENT, false});
      }
      page.set_page_size(size);
    }
    if (query->contains("page_token")) {
      page.set_page_token(query->at("page_token"));
    }
    auto scope = ports.delegation_.catalog_scope(permission, catalog, child_kind, selection,
                                                 request.access_token, request.deadline);
    if (const auto* failure = std::get_if<OwnerFailure>(&scope)) {
      return context.failure(*failure);
    }
    auto proof = context.delegate(ports.delegation_, "runtime", permission,
                                  std::get<wire::ResourceScope>(scope));
    if (const auto* failure = std::get_if<OwnerFailure>(&proof)) {
      return context.failure(*failure);
    }
    const auto forward =
        [&]<typename Input, typename Response>(OwnerOutcome<Response> (RuntimeResourcesPort::*call)(
            const Input&, std::string_view)) -> OwnerHttpResult {
      Input input;
      if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
        return context.failure({wire::INVALID_ARGUMENT, false});
      }
      *input.mutable_catalog() = catalog;
      *input.mutable_page() = page;
      const auto result = (ports.resources_->*call)(input, std::get<std::string>(proof));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      auto value = json_request_value(json_response(std::get<Response>(result)));
      const auto field = child_kind == wire::RESOURCE_ACCOUNT        ? "accounts"
                         : child_kind == wire::RESOURCE_INSTALLATION ? "installations"
                         : child_kind == wire::RESOURCE_INSTANCE     ? "instances"
                         : child_kind == wire::RESOURCE_ADAPTER      ? "adapters"
                                                                     : "hosts";
      if (!value.contains(field)) {
        value[field] = Json::array();
      }
      return {200, value.dump()};
    };
    if (child_kind == wire::RESOURCE_ACCOUNT) {
      return forward(&RuntimeResourcesPort::list_accounts);
    }
    if (child_kind == wire::RESOURCE_INSTALLATION) {
      return forward(&RuntimeResourcesPort::list_installations);
    }
    if (child_kind == wire::RESOURCE_INSTANCE) {
      return forward(&RuntimeResourcesPort::list_instances);
    }
    if (child_kind == wire::RESOURCE_ADAPTER) {
      return forward(&RuntimeResourcesPort::list_adapters);
    }
    return forward(&RuntimeResourcesPort::list_hosts);
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
