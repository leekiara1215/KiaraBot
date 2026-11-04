#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>

#include "kiarabot/control/target/owner_gateway.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace wire = kiarabot::v1;
namespace codec = core::platform::target_codec;
using Json = nlohmann::json;
using Query = std::map<std::string, std::string>;

// Non-owning ports supplied by the composition root. No handler owns business state.
struct Ports {
  DelegationPort& delegation_;
  RuntimeActionsPort& runtime_;
  IdentityPrincipalsPort* principals_;
  HealthPort* health_;
  RuntimeResourcesPort* resources_;
  SchedulerTasksPort* tasks_;
  OperationPort* operations_;
  ModelManagementPort* model_;
  AuditLogSourcePort* audit_logs_;
};

// Created once per request; handlers preserve the original bearer, deadline and identifiers.
struct RequestContext {
  const OwnerHttpRequest& request_;
  std::string_view path_;
  std::string request_id_;
  std::string trace_id_;

  OwnerHttpResult error(wire::ErrorCode code, bool retryable = false) const;
  OwnerHttpResult failure(OwnerFailure failure) const;
  OwnerHttpResult method_not_allowed() const;
  OwnerOutcome<std::string> delegate(DelegationPort& port, std::string_view audience,
                                     wire::Permission permission,
                                     const wire::ResourceScope& scope) const;
};

std::string random_identifier();
std::string request_identifier(const OwnerHttpRequest& request);
std::string trace_identifier(const OwnerHttpRequest& request);
std::optional<std::string> header(const OwnerHttpRequest& request, std::string_view name);
std::optional<Query> parse_query(std::string_view target);
std::optional<wire::ResourceScope> parse_scope(const Query& query);
std::optional<wire::ResourceScope> optional_scope(const Query& query);
bool catalog_uuid(std::string_view value);
bool valid_catalog(const wire::ResourceRef& resource);
wire::ResourceScope exact_scope(const wire::ResourceRef& resource);
wire::ResourceScope control_catalog_scope();
wire::ResourceScope account_scope(std::string_view id);
wire::ResourceScope instance_scope(std::string_view id);
std::string json_time(const google::protobuf::Timestamp& value);
Json json_request_value(const std::string& body);
bool fill_context(wire::RequestContext* context, std::string_view request_id,
                  std::string_view trace_id, const OwnerHttpRequest& request);
void fill_context(wire::RequestContext* context, const OwnerHttpRequest& request,
                  const std::string& request_id, const std::string& trace_id);

template <typename Message> bool json_request(const std::string& body, Message& value) {
  return !body.empty() && google::protobuf::util::JsonStringToMessage(body, &value).ok();
}
template <typename Message> std::string json_response(const Message& value) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string body;
  if (!google::protobuf::util::MessageToJsonString(value, &body, options).ok()) {
    return "{}";
  }
  return body;
}

// Keep owner-specific page-size limits explicit. The page submessage is created only when
// the original request supplied a page field; an absent page remains absent.
template <typename Request>
bool fill_page(Request& input, const Query& query, std::uint32_t minimum,
               std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max()) {
  if (const auto size = query.find("page_size"); size != query.end()) {
    std::uint32_t value = 0;
    const auto& raw = size->second;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() || value < minimum ||
        value > maximum) {
      return false;
    }
    input.mutable_page()->set_page_size(value);
  }
  if (const auto token = query.find("page_token"); token != query.end()) {
    input.mutable_page()->set_page_token(token->second);
  }
  return true;
}

using Handler = std::optional<OwnerHttpResult> (*)(const RequestContext&, const Ports&);
std::optional<OwnerHttpResult> handle_health(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_principals(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_catalogs(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_operations(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_accounts(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_instances(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_tasks(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_audit_logs(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_lifecycle(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_models(const RequestContext& context, const Ports& ports);
std::optional<OwnerHttpResult> handle_actions(const RequestContext& context, const Ports& ports);

} // namespace kiarabot::control::target::owner_gateway
