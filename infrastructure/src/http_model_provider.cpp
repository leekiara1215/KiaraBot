#include "kiarabot/infrastructure/http_model_provider.hpp"

#include <cmath>
#include <limits>

#include <curl/curl.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

namespace kiarabot::infrastructure {
namespace {
namespace wire = kiarabot::v1;
using Json = google::protobuf::Struct;
wire::Error failure(wire::ErrorCode code) {
  wire::Error error;
  error.set_code(code);
  error.set_message(wire::ErrorCode_Name(code));
  error.set_retryable(code == wire::RATE_LIMITED);
  return error;
}
std::string part(CURLU* url, CURLUPart component) {
  char* raw = nullptr;
  if (curl_url_get(url, component, &raw, 0) != CURLUE_OK) {
    return {};
  }
  const std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
  return raw;
}
bool valid_config(const HttpModelConfig& config) {
  if (config.api_key.empty() || config.api_key.size() > 4096 || config.model.empty() ||
      config.model.size() > 256 || config.endpoint.size() > 2048 ||
      config.endpoint.find('\0') != std::string::npos) {
    return false;
  }
  for (const unsigned char character : config.api_key) {
    if (character <= 32 || character >= 127) {
      return false;
    }
  }
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, config.endpoint.c_str(), 0) != CURLUE_OK) {
    return false;
  }
  const auto scheme = part(url.get(), CURLUPART_SCHEME);
  return (scheme == "https" ||
          (scheme == "http" && part(url.get(), CURLUPART_HOST) == "127.0.0.1")) &&
         part(url.get(), CURLUPART_USER).empty() && part(url.get(), CURLUPART_PASSWORD).empty() &&
         part(url.get(), CURLUPART_QUERY).empty() && part(url.get(), CURLUPART_FRAGMENT).empty();
}
std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
  try {
    auto& body = *static_cast<std::string*>(context);
    if (size != 0 && count > (1048576 - body.size()) / size) {
      return 0;
    }
    body.append(bytes, size * count);
    return size * count;
  } catch (...) {
    return 0;
  }
}
const google::protobuf::Value& field(const Json& object, const std::string& key) {
  static const google::protobuf::Value missing;
  const auto found = object.fields().find(key);
  return found == object.fields().end() ? missing : found->second;
}
std::optional<std::uint64_t> tokens(const Json& usage, const std::string& key) {
  const auto& value = field(usage, key);
  const double count = value.number_value();
  if (value.kind_case() != google::protobuf::Value::kNumberValue || !std::isfinite(count) ||
      count < 0 || count > 9007199254740991.0 || std::floor(count) != count) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(count);
}
} // namespace

HttpModelProvider::HttpModelProvider(HttpModelConfig config) : config_(std::move(config)) {}
core::PortResult<std::shared_ptr<HttpModelProvider>>
HttpModelProvider::open(HttpModelConfig config) try {
  // Process lifetime initialization; no cleanup while another provider may be using curl.
  static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initialized != CURLE_OK) {
    return failure(wire::INTERNAL);
  }
  if (!valid_config(config)) {
    return failure(wire::INVALID_ARGUMENT);
  }
  return std::shared_ptr<HttpModelProvider>(new HttpModelProvider(std::move(config)));
} catch (...) {
  return failure(wire::INTERNAL);
}

core::PortResult<wire::ModelChatResponse>
HttpModelProvider::chat(const wire::ModelChatRequest& request,
                        std::chrono::milliseconds timeout) try {
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return failure(wire::RATE_LIMITED);
  }
  if (timeout.count() <= 0) {
    return failure(wire::TIMEOUT);
  }
  if (request.model() != "default" && request.model() != config_.model) {
    return failure(wire::CAPABILITY_NOT_SUPPORTED);
  }
  if (request.messages().empty() || request.messages_size() > 128 ||
      request.ByteSizeLong() > 65536) {
    return failure(wire::INVALID_ARGUMENT);
  }
  Json payload;
  (*payload.mutable_fields())["model"].set_string_value(config_.model);
  (*payload.mutable_fields())["stream"].set_bool_value(false);
  auto* messages = (*payload.mutable_fields())["messages"].mutable_list_value();
  for (const auto& message : request.messages()) {
    if (message.role() != "system" && message.role() != "user" && message.role() != "assistant") {
      return failure(wire::INVALID_ARGUMENT);
    }
    auto* fields = messages->add_values()->mutable_struct_value()->mutable_fields();
    (*fields)["role"].set_string_value(message.role());
    (*fields)["content"].set_string_value(message.text());
  }
  std::string encoded;
  if (!google::protobuf::util::MessageToJsonString(payload, &encoded).ok()) {
    return failure(wire::INVALID_ARGUMENT);
  }
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                 curl_easy_cleanup);
  if (!curl) {
    return failure(wire::INTERNAL);
  }
  curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
  if (!raw_headers) {
    return failure(wire::INTERNAL);
  }
  auto* appended =
      curl_slist_append(raw_headers, ("Authorization: Bearer " + config_.api_key).c_str());
  if (!appended) {
    curl_slist_free_all(raw_headers);
    return failure(wire::INTERNAL);
  }
  const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(appended,
                                                                            curl_slist_free_all);
  std::string body;
  const long deadline = static_cast<long>(std::min(timeout.count(), std::int64_t{30000}));
  if (curl_easy_setopt(curl.get(), CURLOPT_URL, config_.endpoint.c_str()) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, encoded.c_str()) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(encoded.size())) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, deadline) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_PROXY, "") != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https,http") != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body) != CURLE_OK) {
    return failure(wire::INTERNAL);
  }
  const auto status = curl_easy_perform(curl.get());
  if (status != CURLE_OK) {
    return failure(status == CURLE_OPERATION_TIMEDOUT ? wire::TIMEOUT : wire::UPSTREAM_UNAVAILABLE);
  }
  long http_status = 0;
  if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK ||
      http_status != 200) {
    return failure(http_status == 429 ? wire::RATE_LIMITED : wire::UPSTREAM_UNAVAILABLE);
  }
  Json decoded;
  if (!google::protobuf::util::JsonStringToMessage(body, &decoded).ok()) {
    return failure(wire::UPSTREAM_UNAVAILABLE);
  }
  const auto& choices = field(decoded, "choices").list_value();
  if (choices.values_size() != 1) {
    return failure(wire::UPSTREAM_UNAVAILABLE);
  }
  const auto& message = field(choices.values(0).struct_value(), "message").struct_value();
  const auto& content = field(message, "content");
  if (content.kind_case() != google::protobuf::Value::kStringValue ||
      content.string_value().size() > 65536 || message.fields().contains("tool_calls") ||
      message.fields().contains("function_call")) {
    return failure(wire::UPSTREAM_UNAVAILABLE);
  }
  wire::ModelChatResponse response;
  response.set_text(content.string_value());
  if (decoded.fields().contains("usage")) {
    const auto& usage = field(decoded, "usage").struct_value();
    const auto input = tokens(usage, "prompt_tokens");
    const auto output = tokens(usage, "completion_tokens");
    if (!input || !output) {
      return failure(wire::UPSTREAM_UNAVAILABLE);
    }
    response.set_input_tokens(*input);
    response.set_output_tokens(*output);
  }
  return response;
} catch (...) {
  return failure(wire::INTERNAL);
}
} // namespace kiarabot::infrastructure
