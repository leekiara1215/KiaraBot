#include "model_http_stream_provider.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <curl/curl.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "model_sse_parser.hpp"

namespace kiarabot::infrastructure {
namespace {
namespace t = core::target;
using Json = google::protobuf::Struct;
using Value = google::protobuf::Value;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static_assert(std::is_nothrow_move_constructible_v<t::ModelProviderStreamResult>);

struct CurlDeleter {
  void operator()(CURL* handle) const noexcept {
    curl_easy_cleanup(handle);
  }
};
struct MultiDeleter {
  void operator()(CURLM* handle) const noexcept {
    curl_multi_cleanup(handle);
  }
};
struct HeadersDeleter {
  void operator()(curl_slist* headers) const noexcept {
    curl_slist_free_all(headers);
  }
};
using Curl = std::unique_ptr<CURL, CurlDeleter>;
using Multi = std::unique_ptr<CURLM, MultiDeleter>;
using Headers = std::unique_ptr<curl_slist, HeadersDeleter>;
using CurlUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;

const Value& field(const Json& object, const std::string& key) {
  static const Value missing;
  const auto found = object.fields().find(key);
  return found == object.fields().end() ? missing : found->second;
}

std::optional<std::uint64_t> token_count(const Json& usage, const std::string& key) {
  const auto& value = field(usage, key);
  const auto number = value.number_value();
  if (value.kind_case() != Value::kNumberValue || !std::isfinite(number) || number < 0 ||
      number > 9007199254740991.0 || std::floor(number) != number) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(number);
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' ||
                            value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool equal_ascii(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto lower = [](unsigned char value) {
      return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
    };
    if (lower(static_cast<unsigned char>(left[index])) !=
        lower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

t::ModelProviderStreamResult
not_dispatched(t::ErrorCode code,
               t::ModelStreamTermination termination = t::ModelStreamTermination::Failed) {
  t::ModelProviderStreamResult result;
  result.outcome = t::ModelProviderOutcome::DefiniteFailure;
  result.termination = termination;
  result.may_have_billed = false;
  result.usage.known = true;
  auto recovery = t::Recovery::Diagnose;
  if (code == t::ErrorCode::InvalidArgument || code == t::ErrorCode::CapabilityNotSupported) {
    recovery = t::Recovery::CorrectInput;
  } else if (code == t::ErrorCode::UpstreamUnavailable &&
             termination != t::ModelStreamTermination::Cancelled) {
    recovery = t::Recovery::Backoff;
  }
  result.failure = t::Failure{code, t::Acceptance::NotAccepted, recovery, {}, {}};
  return result;
}

bool valid_request(const t::ModelProviderRequest& request) {
  if (!t::valid_model_endpoint(request.endpoint) ||
      !t::valid_provider_model(request.provider_model) ||
      !t::valid_model_timeout(request.timeout_ms) || !t::valid_model_secret(request.secret) ||
      request.messages.empty() || request.messages.size() > t::kModelMaximumMessages) {
    return false;
  }
  std::size_t bytes = 0;
  for (const auto& message : request.messages) {
    if (!t::valid_model_message_role(message.role) ||
        message.text.find('\0') != std::string::npos ||
        message.text.size() > t::kModelMaximumRequestBytes - bytes) {
      return false;
    }
    bytes += message.text.size();
  }
  return t::model_request_canonical(request.provider_model, request.messages).size() <=
         t::kModelMaximumRequestBytes;
}

struct Transfer {
  Transfer(t::ModelProviderStreamSink& output, t::ModelStreamCancellation stop,
           Clock::time_point until)
      : sink(output), cancellation(stop), deadline(until),
        parser([this](std::string_view event, std::string_view data) {
          return decode(event, data);
        }) {}

  bool fail(t::ErrorCode code) {
    if (!error) {
      error = code;
    }
    return false;
  }

  bool stopped() {
    if (cancellation.stop_requested()) {
      cancelled = true;
      fail(t::ErrorCode::UpstreamUnavailable);
      return true;
    }
    if (Clock::now() >= deadline) {
      fail(t::ErrorCode::Timeout);
      return true;
    }
    return error.has_value();
  }

  bool emit(std::string_view text) {
    if (text.size() > t::kModelMaximumResultBytes - result.emitted_bytes) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    std::size_t begin = 0;
    while (begin < text.size()) {
      auto end = std::min(begin + t::kModelStreamMaximumDeltaBytes, text.size());
      if (end < text.size()) {
        while (end > begin && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) {
          --end;
        }
      }
      t::ModelStreamDelta delta{std::string{text.substr(begin, end - begin)}};
      const auto enqueue_deadline = std::min(deadline, Clock::now() + t::kModelStreamEnqueueBudget);
      for (;;) {
        if (stopped()) {
          return false;
        }
        const auto admitted = sink.try_emit(delta);
        if (admitted == t::ModelStreamSinkResult::Accepted) {
          result.emitted_bytes += delta.text.size();
          ++result.emitted_delta_count;
          break;
        }
        if (admitted == t::ModelStreamSinkResult::Cancelled) {
          cancelled = true;
          return fail(t::ErrorCode::UpstreamUnavailable);
        }
        if (Clock::now() >= enqueue_deadline) {
          return fail(t::ErrorCode::UpstreamUnavailable);
        }
        std::this_thread::sleep_for(1ms);
      }
      begin = end;
    }
    return true;
  }

  bool decode(std::string_view event, std::string_view data) {
    if (stopped()) {
      return false;
    }
    if (event != "message") {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    if (data.empty()) {
      return true; // Legal empty SSE events are not a chat completion.
    }
    if (done) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    if (data == "[DONE]") {
      if (result.finish_reason == t::ModelStreamFinishReason::Unspecified) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      done = true;
      return true;
    }
    Json chunk;
    if (!google::protobuf::util::JsonStringToMessage(std::string{data}, &chunk).ok()) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto& id = field(chunk, "id");
    if (id.kind_case() == Value::kStringValue && t::valid_provider_request_id(id.string_value())) {
      if (!result.provider_request_id.empty() && result.provider_request_id != id.string_value()) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      result.provider_request_id = id.string_value();
    }
    const auto& choices = field(chunk, "choices");
    if (choices.kind_case() != Value::kListValue || choices.list_value().values_size() > 1) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    if (choices.list_value().values_size() == 0) {
      const auto& usage = field(chunk, "usage");
      if (result.finish_reason == t::ModelStreamFinishReason::Unspecified ||
          usage.kind_case() != Value::kStructValue || result.usage.known) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      const auto input = token_count(usage.struct_value(), "prompt_tokens");
      const auto output = token_count(usage.struct_value(), "completion_tokens");
      if (!input || !output) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      result.usage = {true, *input, *output};
      return true;
    }
    if (result.finish_reason != t::ModelStreamFinishReason::Unspecified) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto& choice_value = choices.list_value().values(0);
    if (choice_value.kind_case() != Value::kStructValue) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto& choice = choice_value.struct_value();
    const auto& index = field(choice, "index");
    if (index.kind_case() != Value::KIND_NOT_SET &&
        (index.kind_case() != Value::kNumberValue || index.number_value() != 0)) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto& delta = field(choice, "delta");
    if (delta.kind_case() != Value::kStructValue) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto& object = delta.struct_value();
    for (const auto* unsupported : {"tool_calls", "function_call", "audio", "refusal"}) {
      if (object.fields().contains(unsupported)) {
        return fail(t::ErrorCode::CapabilityNotSupported);
      }
    }
    const auto& role = field(object, "role");
    if (role.kind_case() != Value::KIND_NOT_SET &&
        (role.kind_case() != Value::kStringValue || role.string_value() != "assistant")) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    const auto& text = field(object, "content");
    if (text.kind_case() == Value::kStringValue) {
      if (!emit(text.string_value())) {
        return false;
      }
    } else if (text.kind_case() != Value::KIND_NOT_SET && text.kind_case() != Value::kNullValue) {
      return fail(t::ErrorCode::CapabilityNotSupported);
    }
    const auto& finish = field(choice, "finish_reason");
    if (finish.kind_case() != Value::KIND_NOT_SET && finish.kind_case() != Value::kNullValue) {
      if (finish.kind_case() != Value::kStringValue) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      if (finish.string_value() == "stop") {
        result.finish_reason = t::ModelStreamFinishReason::Stop;
      } else if (finish.string_value() == "length") {
        result.finish_reason = t::ModelStreamFinishReason::Length;
      } else if (finish.string_value() == "content_filter") {
        result.finish_reason = t::ModelStreamFinishReason::ContentFilter;
      } else {
        return fail(t::ErrorCode::CapabilityNotSupported);
      }
    }
    return true;
  }

  bool header(std::string_view line) {
    if (line.size() > 16384 - header_bytes) {
      return fail(t::ErrorCode::UpstreamUnavailable);
    }
    header_bytes += line.size();
    if (line.starts_with("HTTP/")) {
      const auto space = line.find(' ');
      if (space == std::string_view::npos || line.size() < space + 4) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      const auto parsed =
          std::from_chars(line.data() + space + 1, line.data() + space + 4, http_status);
      if (parsed.ec != std::errc{}) {
        return fail(t::ErrorCode::UpstreamUnavailable);
      }
      event_stream = false;
      return true;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      return true;
    }
    const auto name = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1));
    if (equal_ascii(name, "content-type")) {
      event_stream = equal_ascii(trim(value.substr(0, value.find(';'))), "text/event-stream");
    } else if (equal_ascii(name, "retry-after")) {
      std::uint64_t seconds = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
      if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
          seconds <= 60000) {
        retry_after = std::chrono::milliseconds{seconds * 1000};
      }
    }
    return true;
  }

  t::ModelProviderStreamSink& sink;
  t::ModelStreamCancellation cancellation;
  Clock::time_point deadline;
  t::ModelProviderStreamResult result;
  std::optional<t::ErrorCode> error;
  std::optional<std::chrono::milliseconds> retry_after;
  bool cancelled = false;
  bool done = false;
  bool event_stream = false;
  int http_status = 0;
  std::size_t header_bytes = 0;
  std::size_t response_bytes = 0;
  ModelSseParser parser;
};

std::size_t receive(char* bytes, std::size_t size, std::size_t count, void* context) noexcept {
  auto& transfer = *static_cast<Transfer*>(context);
  try {
    if (size != 0 &&
        count > (t::kModelMaximumProviderResponseBytes - transfer.response_bytes) / size) {
      transfer.fail(t::ErrorCode::UpstreamUnavailable);
      return 0;
    }
    const auto length = size * count;
    transfer.response_bytes += length;
    if (transfer.http_status == 200) {
      if (!transfer.event_stream || !transfer.parser.feed({bytes, length})) {
        transfer.fail(t::ErrorCode::UpstreamUnavailable);
        return 0;
      }
    }
    return length;
  } catch (...) {
    transfer.fail(t::ErrorCode::Internal);
    return 0;
  }
}

std::size_t receive_header(char* bytes, std::size_t size, std::size_t count,
                           void* context) noexcept {
  auto& transfer = *static_cast<Transfer*>(context);
  try {
    if (size != 0 && count > 16384 / size) {
      transfer.fail(t::ErrorCode::UpstreamUnavailable);
      return 0;
    }
    return transfer.header({bytes, size * count}) ? size * count : 0;
  } catch (...) {
    transfer.fail(t::ErrorCode::Internal);
    return 0;
  }
}

// The multi loop checks cancellation at most every 20ms, including a provider that never sends
// another byte. No detached watchdog, callback stack reference or global curl cleanup race.
CURLcode perform(CURL* curl, Transfer& transfer) {
  const Multi multi(curl_multi_init());
  if (!multi || curl_multi_add_handle(multi.get(), curl) != CURLM_OK) {
    transfer.fail(t::ErrorCode::UpstreamUnavailable);
    return CURLE_FAILED_INIT;
  }
  CURLcode status = CURLE_ABORTED_BY_CALLBACK;
  for (;;) {
    if (transfer.stopped()) {
      break;
    }
    int running = 0;
    transfer.result.provider_dispatched = true;
    if (curl_multi_perform(multi.get(), &running) != CURLM_OK) {
      transfer.fail(t::ErrorCode::UpstreamUnavailable);
      break;
    }
    int remaining = 0;
    if (const auto* message = curl_multi_info_read(multi.get(), &remaining);
        message && message->msg == CURLMSG_DONE) {
      status = message->data.result;
      break;
    }
    int events = 0;
    if (curl_multi_poll(multi.get(), nullptr, 0, 20, &events) != CURLM_OK) {
      transfer.fail(t::ErrorCode::UpstreamUnavailable);
      break;
    }
  }
  curl_multi_remove_handle(multi.get(), curl);
  return status;
}

t::ModelProviderStreamResult finish(Transfer& transfer, CURLcode status,
                                    Clock::time_point started) {
  auto& result = transfer.result;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
  result.latency_ms = static_cast<std::uint32_t>(
      std::clamp<std::int64_t>(elapsed.count(), 0, t::kModelMaximumLatencyMs));
  if (!result.provider_dispatched) {
    return not_dispatched(transfer.error.value_or(t::ErrorCode::UpstreamUnavailable),
                          transfer.cancelled ? t::ModelStreamTermination::Cancelled
                                             : t::ModelStreamTermination::Failed);
  }
  if (transfer.http_status >= 400 && transfer.http_status < 500) {
    result.outcome = t::ModelProviderOutcome::DefiniteFailure;
    result.may_have_billed = false;
    result.usage = {true, 0, 0};
    auto code = t::ErrorCode::UpstreamUnavailable;
    if (transfer.http_status == 400 || transfer.http_status == 422) {
      code = t::ErrorCode::InvalidArgument;
    } else if (transfer.http_status == 404 || transfer.http_status == 405 ||
               transfer.http_status == 410) {
      code = t::ErrorCode::CapabilityNotSupported;
    } else if (transfer.http_status == 429) {
      code = t::ErrorCode::RateLimited;
    }
    result.failure =
        t::Failure{code,
                   t::Acceptance::NotAccepted,
                   t::Recovery::Diagnose,
                   {},
                   code == t::ErrorCode::RateLimited ? transfer.retry_after : std::nullopt};
  } else if (status == CURLE_OK && transfer.http_status == 200 && transfer.event_stream &&
             transfer.done && !transfer.error) {
    result.outcome = t::ModelProviderOutcome::Succeeded;
    result.termination = result.finish_reason == t::ModelStreamFinishReason::Stop
                             ? t::ModelStreamTermination::Completed
                             : t::ModelStreamTermination::Truncated;
  } else {
    result.outcome = t::ModelProviderOutcome::Unknown;
    result.termination = transfer.cancelled ? t::ModelStreamTermination::Cancelled
                                            : t::ModelStreamTermination::Failed;
    const auto code = status == CURLE_OPERATION_TIMEDOUT
                          ? t::ErrorCode::Timeout
                          : transfer.error.value_or(t::ErrorCode::UpstreamUnavailable);
    result.failure =
        t::Failure{code, t::Acceptance::Unknown, t::Recovery::QuerySameIdentity, {}, {}};
  }
  result.partial =
      result.emitted_bytes > 0 && result.termination != t::ModelStreamTermination::Completed;
  return std::move(result);
}

} // namespace

t::ModelProviderStreamResult
perform_model_http_stream_impl(const t::ModelProviderRequest& request,
                               t::ModelProviderStreamSink& sink,
                               t::ModelStreamCancellation cancellation) {
  if (!valid_request(request)) {
    return not_dispatched(t::ErrorCode::InvalidArgument);
  }
  if (cancellation.stop_requested()) {
    return not_dispatched(t::ErrorCode::UpstreamUnavailable, t::ModelStreamTermination::Cancelled);
  }
  // libcurl's URL parser rejects malformed ports/authorities before the first perform. The
  // domain shape check above additionally forbids credentials, queries, fragments and remote
  // plaintext HTTP; curl's later connection/handshake failures remain conservative UNKNOWN.
  const CurlUrl url(curl_url(), &curl_url_cleanup);
  if (!url) {
    return not_dispatched(t::ErrorCode::UpstreamUnavailable);
  }
  const auto url_status = curl_url_set(url.get(), CURLUPART_URL, request.endpoint.c_str(), 0);
  if (url_status != CURLUE_OK) {
    return not_dispatched(url_status == CURLUE_OUT_OF_MEMORY ? t::ErrorCode::UpstreamUnavailable
                                                             : t::ErrorCode::InvalidArgument);
  }
  const auto started = Clock::now();
  Transfer transfer(sink, cancellation, started + std::chrono::milliseconds{request.timeout_ms});
  try {
    Json payload;
    (*payload.mutable_fields())["model"].set_string_value(request.provider_model);
    (*payload.mutable_fields())["stream"].set_bool_value(true);
    auto* options = (*payload.mutable_fields())["stream_options"].mutable_struct_value();
    (*options->mutable_fields())["include_usage"].set_bool_value(true);
    auto* messages = (*payload.mutable_fields())["messages"].mutable_list_value();
    for (const auto& message : request.messages) {
      auto* object = messages->add_values()->mutable_struct_value()->mutable_fields();
      (*object)["role"].set_string_value(message.role);
      (*object)["content"].set_string_value(message.text);
    }
    std::string encoded;
    if (!google::protobuf::util::MessageToJsonString(payload, &encoded).ok()) {
      return not_dispatched(t::ErrorCode::InvalidArgument);
    }
    const Curl curl(curl_easy_init());
    if (!curl) {
      return not_dispatched(t::ErrorCode::UpstreamUnavailable);
    }
    Headers headers;
    const auto append = [&](const char* line) {
      auto* next = curl_slist_append(headers.get(), line);
      if (!next) {
        return false;
      }
      headers.release();
      headers.reset(next);
      return true;
    };
    std::string authorization{"Authorization: Bearer "};
    authorization.append(reinterpret_cast<const char*>(request.secret.data()),
                         request.secret.size());
    const bool headers_ready = append("Content-Type: application/json") &&
                               append("Accept: text/event-stream") && append(authorization.c_str());
    volatile char* secret_bytes = authorization.data();
    for (std::size_t index = 0; index < authorization.size(); ++index) {
      secret_bytes[index] = 0;
    }
    if (!headers_ready) {
      return not_dispatched(t::ErrorCode::UpstreamUnavailable);
    }
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
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, receive_header) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &transfer) != CURLE_OK) {
      return not_dispatched(t::ErrorCode::UpstreamUnavailable);
    }
    return finish(transfer, perform(curl.get(), transfer), started);
  } catch (...) {
    transfer.fail(t::ErrorCode::Internal);
    return finish(transfer, CURLE_ABORTED_BY_CALLBACK, started);
  }
}

t::ModelProviderStreamResult perform_model_http_stream(const t::ModelProviderRequest& request,
                                                       t::ModelProviderStreamSink& sink,
                                                       t::ModelStreamCancellation cancellation) {
  try {
    return perform_model_http_stream_impl(request, sink, cancellation);
  } catch (...) {
    // The inner boundary catches every exception after dispatch. Remaining failures occurred
    // during request validation/parser allocation before any curl request could be started.
    return not_dispatched(t::ErrorCode::Internal);
  }
}

} // namespace kiarabot::infrastructure
