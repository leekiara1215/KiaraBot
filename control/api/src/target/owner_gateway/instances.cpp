#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
Json state_json(const wire::VersionedResourceState& state) {
  Json value{
      {"resource",
       {{"kind", wire::ResourceKind_Name(state.resource().kind())}, {"id", state.resource().id()}}},
      {"version", std::to_string(state.version())},
      {"lifecycle", wire::LifecycleState_Name(state.lifecycle())},
      {"desired", wire::DesiredState_Name(state.desired())},
      {"registered", state.registered()},
      {"desired_config_version", std::to_string(state.desired_config_version())},
      {"applied_config_version", std::to_string(state.applied_config_version())},
      {"process_outcome", wire::ExecutionOutcome_Name(state.process_outcome())},
      {"observed_at", json_time(state.observed_at())}};
  if (!state.source_service().empty()) {
    value["source_service"] = state.source_service();
  }
  return value;
}
Json instance_record_json(const wire::InstanceRecord& instance) {
  Json value{{"instance_id", instance.instance_id()},
             {"installation_id", instance.installation_id()},
             {"externally_managed", instance.externally_managed()},
             {"state", state_json(instance.state())}};
  if (!instance.host_id().empty()) {
    value["host_id"] = instance.host_id();
  }
  return value;
}
std::string instance_apply_state(const wire::VersionedResourceState& state) {
  // Do not claim APPLIED from desired state alone: only an equal, non-zero applied version counts
  // as plugin/Host confirmation; otherwise the fact stays PENDING/UNAVAILABLE.
  if (state.applied_config_version() > 0 &&
      state.applied_config_version() >= state.desired_config_version()) {
    return "CONFIG_APPLY_APPLIED";
  }
  if (state.desired_config_version() > 0) {
    return "CONFIG_APPLY_PENDING";
  }
  return "CONFIG_APPLY_UNAVAILABLE";
}

} // namespace

