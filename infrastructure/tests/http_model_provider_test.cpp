// Boundary tests for the only external network adapter.
//
// These tests are intentionally independent from any real model service: they exercise
// configuration rejection (scheme/host/credential handling) and the response mapping
// against a loopback HTTP server that the test itself controls.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "kiarabot/infrastructure/http_model_provider.hpp"

namespace {
namespace wire = kiarabot::v1;
using kiarabot::infrastructure::HttpModelConfig;
using kiarabot::infrastructure::HttpModelProvider;

void check(bool result, const std::string& message) {
  if (!result) {
    throw std::runtime_error(message);
  }
}

wire::ErrorCode code_of(const kiarabot::core::PortResult<wire::ModelChatResponse>& result) {
  const auto* error = std::get_if<wire::Error>(&result);
  return error == nullptr ? wire::ERROR_CODE_UNSPECIFIED : error->code();
}

std::string http_response(int status, const std::string& reason, const std::string& body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason +
         "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

// Minimal one-shot HTTP server used to test the provider without external services.
// It records the request head so tests can verify exactly what crosses the network boundary.
class LoopbackServer {
public:
  using Handler = std::function<std::string(const std::string& request)>;

  explicit LoopbackServer(Handler handler) : handler_(std::move(handler)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    check(fd_ >= 0, "socket() failed");
    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    check(::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
          "bind() failed");
    check(::listen(fd_, 1) == 0, "listen() failed");
    socklen_t length = sizeof(address);
    check(::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0,
          "getsockname() failed");
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
        if (received.size() > 65536) {
          break;
        }
      }
      const auto separator = received.find("\r\n\r\n");
      if (separator != std::string::npos) {
        // Drain the request body so the client is never reset mid-write.
        const auto headers = received.substr(0, separator);
        std::size_t length = 0;
        const auto content_length = headers.find("Content-Length:");
        if (content_length != std::string::npos) {
          length = static_cast<std::size_t>(
              std::strtoul(headers.c_str() + content_length + 15, nullptr, 10));
        }
        const auto body_size = received.size() - separator - 4;
        while (body_size < length) {
          const auto read = ::recv(client, buffer, sizeof(buffer), 0);
          if (read <= 0) {
            break;
          }
          received.append(buffer, static_cast<std::size_t>(read));
          if (received.size() > 1U << 20) {
            break;
          }
        }
      }
      if (!stop_) {
        {
          std::lock_guard lock(mutex_);
          request_ = received;
        }
        const auto response = handler_(received);
        ::send(client, response.data(), response.size(), 0);
      }
      ::shutdown(client, SHUT_RDWR);
      ::close(client);
      return;
    }
  }

  int fd_{-1};
  unsigned short port_{0};
  Handler handler_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  std::string request_;
};

wire::ModelChatRequest request() {
  wire::ModelChatRequest chat;
  chat.set_model("default");
  auto* message = chat.add_messages();
  message->set_role("user");
  message->set_text("hello");
  return chat;
}

HttpModelConfig config(const std::string& endpoint) {
  return HttpModelConfig{endpoint, "test-key", "default"};
}

void expect_open_failure(const HttpModelConfig& candidate, const std::string& message) {
  const auto opened = HttpModelProvider::open(candidate);
  const auto* error = std::get_if<wire::Error>(&opened);
  check(error != nullptr, message + ": configuration must be rejected");
  check(error->code() == wire::INVALID_ARGUMENT, message + ": stable INVALID_ARGUMENT");
}

void test_configuration_boundaries() {
  expect_open_failure(HttpModelConfig{"https://127.0.0.1/v1", "", "default"}, "empty api key");
  expect_open_failure(HttpModelConfig{"https://127.0.0.1/v1", "test-key", ""}, "empty model");
  expect_open_failure(HttpModelConfig{"http://example.com/v1", "test-key", "default"},
                      "plain http must be loopback only");
  expect_open_failure(HttpModelConfig{"http://localhost/v1", "test-key", "default"},
                      "only the literal loopback address is accepted");
  const std::string userinfo = std::string("https://user:") + "password" + "@example.com/v1";
  expect_open_failure(HttpModelConfig{userinfo, "test-key", "default"},
                      "userinfo must be rejected");
  expect_open_failure(HttpModelConfig{"https://example.com/v1?token=x", "test-key", "default"},
                      "query string must be rejected");
  expect_open_failure(HttpModelConfig{"https://example.com/v1#fragment", "test-key", "default"},
                      "fragment must be rejected");
  expect_open_failure(HttpModelConfig{"not a url", "test-key", "default"}, "invalid url");
  expect_open_failure(HttpModelConfig{"https://example.com/v1", std::string("bad key"), "default"},
                      "api key with control characters");
  expect_open_failure(HttpModelConfig{"https://example.com/v1", std::string("ok\nkey"), "default"},
                      "api key newline");

  const auto https = HttpModelProvider::open(config("https://example.com/v1/chat"));
  check(std::holds_alternative<std::shared_ptr<HttpModelProvider>>(https),
        "https endpoint must be accepted");
  LoopbackServer server([](const std::string&) { return http_response(200, "OK", "{}"); });
  const auto loopback = HttpModelProvider::open(config(server.endpoint()));
  check(std::holds_alternative<std::shared_ptr<HttpModelProvider>>(loopback),
        "loopback http endpoint must be accepted");
}

