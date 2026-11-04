#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "kiarabot/infrastructure/model_http_provider.hpp"

namespace {
namespace t = kiarabot::core::target;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{message});
  }
}

bool send_bytes(int socket, std::string_view value) {
  while (!value.empty()) {
    const auto sent = ::send(socket, value.data(), value.size(), 0);
    if (sent <= 0) {
      return false;
    }
    value.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

void send_chunk(int socket, std::string_view value) {
  std::ostringstream header;
  header << std::hex << value.size() << "\r\n";
  if (send_bytes(socket, header.str()) && send_bytes(socket, value)) {
    send_bytes(socket, "\r\n");
  }
}

constexpr std::string_view kHeaders =
    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n"
    "Connection: close\r\n\r\n";

std::string delta(std::string_view text) {
  return "data: {\"id\":\"request-1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" +
         std::string{text} + "\"},\"finish_reason\":null}]}\n\n";
}

std::string ending(std::string_view reason = "stop", bool usage = true) {
  std::string result = "data: {\"id\":\"request-1\",\"choices\":[{\"index\":0,\"delta\":{},"
                       "\"finish_reason\":\"" +
                       std::string{reason} + "\"}]}\n\n";
  if (usage) {
    result += "data: {\"id\":\"request-1\",\"choices\":[],\"usage\":{\"prompt_tokens\":12,"
              "\"completion_tokens\":3}}\n\n";
  }
  return result + "data: [DONE]\n\n";
}

class Fixture {
public:
  using Reply = std::function<void(int, const std::string&)>;
  explicit Fixture(Reply reply) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    check(socket_ >= 0, "fixture socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "fixture bind");
    check(::listen(socket_, 1) == 0, "fixture listen");
    socklen_t size = sizeof(address);
    check(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
          "fixture port");
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this, reply = std::move(reply)] {
      try {
        while (!stop_.load()) {
          pollfd descriptor{socket_, POLLIN, 0};
          if (::poll(&descriptor, 1, 20) <= 0) {
            continue;
          }
          const auto client = ::accept(socket_, nullptr, nullptr);
          if (client < 0) {
            continue;
          }
          timeval timeout{3, 0};
          ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
          ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
          std::string request;
          char buffer[4096];
          for (;;) {
            const auto received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) {
              break;
            }
            request.append(buffer, static_cast<std::size_t>(received));
            const auto boundary = request.find("\r\n\r\n");
            if (boundary != std::string::npos) {
              const auto position = request.find("Content-Length:");
              if (position != std::string::npos) {
                const auto length = std::stoull(request.substr(position + 15));
                if (request.size() >= boundary + 4 + length) {
                  break;
                }
              }
            }
          }
          {
            std::lock_guard lock(mutex_);
            request_ = request;
          }
          ++requests_;
          try {
            reply(client, request);
          } catch (...) {
            error_ = std::current_exception();
          }
          ::shutdown(client, SHUT_RDWR);
          ::close(client);
          break;
        }
      } catch (...) {
        error_ = std::current_exception();
      }
    });
  }
  ~Fixture() {
    stop_.store(true);
    worker_.join();
    ::close(socket_);
  }
  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat/completions";
  }
  int requests() const {
    return requests_.load();
  }
  std::string request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }

private:
  int socket_ = -1;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> requests_{0};
  mutable std::mutex mutex_;
  std::string request_;
  std::exception_ptr error_;
  std::thread worker_;
};

class Sink final : public t::ModelProviderStreamSink {
public:
  t::ModelStreamSinkResult try_emit(const t::ModelStreamDelta& value) override {
    if (admit) {
      const auto result = admit(value);
      if (result != t::ModelStreamSinkResult::Accepted) {
        return result;
      }
    }
    chunks.push_back(value.text);
    text += value.text;
    return t::ModelStreamSinkResult::Accepted;
  }
  std::function<t::ModelStreamSinkResult(const t::ModelStreamDelta&)> admit;
  std::vector<std::string> chunks;
  std::string text;
};

t::ModelProviderRequest request(std::string endpoint) {
  static constexpr char kSecret[] = "fixture-secret";
  t::ModelProviderRequest result;
  result.endpoint = std::move(endpoint);
  result.provider_model = "fixture-model";
  result.messages = {{"user", "hello"}};
  result.secret = std::as_bytes(std::span{kSecret, sizeof(kSecret) - 1});
  result.timeout_ms = 2000;
  return result;
}

