#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
Json model_page_json(const wire::ModelListModelMetadataResponse& response) {
  // OpenAPI ModelPage(GenericResource items + PageInfo) instead of the raw proto
  // {"models":[...]} shape. Only the documented display fields are exposed.
  Json items = Json::array();
  for (const auto& model : response.models()) {
    Json item{{"id", model.model_id()}};
    item["name"] = model.alias().empty() ? model.model_id() : model.alias();
    item["state"] = wire::LifecycleState_Name(model.state());
    items.push_back(std::move(item));
  }
  Json page{{"page_size", response.page().page_size()}};
  if (!response.page().next_page_token().empty()) {
    page["next_page_token"] = response.page().next_page_token();
  }
  return {{"items", std::move(items)}, {"page", std::move(page)}};
}
std::string uint64_text(std::uint64_t value) {
  return std::to_string(value);
}
Json error_json(const wire::Error& error) {
  Json value{{"code", wire::ErrorCode_Name(error.code())},
             {"message", error.message()},
             {"details", Json::object()},
             {"request_id", error.request_id()},
             {"trace_id", error.trace_id()},
             {"retryable", error.retryable()}};
  if (value["request_id"].get<std::string>().empty()) {
    value["request_id"] = "control-request";
  }
  if (value["trace_id"].get<std::string>().empty()) {
    value["trace_id"] = "control-trace";
  }
  if (value["message"].get<std::string>().empty()) {
    value["message"] = wire::ErrorCode_Name(error.code());
  }
  return value;
}
Json model_metadata_json(const wire::ModelMetadata& value) {
  return Json{{"model_id", value.model_id()},
              {"alias", value.alias()},
              {"config_version", uint64_text(value.config_version())},
              {"state", wire::LifecycleState_Name(value.state())},
              {"provider_kind", value.provider_kind()}};
}
Json model_configuration_json(const wire::ModelConfiguration& value) {
  return Json{{"model_id", value.model_id()},
              {"alias", value.alias()},
              {"provider_kind", value.provider_kind()},
              {"endpoint", value.endpoint()},
              {"provider_model", value.provider_model()},
              {"timeout_ms", value.timeout_ms()},
              {"version", uint64_text(value.version())}};
}
Json operation_ref_json(const wire::OperationRef& value) {
  Json ref{{"owner_service", value.owner_service()}, {"operation_id", value.operation_id()}};
  if (value.has_resource()) {
    ref["resource"] = Json{{"kind", wire::ResourceKind_Name(value.resource().kind())},
                           {"id", value.resource().id()}};
  }
  return ref;
}
Json call_record_json(const wire::ModelCallRecord& value) {
  Json record;
  record["logical_call_id"] = value.logical_call_id();
  record["owner_instance_id"] = value.owner_instance_id();
  record["model_id"] = value.model_id();
  record["config_version"] = uint64_text(value.config_version());
  record["state"] = wire::OperationState_Name(value.state());
  record["execution_outcome"] = wire::ExecutionOutcome_Name(value.execution_outcome());
  record["input_tokens"] = uint64_text(value.input_tokens());
  record["output_tokens"] = uint64_text(value.output_tokens());
  if (value.has_observed_at()) {
    record["observed_at"] = json_time(value.observed_at());
  }
  if (value.has_reason()) {
    record["reason"] = error_json(value.reason());
  }
  record["secret_version"] = uint64_text(value.secret_version());
  record["usage_known"] = value.usage_known();
  record["may_have_billed"] = value.may_have_billed();
  record["provider_request_id"] = value.provider_request_id();
  record["latency_ms"] = value.latency_ms();
  record["disposition"] = wire::ModelCallDisposition_Name(value.disposition());
  return record;
}
// P11d model REST row: returns the exact single model resource scope used for the one-shot
// delegation. The model id comes from the path; a query scope must name the same model and nothing
// else, so Control never widens to a role or another model.
std::optional<wire::ResourceScope>
single_model_scope(const std::map<std::string, std::string>& query, std::string_view model_id,
                   bool scope_required) {
  const auto found = query.find("scope");
  if (found == query.end()) {
    if (scope_required) {
      return std::nullopt;
    }
    wire::ResourceScope scope;
    auto* resource = scope.add_resources();
    resource->set_kind(wire::RESOURCE_MODEL);
    resource->set_id(std::string{model_id});
    return scope;
  }
  wire::ResourceScope scope;
  if (!google::protobuf::util::JsonStringToMessage(found->second, &scope).ok() ||
      scope.resources_size() != 1 || scope.resources(0).kind() != wire::RESOURCE_MODEL ||
      scope.resources(0).id() != model_id) {
    return std::nullopt;
  }
  return scope;
}
std::optional<std::uint64_t> parse_uint64_json(const nlohmann::json& value) {
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_string()) {
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 20) {
      return std::nullopt;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return parsed;
  }
  return std::nullopt;
}

} // namespace

