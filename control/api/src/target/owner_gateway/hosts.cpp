#include <charconv>
#include <string>
#include <string_view>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
constexpr std::string_view kHosts = "/api/v1/hosts/";
constexpr std::string_view kCancel = "/api/v1/operations/host/";

Json maintenance_json(const wire::HostMaintenanceRecord& value) {
  Json record{{"host_id", value.host_id()}};
  record["version"] = std::to_string(value.version());
  record["admission"] = value.admission() == wire::HOST_ADMISSION_OPEN ? "HOST_ADMISSION_OPEN"
                                                                       : "HOST_ADMISSION_CLOSED";
  record["observed_at"] = json_time(value.observed_at());
  if (!value.operation().operation_id().empty()) {
    Json operation{{"owner_service", value.operation().owner_service()},
                   {"operation_id", value.operation().operation_id()}};
    if (value.operation().has_resource()) {
      operation["resource"] = {
          {"kind", wire::ResourceKind_Name(value.operation().resource().kind())},
          {"id", value.operation().resource().id()}};
    }
    record["operation"] = std::move(operation);
  }
  return record;
}

unsigned int accepted_status(const wire::Operation& operation) {
  // 202 means only durably accepted, never completed work. The owner operation remains the sole
  // completion authority; transport failures never synthesize a state here.
  return operation.state() == wire::OPERATION_ACCEPTED ? 202 : 200;
}

bool positive_uint64(const Json& value, std::uint64_t* output) {
  if (!value.is_string()) {
    return false;
  }
  const auto& text = value.get_ref<const std::string&>();
  if (text.empty() || text.front() == '0') {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || parsed == 0) {
    return false;
  }
  *output = parsed;
  return true;
}

// Client-supplied cancellation scope is forwarded exactly once. Control cannot read the stored
// operation resource; the Host owner validates the stored resource, so a relabel becomes NOT_FOUND
// at the owner instead of a false success.
std::optional<wire::ResourceScope> cancel_scope(const Json& body) {
  if (!body.is_object() || !body.contains("scope") || !body["scope"].is_object()) {
    return std::nullopt;
  }
  const auto& scope_value = body["scope"];
  if (!scope_value.contains("resources") || !scope_value["resources"].is_array() ||
      scope_value["resources"].size() != 1) {
    return std::nullopt;
  }
  const auto& resource = scope_value["resources"][0];
  if (!resource.is_object() || !resource.contains("kind") || !resource.contains("id") ||
      resource["kind"] != "RESOURCE_HOST" || !resource["id"].is_string()) {
    return std::nullopt;
  }
  const auto id = resource["id"].get<std::string>();
  if (!canonical_uuid(id)) {
    return std::nullopt;
  }
  return host_scope(id);
}
} // namespace