std::unique_ptr<t::ModelProviderPort> provider() {
  auto result = kiarabot::infrastructure::make_model_http_provider();
  check(std::holds_alternative<std::unique_ptr<t::ModelProviderPort>>(result), "open provider");
  return std::move(std::get<std::unique_ptr<t::ModelProviderPort>>(result));
}

void send_stream(int socket, std::string_view body, std::size_t step = 7) {
  send_bytes(socket, kHeaders);
  for (std::size_t index = 0; index < body.size(); index += step) {
    send_chunk(socket, body.substr(index, step));
  }
  send_bytes(socket, "0\r\n\r\n");
}

void true_incremental_success() {
  std::mutex mutex;
  std::condition_variable received;
  bool saw_first = false;
  std::atomic<bool> final_sent{false};
  std::atomic<bool> incremental{false};
  Fixture server([&](int socket, const std::string&) {
    send_bytes(socket, kHeaders);
    send_chunk(socket, delta("你好"));
    {
      std::unique_lock lock(mutex);
      check(received.wait_for(lock, 1s, [&] { return saw_first; }),
            "sink receives before fixture finishes");
    }
    final_sent.store(true);
    send_chunk(socket, delta("🙂") + ending());
    send_bytes(socket, "0\r\n\r\n");
  });
  auto model = provider();
  check(model->supports_streaming(), "real provider advertises streaming");
  Sink sink;
  sink.admit = [&](const t::ModelStreamDelta&) {
    if (!final_sent.load()) {
      incremental.store(true);
      {
        std::lock_guard lock(mutex);
        saw_first = true;
      }
      received.notify_all();
    }
    return t::ModelStreamSinkResult::Accepted;
  };
  const auto result = model->chat_stream(request(server.endpoint()), sink);
  check(incremental.load() && result.outcome == t::ModelProviderOutcome::Succeeded,
        "incremental success");
  check(result.termination == t::ModelStreamTermination::Completed && !result.partial,
        "complete fact");
  check(sink.text == "你好🙂" && result.emitted_bytes == 10 && result.emitted_delta_count == 2,
        "text and byte counts");
  check(result.usage.known && result.usage.input_tokens == 12 && result.usage.output_tokens == 3,
        "authoritative usage-only chunk");
  check(result.provider_dispatched && result.may_have_billed && !result.failure,
        "provider billing facts");
  check(server.requests() == 1 && result.provider_request_id == "request-1",
        "single POST and sanitized id");
  const auto sent = server.request();
  check(sent.find("\"stream\":true") != std::string::npos &&
            sent.find("\"include_usage\":true") != std::string::npos &&
            sent.find("Authorization: Bearer fixture-secret") != std::string::npos &&
            sent.find("Accept: text/event-stream") != std::string::npos,
        "streaming HTTP request");
}

void finish_and_usage() {
  for (const auto& reason : {"stop", "length", "content_filter"}) {
    Fixture server([&](int socket, const std::string&) {
      send_stream(socket, delta("text") + ending(reason, false), 1);
    });
    Sink sink;
    const auto result = provider()->chat_stream(request(server.endpoint()), sink);
    check(result.outcome == t::ModelProviderOutcome::Succeeded && !result.usage.known &&
              result.usage.input_tokens == 0 && result.may_have_billed,
          "missing usage is unknown not free");
    check((result.termination == t::ModelStreamTermination::Completed) ==
              (std::string_view{reason} == "stop"),
          "stop versus truncation");
    check(result.partial == (std::string_view{reason} != "stop"), "truncated text stays partial");
  }
  Fixture empty([](int socket, const std::string&) { send_stream(socket, ending()); });
  Sink sink;
  const auto result = provider()->chat_stream(request(empty.endpoint()), sink);
  check(result.outcome == t::ModelProviderOutcome::Succeeded && sink.text.empty() &&
            !result.partial,
        "empty valid completion");
}

