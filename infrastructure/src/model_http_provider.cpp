#include "kiarabot/infrastructure/model_http_provider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "kiarabot/core/domain/target/model_gateway.hpp"

namespace kiarabot::infrastructure {
namespace {
namespace target = core::target;
using Json = google::protobuf::Struct;

constexpr std::size_t kMaximumResponseBytes = target::kModelMaximumProviderResponseBytes;

target::Failure failure(target::ErrorCode code,
                        std::optional<std::chrono::milliseconds> retry_after = std::nullopt) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, retry_after};
}
target::ModelProviderResult
rejected(target::ErrorCode code,
         std::optional<std::chrono::milliseconds> retry_after = std::nullopt) {
  target::ModelProviderResult result;
  result.outcome = target::ModelProviderOutcome::DefiniteFailure;
  result.failure = failure(code, retry_after);
  return result;
}
target::ModelProviderResult unknown(target::ErrorCode code) {
  target::ModelProviderResult result;
  result.outcome = target::ModelProviderOutcome::Unknown;
  result.failure = failure(code);
  return result;
}
// Values with this lifetime may contain a Provider bearer secret; clear before destruction. The
// curl header list itself is freed by the adapter below without logging.
class SecretString final {
public:
  explicit SecretString(std::string value) : value_(std::move(value)) {}
  ~SecretString() {
    volatile char* bytes = value_.data();
    for (std::size_t index = 0; index < value_.size(); ++index) {
      bytes[index] = 0;
    }
  }
  SecretString(const SecretString&) = delete;
  SecretString& operator=(const SecretString&) = delete;
  const std::string& value() const noexcept {
    return value_;
  }

private:
  std::string value_;
};
struct CurlDeleter {
  void operator()(CURL* handle) const noexcept {
    if (handle != nullptr) {
      curl_easy_cleanup(handle);
    }
  }
};
struct HeaderListDeleter {
  void operator()(curl_slist* list) const noexcept {
    if (list != nullptr) {
      curl_slist_free_all(list);
    }
  }
};
struct UrlDeleter {
  void operator()(CURLU* url) const noexcept {
    if (url != nullptr) {
      curl_url_cleanup(url);
    }
  }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;
using HeaderList = std::unique_ptr<curl_slist, HeaderListDeleter>;
using UrlHandle = std::unique_ptr<CURLU, UrlDeleter>;

void initialize_curl() {
  static const auto initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
  if (initialized != CURLE_OK) {
    throw std::runtime_error("curl global initialization failed");
  }
}
std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
  try {
    auto& body = *static_cast<std::string*>(context);
    if (size != 0 && count > (kMaximumResponseBytes - body.size()) / size) {
      return 0;
    }
    body.append(bytes, size * count);
    return size * count;
  } catch (...) {
    return 0;
  }
}
std::size_t receive_header(char* bytes, std::size_t size, std::size_t count,
                           void* context) noexcept {
  try {
    auto& retry_after = *static_cast<std::optional<std::chrono::milliseconds>*>(context);
    const std::string_view line{bytes, size * count};
    constexpr std::string_view kName = "retry-after:";
    if (line.size() >= kName.size() &&
        std::equal(kName.begin(), kName.end(), line.begin(), [](char left, char right) {
          return static_cast<char>(std::tolower(static_cast<unsigned char>(left))) ==
                 static_cast<char>(std::tolower(static_cast<unsigned char>(right)));
        })) {
      auto value = line.substr(kName.size());
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      const auto end = value.find_first_of("\r\n");
      if (end != std::string_view::npos) {
        value = value.substr(0, end);
      }
      std::uint64_t seconds = 0;
      bool numeric = !value.empty();
      for (const char character : value) {
        if (character < '0' || character > '9' || seconds > 60000) {
          numeric = false;
          break;
        }
        seconds = seconds * 10 + static_cast<std::uint64_t>(character - '0');
      }
      if (numeric && seconds <= 60000) {
        retry_after = std::chrono::milliseconds{seconds * 1000};
      }
    }
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
std::string part(CURLU* url, CURLUPart component) {
  char* raw = nullptr;
  if (curl_url_get(url, component, &raw, 0) != CURLUE_OK) {
    return {};
  }
  const std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
  return raw;
}
bool endpoint_allowed(const std::string& endpoint) {
  const UrlHandle url(curl_url());
  if (!url || curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(), 0) != CURLUE_OK) {
    return false;
  }
  const auto scheme = part(url.get(), CURLUPART_SCHEME);
  const auto host = part(url.get(), CURLUPART_HOST);
  return (scheme == "https" ||
          (scheme == "http" && (host == "127.0.0.1" || host == "localhost"))) &&
         part(url.get(), CURLUPART_USER).empty() && part(url.get(), CURLUPART_PASSWORD).empty() &&
         part(url.get(), CURLUPART_QUERY).empty() && part(url.get(), CURLUPART_FRAGMENT).empty();
}
std::uint32_t latency_ms(std::chrono::steady_clock::duration elapsed) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  if (ms <= 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(std::min<std::int64_t>(ms, target::kModelMaximumLatencyMs));
}
class ModelHttpProvider final : public target::ModelProviderPort {
public:
  ModelHttpProvider() {
    initialize_curl();
  }
  target::ModelProviderResult chat(const target::ModelProviderRequest& request) override {
    try {
      return perform(request);
    } catch (...) {
      return unknown(target::ErrorCode::Internal);
    }
  }

private:
  target::ModelProviderResult perform(const target::ModelProviderRequest& request) {
    if (request.messages.empty() || request.messages.size() > target::kModelMaximumMessages ||
        request.endpoint.empty() || !endpoint_allowed(request.endpoint) ||
        !target::valid_provider_model(request.provider_model) ||
        !target::valid_model_timeout(request.timeout_ms) || request.secret.empty() ||
        request.secret.size() > target::kModelMaximumSecretBytes ||
        !target::valid_model_secret(request.secret)) {
      return rejected(target::ErrorCode::InvalidArgument);
    }
    std::size_t request_bytes = 0;
    for (const auto& message : request.messages) {
      if (!target::valid_model_message_role(message.role) || request_bytes > 65536) {
        return rejected(target::ErrorCode::InvalidArgument);
      }
      request_bytes += message.role.size() + message.text.size() + 64;
    }
    if (request_bytes > target::kModelMaximumRequestBytes) {
      return rejected(target::ErrorCode::InvalidArgument);
    }
    Json payload;
    (*payload.mutable_fields())["model"].set_string_value(request.provider_model);
    (*payload.mutable_fields())["stream"].set_bool_value(false);
    auto* messages = (*payload.mutable_fields())["messages"].mutable_list_value();
    for (const auto& message : request.messages) {
      auto* fields = messages->add_values()->mutable_struct_value()->mutable_fields();
      (*fields)["role"].set_string_value(message.role);
      (*fields)["content"].set_string_value(message.text);
    }
    std::string encoded;
    if (!google::protobuf::util::MessageToJsonString(payload, &encoded).ok()) {
      return rejected(target::ErrorCode::InvalidArgument);
    }
    const CurlHandle curl(curl_easy_init());
    if (!curl) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    SecretString authorization{
        "Authorization: Bearer " +
        std::string(reinterpret_cast<const char*>(request.secret.data()), request.secret.size())};
    curl_slist* raw_headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (raw_headers == nullptr) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    raw_headers = curl_slist_append(raw_headers, authorization.value().c_str());
    HeaderList headers(raw_headers);
    if (!headers) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    std::string body;
    std::optional<std::chrono::milliseconds> retry_after;
    const auto started = std::chrono::steady_clock::now();
    if (curl_easy_setopt(curl.get(), CURLOPT_URL, request.endpoint.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, encoded.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(encoded.size())) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_ms)) !=
            CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(std::min<std::uint32_t>(request.timeout_ms, 5000))) !=
            CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, "") != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https,http") != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, receive_header) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &retry_after) != CURLE_OK) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    const auto status = curl_easy_perform(curl.get());
    const auto latency = latency_ms(std::chrono::steady_clock::now() - started);
    if (status != CURLE_OK) {
      if (status == CURLE_OPERATION_TIMEDOUT) {
        return unknown(target::ErrorCode::Timeout);
      }
      // TLS/connection/write-cap/abort after the POST may have reached the provider: unknown.
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    long http_status = 0;
    if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    if (http_status == 400 || http_status == 422) {
      return rejected(target::ErrorCode::InvalidArgument);
    }
    if (http_status == 401 || http_status == 403) {
      return rejected(target::ErrorCode::UpstreamUnavailable);
    }
    if (http_status == 404 || http_status == 405 || http_status == 410) {
      return rejected(target::ErrorCode::CapabilityNotSupported);
    }
    if (http_status == 429) {
      return rejected(target::ErrorCode::RateLimited, retry_after);
    }
    if (http_status >= 400 && http_status < 500) {
      return rejected(target::ErrorCode::UpstreamUnavailable);
    }
    if (http_status != 200) {
      // 3xx (redirects are disabled), 5xx and malformed non-200 answers can follow a delivered
      // POST: never a known failure, always a conservative unknown.
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    Json decoded;
    if (!google::protobuf::util::JsonStringToMessage(body, &decoded).ok()) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    const auto& choices = field(decoded, "choices").list_value();
    if (choices.values_size() != 1) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    const auto& message = field(choices.values(0).struct_value(), "message").struct_value();
    const auto& content = field(message, "content");
    if (content.kind_case() != google::protobuf::Value::kStringValue ||
        content.string_value().size() > target::kModelMaximumResultBytes ||
        message.fields().contains("tool_calls") || message.fields().contains("function_call")) {
      return unknown(target::ErrorCode::UpstreamUnavailable);
    }
    target::ModelProviderResult result;
    result.outcome = target::ModelProviderOutcome::Succeeded;
    result.reply.text = content.string_value();
    result.reply.latency_ms = latency;
    const auto& id = field(decoded, "id");
    if (id.kind_case() == google::protobuf::Value::kStringValue &&
        target::valid_provider_request_id(id.string_value())) {
      result.reply.provider_request_id = id.string_value();
    }
    if (decoded.fields().contains("usage")) {
      const auto& usage = field(decoded, "usage").struct_value();
      const auto input = tokens(usage, "prompt_tokens");
      const auto output = tokens(usage, "completion_tokens");
      if (!input || !output) {
        return unknown(target::ErrorCode::UpstreamUnavailable);
      }
      result.reply.usage = {true, *input, *output};
    } else {
      result.reply.usage = {false, 0, 0};
    }
    return result;
  }
};
} // namespace

core::target::Outcome<std::unique_ptr<core::target::ModelProviderPort>> make_model_http_provider() {
  try {
    return std::unique_ptr<core::target::ModelProviderPort>(std::make_unique<ModelHttpProvider>());
  } catch (...) {
    return core::target::Failure{core::target::ErrorCode::UpstreamUnavailable,
                                 core::target::Acceptance::NotAccepted,
                                 core::target::Recovery::Backoff,
                                 {},
                                 {}};
  }
}
} // namespace kiarabot::infrastructure
