#include <algorithm>
#include <array>
#include <cstddef>

#include <google/protobuf/util/time_util.h>
#include <openssl/rand.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
std::string random_identifier() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return "00000000-0000-4000-8000-000000000000";
  }
  // Response request/trace ids are forwarded into owner RPC RequestContexts; every owner RPC
  // boundary requires canonical UUIDv4 and rejects raw 32-hex tokens with INVALID_ARGUMENT
  // before the owner use case runs (P14k native single-host finding).
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  constexpr char kHex[] = "0123456789abcdef";
  std::string value;
  value.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      value.push_back('-');
    }
    value.push_back(kHex[bytes[index] >> 4U]);
    value.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return value;
}
std::optional<std::string> header(const OwnerHttpRequest& request, std::string_view name) {
  for (const auto& [key, value] : request.headers) {
    if (key.size() == name.size() &&
        std::equal(key.begin(), key.end(), name.begin(), [](char left, char right) {
          const auto lower = [](char value) {
            return static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value);
          };
          return lower(left) == lower(right);
        })) {
      return value;
    }
  }
  return std::nullopt;
}
std::optional<std::string> percent_decode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '%') {
      if (index + 2 >= value.size()) {
        return std::nullopt;
      }
      unsigned number = 0;
      const auto parsed =
          std::from_chars(value.data() + index + 1, value.data() + index + 3, number, 16);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + index + 3 || number < 32 ||
          number >= 127) {
        return std::nullopt;
      }
      result.push_back(static_cast<char>(number));
      index += 2;
    } else {
      result.push_back(value[index] == '+' ? ' ' : value[index]);
    }
  }
  return result;
}
std::optional<std::map<std::string, std::string>> parse_query(std::string_view target) {
  std::map<std::string, std::string> query;
  const auto separator = target.find('?');
  if (separator == std::string_view::npos) {
    return query;
  }
  auto rest = target.substr(separator + 1);
  while (!rest.empty()) {
    const auto end = rest.find('&');
    const auto part = rest.substr(0, end);
    const auto equal = part.find('=');
    if (equal == std::string_view::npos) {
      return std::nullopt;
    }
    const auto name = percent_decode(part.substr(0, equal));
    const auto value = percent_decode(part.substr(equal + 1));
    if (!name || !value || !query.emplace(*name, *value).second) {
      return std::nullopt;
    }
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
  }
  return query;
}
std::optional<wire::ResourceScope> parse_scope(const std::map<std::string, std::string>& query) {
  const auto found = query.find("scope");
  if (found == query.end()) {
    return std::nullopt;
  }
  wire::ResourceScope scope;
  if (!google::protobuf::util::JsonStringToMessage(found->second, &scope).ok() ||
      scope.resources_size() != 1) {
    return std::nullopt;
  }
  return scope;
}
bool catalog_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto character = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (character != '-') {
        return false;
      }
    } else if (!((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}
bool valid_catalog(const wire::ResourceRef& resource) {
  return resource.kind() == wire::RESOURCE_CATALOG && catalog_uuid(resource.id());
}
wire::ResourceScope exact_scope(const wire::ResourceRef& resource) {
  wire::ResourceScope scope;
  *scope.add_resources() = resource;
  return scope;
}
std::string json_time(const google::protobuf::Timestamp& value) {
  auto result = google::protobuf::util::TimeUtil::ToString(value);
  if (!result.empty() && result.back() != 'Z' && result.find('+') == std::string::npos) {
    result += 'Z';
  }
  return result;
}
Json error_json(wire::ErrorCode code, std::string_view request_id, std::string_view trace_id,
                bool retryable) {
  return {{"code", wire::ErrorCode_Name(code)},
          {"message", wire::ErrorCode_Name(code)},
          {"request_id", std::string{request_id}},
          {"trace_id", std::string{trace_id}},
          {"retryable", retryable},
          {"details", Json::object()}};
}
wire::ResourceScope control_catalog_scope() {
  wire::ResourceScope scope;
  auto* resource = scope.add_resources();
  resource->set_kind(wire::RESOURCE_CATALOG);
  resource->set_id("control");
  return scope;
}
std::string request_identifier(const OwnerHttpRequest& request) {
  if (const auto value = header(request, "x-request-id");
      value && value->size() >= 8 && value->size() <= 128) {
    return *value;
  }
  return random_identifier();
}
std::string trace_identifier(const OwnerHttpRequest& request) {
  if (const auto value = header(request, "traceparent");
      value && value->size() >= 55 && value->substr(0, 3) == "00-") {
    return value->substr(3, 32);
  }
  return random_identifier();
}
Json json_request_value(const std::string& body) {
  auto parsed = Json::parse(body, nullptr, false);
  return parsed.is_object() ? parsed : Json::object();
}
wire::ResourceScope account_scope(std::string_view id) {
  wire::ResourceScope scope;
  auto* resource = scope.add_resources();
  resource->set_kind(wire::RESOURCE_ACCOUNT);
  resource->set_id(std::string{id});
  return scope;
}
wire::ResourceScope instance_scope(std::string_view id) {
  wire::ResourceScope scope;
  auto* resource = scope.add_resources();
  resource->set_kind(wire::RESOURCE_INSTANCE);
  resource->set_id(std::string{id});
  return scope;
}
bool fill_context(wire::RequestContext* context, std::string_view request_id,
                  std::string_view trace_id, const OwnerHttpRequest& request) {
  context->set_request_id(std::string{request_id});
  context->set_trace_id(std::string{trace_id});
  if (request.method != "GET" && request.method != "HEAD") {
    const auto key = header(request, "idempotency-key");
    if (!key || key->empty() || key->size() > 128) {
      return false;
    }
    context->set_idempotency_key(*key);
  }
  const auto deadline = codec::to_wire(request.deadline);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&deadline)) {
    *context->mutable_deadline() = *timestamp;
    return true;
  }
  return false;
}
std::optional<wire::ResourceScope> optional_scope(const std::map<std::string, std::string>& query) {
  const auto found = query.find("scope");
  if (found == query.end()) {
    return wire::ResourceScope{};
  }
  wire::ResourceScope scope;
  if (!google::protobuf::util::JsonStringToMessage(found->second, &scope).ok()) {
    return std::nullopt;
  }
  return scope;
}
void fill_context(wire::RequestContext* context, const OwnerHttpRequest& request,
                  const std::string& request_id, const std::string& trace_id) {
  context->set_request_id(request_id);
  context->set_trace_id(trace_id);
  const auto deadline = codec::to_wire(request.deadline);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&deadline)) {
    *context->mutable_deadline() = *timestamp;
  }
}
OwnerHttpResult RequestContext::error(wire::ErrorCode code, bool retryable) const {
  return {owner_http_status(code), owner_error_body(code, request_id_, trace_id_, retryable)};
}
OwnerHttpResult RequestContext::failure(OwnerFailure failure) const {
  return error(failure.code, failure.retryable);
}
OwnerHttpResult RequestContext::method_not_allowed() const {
  return {405, owner_error_body(wire::INVALID_ARGUMENT, request_id_, trace_id_, false)};
}
OwnerOutcome<std::string> RequestContext::delegate(DelegationPort& port, std::string_view audience,
                                                   wire::Permission permission,
                                                   const wire::ResourceScope& scope) const {
  return port.issue(audience, permission, scope, request_.access_token, request_.deadline);
}

} // namespace kiarabot::control::target::owner_gateway

namespace kiarabot::control::target {
namespace wire = kiarabot::v1;
unsigned int owner_http_status(wire::ErrorCode code) {
  switch (code) {
  case wire::INVALID_ARGUMENT:
    return 400;
  case wire::UNAUTHENTICATED:
    return 401;
  case wire::PERMISSION_DENIED:
    return 403;
  case wire::NOT_FOUND:
    return 404;
  case wire::CONFLICT:
    return 409;
  case wire::RATE_LIMITED:
    return 429;
  case wire::TIMEOUT:
    return 504;
  case wire::UPSTREAM_UNAVAILABLE:
  case wire::PLUGIN_UNAVAILABLE:
  case wire::CAPABILITY_NOT_SUPPORTED:
    return 503;
  default:
    return 500;
  }
}

std::string owner_error_body(wire::ErrorCode code, std::string_view request_id,
                             std::string_view trace_id, bool retryable) {
  return owner_gateway::error_json(code, request_id, trace_id, retryable).dump();
}

} // namespace kiarabot::control::target