void http_error_mapping() {
  for (const int status : {400, 422, 401, 403, 404, 405, 410, 429, 418, 500, 503, 302}) {
    Fixture server([status](int socket, const std::string&) {
      send_bytes(socket, "HTTP/1.1 " + std::to_string(status) +
                             " Fixture\r\nContent-Length: 0\r\n"
                             "Retry-After: 7\r\nConnection: close\r\n\r\n");
    });
    Sink sink;
    const auto result = provider()->chat_stream(request(server.endpoint()), sink);
    const bool rejected = status >= 400 && status < 500;
    check(result.outcome == (rejected ? t::ModelProviderOutcome::DefiniteFailure
                                      : t::ModelProviderOutcome::Unknown),
          "HTTP outcome mapping");
    check(result.may_have_billed == !rejected && result.provider_dispatched && sink.text.empty(),
          "HTTP billing");
    check(result.failure.has_value(), "HTTP failure stable context");
    if (status == 429) {
      check(result.failure->code == t::ErrorCode::RateLimited && result.failure->retry_after == 7s,
            "429 hint and known no billing");
    } else if (status == 400 || status == 422) {
      check(result.failure->code == t::ErrorCode::InvalidArgument, "400/422");
    } else if (status == 404 || status == 405 || status == 410) {
      check(result.failure->code == t::ErrorCode::CapabilityNotSupported,
            "unsupported HTTP endpoint");
    }
    check(server.requests() == 1, "no retry or redirect on HTTP error");
  }
}

void malformed_partial_and_limits() {
  const std::vector<std::string> malformed{
      "data: not-json\n\n",
      "data: [DONE]\n\n",
      delta("part"),
      delta("part") + "data: {\"choices\":[{\"delta\":{\"tool_calls\":[]}}]}\n\n",
      delta("part") + "data: \xff\n\n",
      delta("part") + "event: error\ndata: {}\n\n",
      ":" + std::string(65536, 'x') + "\n\n",
      delta(std::string(40000, 'a')) + delta(std::string(40000, 'b')) + ending(),
  };
  for (const auto& body : malformed) {
    Fixture server([&](int socket, const std::string&) { send_stream(socket, body, 4096); });
    Sink sink;
    const auto result = provider()->chat_stream(request(server.endpoint()), sink);
    check(result.outcome == t::ModelProviderOutcome::Unknown && result.may_have_billed,
          "malformed/partial cannot be known-unbilled");
    check(result.partial == !sink.text.empty() && result.emitted_bytes == sink.text.size(),
          "partial records accepted text");
    check(server.requests() == 1 && sink.text.size() <= 65536, "malformed single bounded request");
  }
  Fixture wrong_type([](int socket, const std::string&) {
    send_bytes(socket,
               "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");
  });
  Sink sink;
  check(provider()->chat_stream(request(wrong_type.endpoint()), sink).outcome ==
            t::ModelProviderOutcome::Unknown,
        "wrong response MIME rejected");

  const auto body = delta(std::string(4095, 'x') + "🙂") + ending();
  Fixture boundaries([&](int socket, const std::string&) { send_stream(socket, body, 31); });
  Sink bounded;
  const auto result = provider()->chat_stream(request(boundaries.endpoint()), bounded);
  check(result.outcome == t::ModelProviderOutcome::Succeeded && bounded.chunks.size() == 2 &&
            bounded.chunks[0].size() == 4095 && bounded.chunks[1] == "🙂",
        "split delta on UTF-8 boundary");
}

