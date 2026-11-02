// P11b loopback tests for the reviewed Model HTTP provider. No external provider is contacted.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/infrastructure/model_http_provider.hpp"

namespace {
namespace t = kiarabot::core::target;
using kiarabot::infrastructure::make_model_http_provider;

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error("P11b assertion: " + std::string{message});
  }
}
std::string http_response(int status, std::string_view reason, const std::string& body,
                          std::string_view extra_headers = {}) {
  return "HTTP/1.1 " + std::to_string(status) + " " + std::string{reason} +
         "\r\nContent-Type: application/json\r\n" + std::string{extra_headers} +
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
std::string success_body(std::string_view content = "hello back") {
  return std::string{"{\"id\":\"chatcmpl-1\",\"choices\":[{\"message\":{\"role\":"
                     "\"assistant\",\"content\":\""} +
         std::string{content} + "\"}}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}}";
}

// Multi-connection loopback HTTP/1.1 server controlled by the test.
class LoopbackServer {
public:
  using Handler = std::function<std::string(const std::string& request)>;
  explicit LoopbackServer(Handler handler) : handler_(std::move(handler)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    check(fd_ >= 0, "socket");
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    check(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
    check(::listen(fd_, 8) == 0, "listen");
    socklen_t length = sizeof(address);
    check(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname");
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~LoopbackServer() {
    stop_ = true;
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
      ::close(fd_);
      fd_ = -1;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;
  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat/completions";
  }
  std::string request() const {
    std::lock_guard lock(mutex_);
    return request_;
  }
  int requests() const {
    return requests_.load();
  }

private:
  void serve() {
    while (!stop_) {
      const int client = ::accept(fd_, nullptr, nullptr);
      if (client < 0) {
        return;
      }
      std::string received;
      char buffer[4096];
      while (received.find("\r\n\r\n") == std::string::npos) {
        const auto read = ::recv(client, buffer, sizeof(buffer), 0);
        if (read <= 0) {
          break;
        }
        received.append(buffer, static_cast<std::size_t>(read));
        if (received.size() > 1U << 20) {
          break;
        }
      }
      const auto separator = received.find("\r\n\r\n");
      if (separator != std::string::npos) {
        const auto headers = received.substr(0, separator);
        std::size_t length = 0;
        const auto content_length = headers.find("Content-Length:");
        if (content_length != std::string::npos) {
          length = static_cast<std::size_t>(
              std::strtoul(headers.c_str() + content_length + 15, nullptr, 10));
        }
        while (received.size() < separator + 4 + length) {
          const auto read = ::recv(client, buffer, sizeof(buffer), 0);
          if (read <= 0) {
            break;
          }
          received.append(buffer, static_cast<std::size_t>(read));
        }
      }
      requests_.fetch_add(1);
      {
        std::lock_guard lock(mutex_);
        request_ = received;
      }
      const auto response = stop_ ? std::string{} : handler_(received);
      if (!response.empty()) {
        ::send(client, response.data(), response.size(), 0);
      }
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
    }
  }
  int fd_{-1};
  unsigned short port_{0};
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> requests_{0};
  mutable std::mutex mutex_;
  std::string request_;
};

t::ModelProviderRequest request_for(const std::string& endpoint, std::string text = "hello") {
  t::ModelProviderRequest request;
  request.endpoint = endpoint;
  request.provider_model = "gpt-test";
  request.timeout_ms = 5000;
  request.messages = {{"user", std::move(text)}};
  const std::string secret = "test-secret";
  std::vector<std::byte> bytes(secret.size());
  for (std::size_t index = 0; index < secret.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(secret[index]));
  }
  static std::vector<std::byte> storage;
  storage = std::move(bytes);
  request.secret = storage;
  return request;
}
std::unique_ptr<t::ModelProviderPort> open_provider() {
  auto provider = make_model_http_provider();
  check(std::holds_alternative<std::unique_ptr<t::ModelProviderPort>>(provider), "provider open");
  return std::move(std::get<std::unique_ptr<t::ModelProviderPort>>(provider));
}

void success_cases() {
  LoopbackServer server(
      [](const std::string&) { return http_response(200, "OK", success_body()); });
  auto provider = open_provider();
  auto result = provider->chat(request_for(server.endpoint()));
  check(result.outcome == t::ModelProviderOutcome::Succeeded, "success outcome");
  check(result.reply.text == "hello back", "success text");
  check(result.reply.usage.known && result.reply.usage.input_tokens == 3 &&
            result.reply.usage.output_tokens == 4,
        "success usage");
  check(result.reply.provider_request_id == "chatcmpl-1", "success provider id");
  check(result.reply.latency_ms <= t::kModelMaximumLatencyMs, "latency bound");
  const auto request = server.request();
  check(request.find("\"model\":\"gpt-test\"") != std::string::npos, "model sent");
  check(request.find("\"stream\":false") != std::string::npos, "stream false sent");
  check(request.find("\"content\":\"hello\"") != std::string::npos, "message sent");
  check(request.find("Authorization: Bearer test-secret") != std::string::npos, "bearer sent");
  check(request.find("Content-Type: application/json") != std::string::npos, "json header");

  LoopbackServer no_usage([](const std::string&) {
    return http_response(200, "OK",
                         "{\"id\":\"chatcmpl-2\",\"choices\":[{\"message\":{\"role\":"
                         "\"assistant\",\"content\":\"x\"}}]}");
  });
  auto no_usage_result = provider->chat(request_for(no_usage.endpoint()));
  check(no_usage_result.outcome == t::ModelProviderOutcome::Succeeded, "no usage success");
  check(!no_usage_result.reply.usage.known &&
            no_usage_result.reply.provider_request_id == "chatcmpl-2",
        "no usage known=false");

  LoopbackServer bad_id([](const std::string&) {
    return http_response(200, "OK",
                         "{\"id\":\"bad id with spaces\",\"choices\":[{\"message\":{"
                         "\"role\":\"assistant\",\"content\":\"x\"}}]}");
  });
  auto bad_id_result = provider->chat(request_for(bad_id.endpoint()));
  check(bad_id_result.reply.provider_request_id.empty(), "unsafe provider id dropped");
}

void status_mapping() {
  auto provider = open_provider();
  struct Case {
    int status;
    std::string_view reason;
    t::ModelProviderOutcome outcome;
    t::ErrorCode code;
    std::string_view headers;
  };
  const std::vector<Case> cases{
      {400,
       "Bad Request",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::InvalidArgument,
       {}},
      {422,
       "Unprocessable",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::InvalidArgument,
       {}},
      {401,
       "Unauthorized",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::UpstreamUnavailable,
       {}},
      {403,
       "Forbidden",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::UpstreamUnavailable,
       {}},
      {404,
       "Not Found",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::CapabilityNotSupported,
       {}},
      {405,
       "Method",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::CapabilityNotSupported,
       {}},
      {410,
       "Gone",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::CapabilityNotSupported,
       {}},
      {429, "Too Many", t::ModelProviderOutcome::DefiniteFailure, t::ErrorCode::RateLimited,
       "Retry-After: 7\r\n"},
      {418,
       "Teapot",
       t::ModelProviderOutcome::DefiniteFailure,
       t::ErrorCode::UpstreamUnavailable,
       {}},
      {500, "Server", t::ModelProviderOutcome::Unknown, t::ErrorCode::UpstreamUnavailable, {}},
      {503, "Busy", t::ModelProviderOutcome::Unknown, t::ErrorCode::UpstreamUnavailable, {}}};
  for (const auto& entry : cases) {
    LoopbackServer server([entry](const std::string&) {
      return http_response(entry.status, entry.reason,
                           "{\"error\":{\"message\":\"upstream body must not leak\"}}",
                           entry.headers);
    });
    auto result = provider->chat(request_for(server.endpoint()));
    check(result.outcome == entry.outcome, "status " + std::to_string(entry.status) + " outcome");
    check(result.failure.code == entry.code,
          "status " + std::to_string(entry.status) + " stable code");
    if (entry.status == 429) {
      check(result.failure.retry_after && result.failure.retry_after->count() == 7000,
            "429 retry-after passthrough");
    } else {
      check(!result.failure.retry_after, "no retry-after unless 429");
    }
    check(server.requests() == 1, "no provider-level retry");
  }
}

void malformed_and_unknown() {
  auto provider = open_provider();
  const std::vector<std::pair<std::string, int>> malformed{
      {"not json at all", 200},
      {"{\"choices\":[]}", 200},
      {"{\"choices\":[{\"message\":{\"content\":\"a\"}},{\"message\":{\"content\":\"b\"}}]}", 200},
      {"{\"choices\":[{\"message\":{\"content\":\"a\",\"tool_calls\":[]}}]}", 200},
      {"{\"choices\":[{\"message\":{\"content\":\"a\"}}],\"usage\":{\"prompt_tokens\":-1,"
       "\"completion_tokens\":2}}",
       200},
  };
  for (const auto& [body, status] : malformed) {
    LoopbackServer server([body = body, status = status](const std::string&) {
      return http_response(status, "OK", body);
    });
    auto result = provider->chat(request_for(server.endpoint()));
    check(result.outcome == t::ModelProviderOutcome::Unknown, "malformed is unknown");
    check(result.failure.code == t::ErrorCode::UpstreamUnavailable, "malformed stable code");
  }
  // A body larger than 1 MiB aborts the receive and stays unknown.
  const std::string huge_padding(1024 * 1024 + 16, 'x');
  LoopbackServer huge([&](const std::string&) {
    return http_response(200, "OK",
                         "{\"id\":\"big\",\"choices\":[{\"message\":{\"content\":\"" +
                             huge_padding + "\"}}]}");
  });
  auto huge_result = provider->chat(request_for(huge.endpoint()));
  check(huge_result.outcome == t::ModelProviderOutcome::Unknown, "oversized response unknown");
  check(huge_result.failure.code == t::ErrorCode::UpstreamUnavailable, "oversized stable code");
}

void timeout_and_connection() {
  auto provider = open_provider();
  LoopbackServer slow([](const std::string&) {
    std::this_thread::sleep_for(std::chrono::milliseconds{2000});
    return http_response(200, "OK", success_body());
  });
  auto slow_request = request_for(slow.endpoint());
  slow_request.timeout_ms = 1000;
  const auto started = std::chrono::steady_clock::now();
  const auto timeout = provider->chat(slow_request);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  check(timeout.outcome == t::ModelProviderOutcome::Unknown, "timeout unknown");
  check(timeout.failure.code == t::ErrorCode::Timeout, "timeout code");
  check(elapsed < std::chrono::seconds{3}, "timeout bounded");

  int closed_port = 0;
  {
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "probe bind");
    socklen_t length = sizeof(address);
    check(::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0, "probe name");
    closed_port = ntohs(address.sin_port);
    ::close(probe);
  }
  auto refused =
      provider->chat(request_for("http://127.0.0.1:" + std::to_string(closed_port) + "/v1/chat"));
  check(refused.outcome == t::ModelProviderOutcome::Unknown, "connect refused unknown");
  check(refused.failure.code == t::ErrorCode::UpstreamUnavailable, "connect refused code");
}

void request_validation() {
  LoopbackServer server(
      [](const std::string&) { return http_response(200, "OK", success_body()); });
  auto provider = open_provider();
  const auto base = request_for(server.endpoint());
  std::vector<t::ModelProviderRequest> invalid;
  {
    auto value = base;
    value.messages.clear();
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.messages.assign(t::kModelMaximumMessages + 1, {"user", "x"});
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.messages = {{"user", std::string(t::kModelMaximumRequestBytes + 1, 'a')}};
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.messages = {{"tool", "x"}};
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.timeout_ms = 999;
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.timeout_ms = t::kModelMaximumTimeoutMs + 1;
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.secret = {};
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.endpoint = "http://api.example.com/v1";
    invalid.push_back(value);
  }
  {
    auto value = base;
    value.endpoint = "https://user:pass@api.example.com/v1";
    invalid.push_back(value);
  }
  const auto baseline_requests = server.requests();
  for (const auto& value : invalid) {
    auto result = provider->chat(value);
    check(result.outcome == t::ModelProviderOutcome::DefiniteFailure, "invalid request definite");
    check(result.failure.code == t::ErrorCode::InvalidArgument, "invalid request code");
  }
  check(server.requests() == baseline_requests, "invalid request never reaches the network");
}

} // namespace

int main() {
  try {
    success_cases();
    status_mapping();
    malformed_and_unknown();
    timeout_and_connection();
    request_validation();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