std::optional<OwnerHttpResult> handle_hosts(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
  const bool cancel_route = path.starts_with(kCancel);
  if (!cancel_route && !path.starts_with(kHosts)) {
    return std::nullopt;
  }
  if (!ports.host_management_) {
    return context.error(wire::UPSTREAM_UNAVAILABLE, true);
  }
  if (cancel_route) {
    if (method != "POST") {
      return std::nullopt;
    }
    const auto tail = std::string{path.substr(kCancel.size())};
    constexpr std::string_view kSuffix = "/cancel";
    if (tail.size() <= kSuffix.size() || !tail.ends_with(kSuffix)) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    const auto operation_id = tail.substr(0, tail.size() - kSuffix.size());
    if (operation_id.find('/') != std::string::npos || !canonical_uuid(operation_id)) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    Json body;
    try {
      body = Json::parse(request.body);
    } catch (...) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    std::uint64_t expected_version = 0;
    auto scope = cancel_scope(body);
    if (!body.is_object() || body.size() != 2 || !body.contains("expected_version") ||
        !positive_uint64(body["expected_version"], &expected_version) || !scope) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    wire::HostCancelOperationRequest input;
    if (!fill_context(input.mutable_context(), context.request_id_, context.trace_id_, request)) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    input.mutable_operation()->set_owner_service("host");
    input.mutable_operation()->set_operation_id(operation_id);
    *input.mutable_operation()->mutable_resource() = scope->resources(0);
    input.set_expected_version(expected_version);
    auto delegation =
        context.delegate(ports.delegation_, "host", wire::PERMISSION_HOST_EXECUTE, *scope);
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result =
        ports.host_management_->cancel_operation(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    const auto& operation = std::get<wire::HostGetOperationResponse>(result).operation();
    return OwnerHttpResult{accepted_status(operation),
                           Json{{"operation", operation_json(operation)}}.dump()};
  }
  const auto tail = std::string{path.substr(kHosts.size())};
  const auto slash = tail.find('/');
  if (slash == std::string::npos) {
    return std::nullopt; // GET /hosts/{id} belongs to the Runtime typed projection route.
  }
  const auto host_id = tail.substr(0, slash);
  const auto action = tail.substr(slash + 1);
  if (!canonical_uuid(host_id)) {
    return context.error(wire::INVALID_ARGUMENT, false);
  }
  if (action == "maintenance") {
    if (method == "GET") {
      wire::HostGetMaintenanceRequest input;
      input.set_host_id(host_id);
      if (!fill_context(input.mutable_context(), context.request_id_, context.trace_id_, request)) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      auto delegation = context.delegate(ports.delegation_, "host", wire::PERMISSION_HOST_READ,
                                         host_scope(host_id));
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      auto result =
          ports.host_management_->get_maintenance(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      const auto& maintenance = std::get<wire::HostGetMaintenanceResponse>(result).maintenance();
      if (maintenance.host_id() != host_id) {
        return context.error(wire::INTERNAL, false);
      }
      return OwnerHttpResult{200, Json{{"maintenance", maintenance_json(maintenance)}}.dump()};
    }
    if (method != "POST") {
      return context.method_not_allowed();
    }
    wire::HostBeginMaintenanceRequest input;
    if (!json_request(request.body, input) || !input.host_id().empty() ||
        !input.has_expected_version() || input.expected_version() == 0 ||
        input.grace_ms() > 10000 ||
        !fill_context(input.mutable_context(), context.request_id_, context.trace_id_, request)) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    input.set_host_id(host_id);
    auto delegation = context.delegate(ports.delegation_, "host", wire::PERMISSION_HOST_EXECUTE,
                                       host_scope(host_id));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result =
        ports.host_management_->begin_maintenance(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    const auto& response = std::get<wire::HostMaintenanceResponse>(result);
    if (response.maintenance().host_id() != host_id ||
        response.operation().ref().operation_id().empty()) {
      return context.error(wire::INTERNAL, false);
    }
    return OwnerHttpResult{accepted_status(response.operation()),
                           Json{{"maintenance", maintenance_json(response.maintenance())},
                                {"operation", operation_json(response.operation())}}
                               .dump()};
  }
  if (action == "resume") {
    if (method != "POST") {
      return context.method_not_allowed();
    }
    wire::HostResumeAdmissionRequest input;
    if (!json_request(request.body, input) || !input.host_id().empty() ||
        !input.has_expected_version() || input.expected_version() == 0 ||
        !fill_context(input.mutable_context(), context.request_id_, context.trace_id_, request)) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    input.set_host_id(host_id);
    auto delegation = context.delegate(ports.delegation_, "host", wire::PERMISSION_HOST_EXECUTE,
                                       host_scope(host_id));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result =
        ports.host_management_->resume_admission(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    const auto& response = std::get<wire::HostMaintenanceResponse>(result);
    if (response.maintenance().host_id() != host_id ||
        response.operation().ref().operation_id().empty()) {
      return context.error(wire::INTERNAL, false);
    }
    return OwnerHttpResult{accepted_status(response.operation()),
                           Json{{"maintenance", maintenance_json(response.maintenance())},
                                {"operation", operation_json(response.operation())}}
                               .dump()};
  }
  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