void cancellation_and_backpressure() {
  std::stop_source cancelled;
  cancelled.request_stop();
  Fixture untouched([](int socket, const std::string&) { send_stream(socket, ending()); });
  Sink sink;
  const auto before =
      provider()->chat_stream(request(untouched.endpoint()), sink, cancelled.get_token());
  check(before.outcome == t::ModelProviderOutcome::DefiniteFailure && !before.provider_dispatched &&
            !before.may_have_billed && before.termination == t::ModelStreamTermination::Cancelled,
        "pre-dispatch cancellation known not executed");
  check(before.failure && before.failure->recovery == t::Recovery::Diagnose,
        "cancel is not an instruction to correct input");
  check(untouched.requests() == 0, "cancelled before dispatch sends no HTTP request");

  std::stop_source during;
  Fixture stalled([](int socket, const std::string&) {
    send_bytes(socket, kHeaders);
    send_chunk(socket, delta("part"));
    char byte = 0;
    ::recv(socket, &byte, 1, 0); // bounded by fixture timeout, returns when curl closes.
  });
  Sink partial;
  partial.admit = [&](const t::ModelStreamDelta&) {
    during.request_stop();
    return t::ModelStreamSinkResult::Accepted;
  };
  const auto started = Clock::now();
  const auto after =
      provider()->chat_stream(request(stalled.endpoint()), partial, during.get_token());
  check(
      Clock::now() - started < 1s && after.outcome == t::ModelProviderOutcome::Unknown &&
          after.termination == t::ModelStreamTermination::Cancelled && after.partial &&
          after.may_have_billed,
      "in-flight cancellation promptly aborts a stalled socket and keeps partial billing unknown");

  Fixture full([](int socket, const std::string&) { send_stream(socket, delta("x") + ending()); });
  Sink blocked;
  blocked.admit = [](const t::ModelStreamDelta&) { return t::ModelStreamSinkResult::Full; };
  const auto waiting = Clock::now();
  const auto overflow = provider()->chat_stream(request(full.endpoint()), blocked);
  check(Clock::now() - waiting < 1s && overflow.outcome == t::ModelProviderOutcome::Unknown &&
            overflow.emitted_bytes == 0 && overflow.may_have_billed,
        "backpressure budget bounded without fake text");
}

void timeout_and_validation() {
  Fixture timeout([](int socket, const std::string&) {
    send_bytes(socket, kHeaders);
    char byte = 0;
    ::recv(socket, &byte, 1, 0);
  });
  auto input = request(timeout.endpoint());
  input.timeout_ms = 1000;
  Sink sink;
  const auto result = provider()->chat_stream(input, sink);
  check(result.outcome == t::ModelProviderOutcome::Unknown && result.failure &&
            result.failure->code == t::ErrorCode::Timeout && result.may_have_billed,
        "timeout unknown billing");
  Fixture untouched([](int socket, const std::string&) { send_stream(socket, ending()); });
  auto model = provider();
  input = request(untouched.endpoint());
  input.messages = {{"tool", "bad"}};
  const auto invalid = model->chat_stream(input, sink);
  check(invalid.outcome == t::ModelProviderOutcome::DefiniteFailure && !invalid.may_have_billed &&
            !invalid.provider_dispatched && invalid.usage.known && untouched.requests() == 0,
        "invalid request rejected before dispatch");
  check(invalid.failure && invalid.failure->recovery == t::Recovery::CorrectInput,
        "invalid input recovery");
  for (const auto& endpoint : {"https://127.0.0.1:bad-port/v1", "https://127.0.0.1:70000/v1",
                               "https://user:secret@127.0.0.1/v1", "http://example.invalid/v1"}) {
    auto bad_url = request(endpoint);
    const auto rejected = model->chat_stream(bad_url, sink);
    check(rejected.outcome == t::ModelProviderOutcome::DefiniteFailure &&
              !rejected.provider_dispatched && !rejected.may_have_billed && rejected.failure &&
              rejected.failure->code == t::ErrorCode::InvalidArgument &&
              rejected.failure->recovery == t::Recovery::CorrectInput && untouched.requests() == 0,
          "malformed/forbidden URL is a known non-dispatch rejection");
  }
  auto with_query = request(untouched.endpoint() + "?api_key=secret");
  check(!model->chat_stream(with_query, sink).provider_dispatched && untouched.requests() == 0,
        "credential query never reaches fixture");
  const auto valid_url = model->chat_stream(request(untouched.endpoint()), sink);
  check(valid_url.outcome == t::ModelProviderOutcome::Succeeded && untouched.requests() == 1,
        "positive URL control proves fixture was reachable after zero-request refusals");
  struct UnaryOnly final : t::ModelProviderPort {
    t::ModelProviderResult chat(const t::ModelProviderRequest&) override {
      ++calls;
      return {};
    }
    int calls = 0;
  } unary;
  const auto unsupported = unary.chat_stream(input, sink);
  check(!unary.supports_streaming() && unary.calls == 0 && !unsupported.may_have_billed &&
            unsupported.failure->code == t::ErrorCode::CapabilityNotSupported,
        "default unsupported never fragments or calls old chat");
}

} // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    true_incremental_success();
    finish_and_usage();
    http_error_mapping();
    malformed_partial_and_limits();
    cancellation_and_backpressure();
    timeout_and_validation();
    std::cout << "Real HTTP streaming Provider matrix passed (loopback only)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