void test_request_validation() {
  LoopbackServer server([](const std::string&) { return http_response(500, "Error", "{}"); });
  const auto opened = HttpModelProvider::open(config(server.endpoint()));
  check(std::holds_alternative<std::shared_ptr<HttpModelProvider>>(opened),
        "loopback provider must open");
  auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);

  check(code_of(provider.chat(request(), std::chrono::milliseconds(0))) == wire::TIMEOUT,
        "non-positive deadline must return TIMEOUT without a request");
  auto unknown_model = request();
  unknown_model.set_model("other");
  check(code_of(provider.chat(unknown_model, std::chrono::milliseconds(100))) ==
            wire::CAPABILITY_NOT_SUPPORTED,
        "unknown model must be rejected");
  wire::ModelChatRequest empty;
  empty.set_model("default");
  check(code_of(provider.chat(empty, std::chrono::milliseconds(100))) == wire::INVALID_ARGUMENT,
        "empty messages must be rejected");
  auto invalid_role = request();
  invalid_role.mutable_messages(0)->set_role("tool");
  check(code_of(provider.chat(invalid_role, std::chrono::milliseconds(100))) ==
            wire::INVALID_ARGUMENT,
        "unsupported role must be rejected");
  auto oversized = request();
  oversized.mutable_messages(0)->set_text(std::string(70000, 'a'));
  check(code_of(provider.chat(oversized, std::chrono::milliseconds(100))) == wire::INVALID_ARGUMENT,
        "oversized request must be rejected");
  check(server.request().empty(), "validation failures must not open a connection");
}

void test_network_mapping() {
  {
    LoopbackServer server([](const std::string&) {
      return http_response(200, "OK",
                           R"({"choices":[{"message":{"content":"pong"}}],)"
                           R"("usage":{"prompt_tokens":3,"completion_tokens":5}})");
    });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    const auto response = provider.chat(request(), std::chrono::milliseconds(2000));
    const auto* value = std::get_if<wire::ModelChatResponse>(&response);
    check(value != nullptr && value->text() == "pong" && value->input_tokens() == 3 &&
              value->output_tokens() == 5,
          "valid response must be decoded with usage");
    const auto sent = server.request();
    check(sent.find("Authorization: Bearer test-key") != std::string::npos,
          "api key must be sent as a bearer token");
    check(sent.find("Content-Type: application/json") != std::string::npos, "request must be JSON");
    check(sent.find("\"stream\":false") != std::string::npos, "request must be non-streaming");
  }
  {
    LoopbackServer server([](const std::string&) {
      return http_response(429, "Too Many Requests", R"({"error":"slow down"})");
    });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    check(code_of(provider.chat(request(), std::chrono::milliseconds(2000))) == wire::RATE_LIMITED,
          "429 must map to RATE_LIMITED");
  }
  {
    LoopbackServer server([](const std::string&) { return http_response(500, "Error", "{}"); });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    check(code_of(provider.chat(request(), std::chrono::milliseconds(2000))) ==
              wire::UPSTREAM_UNAVAILABLE,
          "5xx must map to UPSTREAM_UNAVAILABLE");
  }
  {
    LoopbackServer server([](const std::string&) { return http_response(200, "OK", "{not json"); });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    check(code_of(provider.chat(request(), std::chrono::milliseconds(2000))) ==
              wire::UPSTREAM_UNAVAILABLE,
          "malformed body must map to UPSTREAM_UNAVAILABLE");
  }
  {
    LoopbackServer server([](const std::string&) {
      return http_response(200, "OK",
                           R"({"choices":[{"message":{"content":"x","tool_calls":[]}}]})");
    });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    check(code_of(provider.chat(request(), std::chrono::milliseconds(2000))) ==
              wire::UPSTREAM_UNAVAILABLE,
          "tool calls must be rejected instead of being executed");
  }
  {
    LoopbackServer server([](const std::string&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(600));
      return http_response(200, "OK", R"({"choices":[{"message":{"content":"late"}}]})");
    });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    check(code_of(provider.chat(request(), std::chrono::milliseconds(100))) == wire::TIMEOUT,
          "deadline expiry must map to TIMEOUT");
  }
  {
    LoopbackServer server(
        [](const std::string&) { return http_response(200, "OK", std::string(1200000, 'x')); });
    const auto opened = HttpModelProvider::open(config(server.endpoint()));
    auto& provider = *std::get<std::shared_ptr<HttpModelProvider>>(opened);
    check(code_of(provider.chat(request(), std::chrono::milliseconds(2000))) ==
              wire::UPSTREAM_UNAVAILABLE,
          "responses above the 1 MiB bound must be rejected");
  }
}
} // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  try {
    test_configuration_boundaries();
    test_request_validation();
    test_network_mapping();
    std::cout << "http_model_provider boundary tests passed\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << "http_model_provider test failed: " << failure.what() << '\n';
    return 1;
  }
}