std::optional<OwnerHttpResult> handle_models(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  const auto& trace_id = context.trace_id_;
  if (path == "/api/v1/models") {
    if (!ports.model_) {
      return context.error(wire::CAPABILITY_NOT_SUPPORTED, false);
    }
    if (request.method != "GET" && request.method != "HEAD") {
      return context.method_not_allowed();
    }
    if (request.access_token.empty()) {
      return context.error(wire::UNAUTHENTICATED, false);
    }
    auto query = parse_query(request.target);
    auto scope = query ? parse_scope(*query) : std::nullopt;
    if (!query || !scope || scope->resources_size() < 1) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    for (const auto& resource : scope->resources()) {
      if (resource.kind() != wire::RESOURCE_MODEL || resource.id().empty()) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
    }
    wire::ModelListModelMetadataRequest input;
    fill_context(input.mutable_context(), request, request_id, trace_id);
    *input.mutable_scope() = *scope;
    if (!fill_page(input, *query, 1, 100)) {
      return context.error(wire::INVALID_ARGUMENT);
    }
    auto delegation =
        context.delegate(ports.delegation_, "model", wire::PERMISSION_MODEL_READ, *scope);
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    auto result = ports.model_->list_metadata(input, std::get<std::string>(delegation));
    if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
      return context.failure(*failure);
    }
    return OwnerHttpResult{
        200, model_page_json(std::get<wire::ModelListModelMetadataResponse>(result)).dump()};
  }
  constexpr std::string_view kModels = "/api/v1/models/";
  if (path.starts_with(kModels)) {
    if (!ports.model_) {
      return context.error(wire::CAPABILITY_NOT_SUPPORTED, false);
    }
    auto query = parse_query(request.target);
    if (!query) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    const auto rest = path.substr(kModels.size());
    const auto first = rest.find('/');
    const auto model_id = rest.substr(0, first);
    if (model_id.empty()) {
      return context.error(wire::INVALID_ARGUMENT, false);
    }
    const auto tail = first == std::string_view::npos ? std::string_view{} : rest.substr(first + 1);
    const auto model_scope = [&]() -> std::optional<wire::ResourceScope> {
      wire::ResourceScope scope;
      auto* resource = scope.add_resources();
      resource->set_kind(wire::RESOURCE_MODEL);
      resource->set_id(std::string{model_id});
      return scope;
    }();
    const auto idempotency = header(request, "idempotency-key");
    const auto require_idempotency = [&]() -> std::optional<OwnerHttpResult> {
      if (!idempotency || idempotency->empty()) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      return std::nullopt;
    };
    const auto dispatch = [&](wire::Permission permission,
                              const wire::ResourceScope& scope) -> OwnerOutcome<std::string> {
      return context.delegate(ports.delegation_, "model", permission, scope);
    };
    const auto invalid = [&]() -> OwnerHttpResult {
      return context.error(wire::INVALID_ARGUMENT, false);
    };
    if (tail.empty() && (request.method == "GET" || request.method == "HEAD")) {
      auto scope = single_model_scope(*query, model_id, true);
      if (!scope) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      auto delegation = dispatch(wire::PERMISSION_MODEL_READ, *scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      wire::ModelGetModelMetadataRequest input;
      fill_context(input.mutable_context(), request, request_id, trace_id);
      input.set_model_id(std::string{model_id});
      *input.mutable_scope() = *scope;
      auto result = ports.model_->metadata(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      const auto& response = std::get<wire::ModelGetModelMetadataResponse>(result);
      if (!response.has_model()) {
        return context.error(wire::NOT_FOUND, false);
      }
      google::protobuf::util::JsonPrintOptions options;
      options.preserve_proto_field_names = true;
      wire::ModelGetModelMetadataResponse body;
      *body.mutable_model() = response.model();
      std::string encoded;
      if (!google::protobuf::util::MessageToJsonString(body, &encoded, options).ok()) {
        return context.error(wire::INTERNAL, false);
      }
      return OwnerHttpResult{200, encoded};
    }
    if (tail.empty() && request.method == "PUT") {
      if (const auto missing = require_idempotency()) {
        return *missing;
      }
      wire::ModelUpdateModelRequest input;
      if (!google::protobuf::util::JsonStringToMessage(request.body, &input).ok() ||
          !input.has_config() || input.config().model_id() != model_id) {
        return invalid();
      }
      fill_context(input.mutable_context(), request, request_id, trace_id);
      input.mutable_context()->set_idempotency_key(*idempotency);
      auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *model_scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      auto result = ports.model_->update(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      const auto& response = std::get<wire::ModelUpdateModelResponse>(result);
      Json body;
      if (response.has_model()) {
        body["model"] = model_metadata_json(response.model());
      }
      if (response.has_operation()) {
        body["operation"] = operation_ref_json(response.operation());
      }
      return OwnerHttpResult{200, body.dump()};
    }
    const auto lifecycle = [&](bool enable) -> std::optional<OwnerHttpResult> {
      if (request.method != "POST") {
        return std::nullopt;
      }
      if (const auto missing = require_idempotency()) {
        return *missing;
      }
      std::optional<std::uint64_t> expected;
      if (!request.body.empty()) {
        const auto parsed = Json::parse(request.body, nullptr, false);
        if (!parsed.is_object() || (parsed.contains("expected_version") &&
                                    !(expected = parse_uint64_json(parsed["expected_version"])))) {
          return invalid();
        }
      }
      if (!expected) {
        return invalid();
      }
      auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *model_scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      const auto proof = std::get<std::string>(delegation);
      Json body;
      if (enable) {
        wire::ModelEnableModelRequest input;
        fill_context(input.mutable_context(), request, request_id, trace_id);
        input.mutable_context()->set_idempotency_key(*idempotency);
        input.set_model_id(std::string{model_id});
        input.set_expected_version(*expected);
        auto result = ports.model_->enable(input, proof);
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.failure(*failure);
        }
        const auto& response = std::get<wire::ModelEnableModelResponse>(result);
        if (response.has_model()) {
          body["model"] = model_metadata_json(response.model());
        }
      } else {
        wire::ModelDisableModelRequest input;
        fill_context(input.mutable_context(), request, request_id, trace_id);
        input.mutable_context()->set_idempotency_key(*idempotency);
        input.set_model_id(std::string{model_id});
        input.set_expected_version(*expected);
        auto result = ports.model_->disable(input, proof);
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.failure(*failure);
        }
        const auto& response = std::get<wire::ModelDisableModelResponse>(result);
        if (response.has_model()) {
          body["model"] = model_metadata_json(response.model());
        }
      }
      return OwnerHttpResult{200, body.dump()};
    };
    if (tail == "disable" || tail == "enable") {
      if (const auto handled = lifecycle(tail == "enable")) {
        return *handled;
      }
      return context.method_not_allowed();
    }
    if (tail == "secret" && request.method == "PUT") {
      if (const auto missing = require_idempotency()) {
        return *missing;
      }
      wire::ModelRotateProviderSecretRequest input;
      if (!google::protobuf::util::JsonStringToMessage(request.body, &input).ok() ||
          input.model_id() != model_id) {
        return invalid();
      }
      fill_context(input.mutable_context(), request, request_id, trace_id);
      input.mutable_context()->set_idempotency_key(*idempotency);
      auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *model_scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      auto result = ports.model_->rotate_secret(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      const auto& response = std::get<wire::ModelRotateProviderSecretResponse>(result);
      Json body{{"version", uint64_text(response.version())}};
      if (response.has_operation()) {
        body["operation"] = operation_ref_json(response.operation());
      }
      return OwnerHttpResult{200, body.dump()};
    }
    if (tail == "configuration" && request.method == "GET") {
      auto scope = single_model_scope(*query, model_id, true);
      if (!scope) {
        return invalid();
      }
      auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *scope);
      if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
        return context.failure(*failure);
      }
      wire::ModelGetModelConfigurationRequest input;
      fill_context(input.mutable_context(), request, request_id, trace_id);
      input.set_model_id(std::string{model_id});
      *input.mutable_scope() = *scope;
      auto result = ports.model_->configuration(input, std::get<std::string>(delegation));
      if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
        return context.failure(*failure);
      }
      const auto& response = std::get<wire::ModelGetModelConfigurationResponse>(result);
      Json body;
      if (response.has_config()) {
        body["config"] = model_configuration_json(response.config());
      }
      body["secret_configured"] = response.secret_configured();
      body["secret_version"] = uint64_text(response.secret_version());
      if (response.has_observed_at()) {
        body["observed_at"] = json_time(response.observed_at());
      }
      return OwnerHttpResult{200, body.dump()};
    }
    if (tail.starts_with("calls/")) {
      const auto call_rest = tail.substr(6);
      const auto slash = call_rest.find('/');
      const auto call_id = call_rest.substr(0, slash);
      if (call_id.empty()) {
        return invalid();
      }
      const auto resolve =
          slash != std::string_view::npos && call_rest.substr(slash + 1) == "resolve";
      if (slash != std::string_view::npos && !resolve) {
        return context.error(wire::INVALID_ARGUMENT, false);
      }
      if (!resolve && request.method == "GET") {
        auto scope = single_model_scope(*query, model_id, true);
        if (!scope) {
          return invalid();
        }
        auto delegation = dispatch(wire::PERMISSION_MODEL_CALL_INSPECT, *scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return context.failure(*failure);
        }
        wire::ModelGetCallStatusRequest input;
        fill_context(input.mutable_context(), request, request_id, trace_id);
        input.set_logical_call_id(std::string{call_id});
        *input.mutable_scope() = *scope;
        auto result = ports.model_->call_status(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.failure(*failure);
        }
        const auto& response = std::get<wire::ModelGetCallStatusResponse>(result);
        if (!response.has_call()) {
          return context.error(wire::NOT_FOUND, false);
        }
        return OwnerHttpResult{200, Json{{"call", call_record_json(response.call())}}.dump()};
      }
      if (resolve && request.method == "POST") {
        if (const auto missing = require_idempotency()) {
          return *missing;
        }
        const auto parsed = Json::parse(request.body, nullptr, false);
        if (!parsed.is_object()) {
          return invalid();
        }
        wire::ModelResolveModelCallRequest input;
        fill_context(input.mutable_context(), request, request_id, trace_id);
        input.mutable_context()->set_idempotency_key(*idempotency);
        input.set_logical_call_id(std::string{call_id});
        if (parsed.contains("expected_version")) {
          const auto expected = parse_uint64_json(parsed["expected_version"]);
          if (!expected) {
            return invalid();
          }
          input.set_expected_version(*expected);
        }
        if (!parsed.contains("disposition") || !parsed["disposition"].is_string()) {
          return invalid();
        }
        wire::ModelCallDisposition disposition = wire::MODEL_CALL_DISPOSITION_UNSPECIFIED;
        if (!wire::ModelCallDisposition_Parse(parsed["disposition"].get<std::string>(),
                                              &disposition) ||
            disposition == wire::MODEL_CALL_DISPOSITION_UNSPECIFIED) {
          return invalid();
        }
        input.set_disposition(disposition);
        // Resolve requires the exact model scope just like the read path; absent scope query
        // derives it from the path, never from role membership.
        auto scope = single_model_scope(*query, model_id, false);
        if (!scope) {
          return invalid();
        }
        auto delegation = dispatch(wire::PERMISSION_MODEL_MANAGE, *scope);
        if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
          return context.failure(*failure);
        }
        auto result = ports.model_->resolve(input, std::get<std::string>(delegation));
        if (const auto* failure = std::get_if<OwnerFailure>(&result)) {
          return context.failure(*failure);
        }
        const auto& response = std::get<wire::ModelResolveModelCallResponse>(result);
        if (!response.has_call()) {
          return context.error(wire::NOT_FOUND, false);
        }
        return OwnerHttpResult{200, Json{{"call", call_record_json(response.call())}}.dump()};
      }
      return context.method_not_allowed();
    }
    // Metadata/List paths keep the P12c explicit-unavailable behavior until their Control route
    // is wired; never fall through to a fabricated empty page.
    return context.error(wire::CAPABILITY_NOT_SUPPORTED, false);
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