std::optional<OwnerHttpResult> handle_instances(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  constexpr std::string_view kInstallations = "/api/v1/installations";
  constexpr std::string_view kInstances = "/api/v1/instances/";
  const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
  if (!ports.resources_ && (path == kInstallations || path.starts_with(kInstances))) {
    return context.error(wire::UPSTREAM_UNAVAILABLE);
  }
  if (path == kInstallations && method == "POST") {
    wire::RuntimeCreateInstallationRequest input;
    if (!json_request(request.body, input) || !input.has_catalog() ||
        !valid_catalog(input.catalog()) ||
        !fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE,
                                       exact_scope(input.catalog()));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code);
    }
    auto result = ports.resources_->create_installation(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code);
    }
    return OwnerHttpResult{
        200, json_response(std::get<wire::RuntimeCreateInstallationResponse>(result))};
  }
  if (path.starts_with("/api/v1/installations/") && path.ends_with("/instances") &&
      method == "POST") {
    const auto prefix = std::string{"/api/v1/installations/"};
    const auto installation = std::string{
        path.substr(prefix.size(), path.size() - prefix.size() - std::string{"/instances"}.size())};
    wire::RuntimeCreateInstanceRequest input;
    if (installation.empty() || installation.find('/') != std::string::npos ||
        !json_request(request.body, input) || input.installation_id() != installation ||
        !fill_context(input.mutable_context(), request_id, trace_id, request)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    wire::ResourceRef parent;
    parent.set_kind(wire::RESOURCE_INSTALLATION);
    parent.set_id(installation);
    auto delegation = context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE,
                                       exact_scope(parent));
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.error(failure->code);
    }
    if (!ports.resources_) {
      return context.error(wire::UPSTREAM_UNAVAILABLE, true);
    }
    auto result = ports.resources_->create_instance(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.error(failure->code);
    }
    auto value =
        json_request_value(json_response(std::get<wire::RuntimeCreateInstanceResponse>(result)));
    value.erase("bootstrap_credential");
    return OwnerHttpResult{200, value.dump()};
  }
  if (path.starts_with("/api/v1/installations/")) {
    const auto rest = path.substr(std::string{"/api/v1/installations/"}.size());
    const auto first = rest.find("/instances/");
    if (first != std::string_view::npos) {
      const auto installation = std::string{rest.substr(0, first)};
      const auto tail = rest.substr(first + std::string{"/instances/"}.size());
      const auto slash = tail.find('/');
      if (slash != std::string_view::npos) {
        const auto instance = std::string{tail.substr(0, slash)};
        const auto action = tail.substr(slash);
        const auto method = request.method == "HEAD" ? std::string{"GET"} : request.method;
        const auto scope = instance_scope(instance);
        if (action == "/plugin-state" && method == "PUT") {
          const auto parsed = Json::parse(request.body, nullptr, false);
          wire::RuntimeSetDesiredStateRequest input;
          if (installation.empty() || instance.empty() || parsed.is_discarded() ||
              !parsed.is_object() || !parsed.contains("installation_id") ||
              !parsed.contains("instance_id") || !parsed.contains("desired") ||
              !parsed["installation_id"].is_string() || !parsed["instance_id"].is_string() ||
              !parsed["desired"].is_string() ||
              parsed["installation_id"].get<std::string>() != installation ||
              parsed["instance_id"].get<std::string>() != instance) {
            return context.error(wire::INVALID_ARGUMENT, false);
          }
          const auto desired_name = parsed["desired"].get<std::string>();
          if (desired_name != "DESIRED_RUNNING" && desired_name != "DESIRED_STOPPED") {
            return context.error(wire::INVALID_ARGUMENT, false);
          }
          input.set_instance_id(instance);
          input.set_desired_state(desired_name == "DESIRED_RUNNING" ? wire::DESIRED_RUNNING
                                                                    : wire::DESIRED_STOPPED);
          if (parsed.contains("expected_version")) {
            if (!parsed["expected_version"].is_string()) {
              return context.error(wire::INVALID_ARGUMENT, false);
            }
            std::uint64_t version = 0;
            const auto& text = parsed["expected_version"].get_ref<const std::string&>();
            const auto converted = std::from_chars(text.data(), text.data() + text.size(), version);
            if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
                version == 0) {
              return context.error(wire::INVALID_ARGUMENT, false);
            }
            input.set_expected_version(version);
          }
          if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
            return context.error(wire::INVALID_ARGUMENT, false);
          }
          auto delegation =
              context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE, scope);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return context.failure(*failure);
          }
          if (!ports.resources_) {
            return context.error(wire::UPSTREAM_UNAVAILABLE, true);
          }
          auto result =
              ports.resources_->set_desired_state(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return context.failure(*failure);
          }
          const auto& response = std::get<wire::RuntimeSetDesiredStateResponse>(result);
          if (response.instance().installation_id() != installation) {
            return context.error(wire::NOT_FOUND, false);
          }
          const auto& state = response.instance().state();
          Json body{{"instance", instance_record_json(response.instance())}};
          body["runtime"] = {{"desired", wire::DesiredState_Name(state.desired())},
                             {"version", std::to_string(state.version())},
                             {"observed_at", json_time(state.observed_at())}};
          if (state.desired_config_version() > 0) {
            body["runtime"]["desired_config_version"] =
                std::to_string(state.desired_config_version());
          }
          Json host{{"registered", state.registered()},
                    {"process_outcome", wire::ExecutionOutcome_Name(state.process_outcome())},
                    {"observed_at", json_time(state.observed_at())}};
          if (!response.instance().host_id().empty()) {
            host["host_id"] = response.instance().host_id();
          }
          body["host"] = std::move(host);
          body["plugin"] = {{"apply_state", instance_apply_state(state)},
                            {"observed_at", json_time(state.observed_at())}};
          if (state.applied_config_version() > 0) {
            body["plugin"]["applied_config_version"] =
                std::to_string(state.applied_config_version());
          }
          if (response.has_operation()) {
            body["operation"] = {{"owner_service", response.operation().owner_service()},
                                 {"operation_id", response.operation().operation_id()}};
          }
          return OwnerHttpResult{200, body.dump()};
        }
        if (action == "/configuration" && (method == "GET" || method == "PUT")) {
          const auto permission = method == "GET" ? wire::PERMISSION_PLUGIN_CONFIG_READ
                                                  : wire::PERMISSION_PLUGIN_MANAGE;
          auto delegation = context.delegate(ports.delegation_, "runtime", permission, scope);
          if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
            return context.failure(*failure);
          }
          if (method == "GET") {
            wire::RuntimeGetInstanceConfigRequest input;
            input.mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
            input.mutable_resource()->set_id(instance);
            if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
              return context.error(wire::INVALID_ARGUMENT, false);
            }
            if (!ports.resources_) {
              return context.error(wire::UPSTREAM_UNAVAILABLE, true);
            }
            auto result =
                ports.resources_->get_instance_config(input, std::get<std::string>(delegation));
            if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
              return context.failure(*failure);
            }
            const auto& config = std::get<wire::RuntimeGetInstanceConfigResponse>(result).config();
            Json runtime{{"desired_config_version", std::to_string(config.desired_version())},
                         {"apply_state", wire::ConfigApplyState_Name(config.apply_state())},
                         {"observed_at", json_time(config.observed_at())}};
            if (!config.delivery_id().empty()) {
              runtime["delivery_id"] = config.delivery_id();
            }
            std::string values;
            google::protobuf::util::JsonPrintOptions options;
            options.preserve_proto_field_names = true;
            if (google::protobuf::util::MessageToJsonString(config.values(), &values, options)
                    .ok()) {
              runtime["configuration"] = json_request_value(values);
            }
            Json host{{"registered", false},
                      {"process_outcome", "EXECUTION_UNKNOWN"},
                      {"observed_at", json_time(config.observed_at())}};
            Json plugin{{"apply_state", wire::ConfigApplyState_Name(config.apply_state())},
                        {"observed_at", json_time(config.observed_at())}};
            if (config.applied_version() > 0) {
              plugin["applied_config_version"] = std::to_string(config.applied_version());
            }
            Json body{{"runtime", std::move(runtime)},
                      {"host", std::move(host)},
                      {"plugin", std::move(plugin)}};
            if (config.has_error()) {
              body["plugin"]["error"] = {{"code", wire::ErrorCode_Name(config.error().code())}};
            }
            return OwnerHttpResult{200, body.dump()};
          }
          const auto parsed = Json::parse(request.body, nullptr, false);
          wire::RuntimeUpdateConfigRequest input;
          if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("configuration") ||
              !parsed["configuration"].is_object() || !parsed.contains("expected_version") ||
              !parsed["expected_version"].is_string()) {
            return context.error(wire::INVALID_ARGUMENT, false);
          }
          std::uint64_t version = 0;
          const auto& version_text = parsed["expected_version"].get_ref<const std::string&>();
          const auto converted = std::from_chars(
              version_text.data(), version_text.data() + version_text.size(), version);
          if (converted.ec != std::errc{} ||
              converted.ptr != version_text.data() + version_text.size() || version == 0 ||
              !google::protobuf::util::JsonStringToMessage(parsed["configuration"].dump(),
                                                           input.mutable_config())
                   .ok()) {
            return context.error(wire::INVALID_ARGUMENT, false);
          }
          input.mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
          input.mutable_resource()->set_id(instance);
          input.set_expected_version(version);
          if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
            return context.error(wire::INVALID_ARGUMENT, false);
          }
          if (!ports.resources_) {
            return context.error(wire::UPSTREAM_UNAVAILABLE, true);
          }
          auto result = ports.resources_->update_config(input, std::get<std::string>(delegation));
          if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
            return context.failure(*failure);
          }
          const auto& response = std::get<wire::RuntimeUpdateConfigResponse>(result);
          std::string values;
          google::protobuf::util::JsonPrintOptions options;
          options.preserve_proto_field_names = true;
          const auto printed =
              google::protobuf::util::MessageToJsonString(input.config(), &values, options);
          Json runtime{{"desired_config_version", std::to_string(response.state().version())},
                       {"apply_state", "CONFIG_APPLY_PENDING"},
                       {"observed_at", json_time(response.state().observed_at())}};
          if (printed.ok()) {
            runtime["configuration"] = json_request_value(values);
          }
          Json body{{"runtime", std::move(runtime)},
                    {"host",
                     {{"registered", false},
                      {"process_outcome", "EXECUTION_UNKNOWN"},
                      {"observed_at", json_time(response.state().observed_at())}}},
                    {"plugin",
                     {{"apply_state", "CONFIG_APPLY_PENDING"},
                      {"observed_at", json_time(response.state().observed_at())}}}};
          if (response.has_operation()) {
            body["operation"] = {{"owner_service", response.operation().owner_service()},
                                 {"operation_id", response.operation().operation_id()}};
          }
          return OwnerHttpResult{200, body.dump()};
        }
        return context.error(wire::NOT_FOUND);
      }
    }
  }
  if (path.starts_with(kInstances)) {
    const auto tail = path.substr(kInstances.size());
    const auto slash = tail.find('/');
    if (slash == std::string_view::npos) {
      return context.error(wire::NOT_FOUND);
    }
    const auto instance = std::string{tail.substr(0, slash)};
    const auto action = tail.substr(slash);
    if (instance.empty() || action == "/desired-state" || action == "/config") {
      const auto scope = instance_scope(instance);
      if (action == "/desired-state" && method == "PUT") {
        wire::RuntimeSetDesiredStateRequest input;
        if (!json_request(request.body, input) || input.instance_id() != instance ||
            !input.has_expected_version() ||
            !fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return context.error(wire::INVALID_ARGUMENT);
        }
        auto delegation =
            context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE, scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return context.error(failure->code);
        }
        auto result = ports.resources_->set_desired_state(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.error(failure->code);
        }
        return OwnerHttpResult{
            200, json_response(std::get<wire::RuntimeSetDesiredStateResponse>(result))};
      }
      if (action == "/config" && method == "PUT") {
        wire::RuntimeUpdateConfigRequest input;
        if (!json_request(request.body, input) || !input.has_resource() ||
            input.resource().id() != instance || !input.has_expected_version() ||
            !fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return context.error(wire::INVALID_ARGUMENT);
        }
        auto delegation =
            context.delegate(ports.delegation_, "runtime", wire::PERMISSION_PLUGIN_MANAGE, scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return context.error(failure->code);
        }
        auto result = ports.resources_->update_config(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.error(failure->code);
        }
        return OwnerHttpResult{200,
                               json_response(std::get<wire::RuntimeUpdateConfigResponse>(result))};
      }
      if (action == "/config" && method == "GET") {
        wire::RuntimeGetInstanceConfigRequest input;
        input.mutable_resource()->set_kind(wire::RESOURCE_INSTANCE);
        input.mutable_resource()->set_id(instance);
        if (!fill_context(input.mutable_context(), request_id, trace_id, request)) {
          return context.error(wire::INVALID_ARGUMENT);
        }
        auto delegation = context.delegate(ports.delegation_, "runtime",
                                           wire::PERMISSION_PLUGIN_CONFIG_READ, scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return context.error(failure->code);
        }
        auto result =
            ports.resources_->get_instance_config(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.error(failure->code);
        }
        return OwnerHttpResult{
            200, json_response(std::get<wire::RuntimeGetInstanceConfigResponse>(result))};
      }
    }
    return context.error(wire::NOT_FOUND);
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
