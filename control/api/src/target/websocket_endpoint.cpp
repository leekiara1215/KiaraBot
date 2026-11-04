#include "kiarabot/control/target/websocket_endpoint.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/post.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/rand.h>

#include "kiarabot/control/target/websocket_protocol.hpp"

namespace kiarabot::control::target {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using Error = boost::system::error_code;

std::string random_identifier() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return "control-websocket";
  }
  constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result += '-';
    }
    result += kDigits[bytes[index] >> 4U];
    result += kDigits[bytes[index] & 0x0fU];
  }
  return result;
}
AuthHttpResponse rejection(unsigned int status, std::string_view code, std::string_view message) {
  const auto request_id = random_identifier();
  const auto trace_id = random_identifier();
  AuthHttpResponse result;
  result.status = status;
  result.body = std::string{"{\"code\":\""} + std::string{code} + "\",\"message\":\"" +
                std::string{message} + "\",\"request_id\":\"" + request_id + "\",\"trace_id\":\"" +
                trace_id + "\",\"retryable\":false,\"details\":{}}";
  result.headers.emplace_back("content-type", "application/json");
  result.headers.emplace_back("cache-control", "no-store");
  result.headers.emplace_back("x-request-id", request_id);
  result.headers.emplace_back("x-trace-id", trace_id);
  return result;
}
bool websocket_key(std::string_view value) {
  if (value.size() != 24 || value[22] != '=' || value[23] != '=') {
    return false;
  }
  return std::all_of(value.begin(), value.begin() + 22, [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
           c == '/';
  });
}
bool contains_upgrade_token(std::string_view value) {
  std::string lower;
  lower.reserve(value.size());
  for (const auto c : value) {
    lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
  }
  return lower.find("upgrade") != std::string::npos;
}
} // namespace

WebSocketEndpoint::WebSocketEndpoint(WebSocketUpgradeConfig config,
                                     std::shared_ptr<IdentityAuthClient> identity,
                                     const core::platform::ProcessConfig& process,
                                     asio::io_context& io)
    : config_(std::move(config)), store_(std::make_shared<WebSocketStreamStore>(config_.retained)),
      source_(make_websocket_source_client(std::move(identity), process)), io_(io), workers_(4) {}

WebSocketEndpoint::~WebSocketEndpoint() {
  if (!finished_) {
    source_->cancel();
    finish_shutdown();
  }
}

WebSocketUpgradeCheck WebSocketEndpoint::check(const http::request<http::string_body>& request) {
  WebSocketUpgradeCheck result;
  const auto reject = [&result](unsigned int status, std::string_view code,
                                std::string_view message) {
    result.rejection = rejection(status, code, message);
    return result;
  };
  if (request.method() != http::verb::get) {
    return reject(405, "METHOD_NOT_ALLOWED", "WebSocket routes accept GET only");
  }
  if (!websocket::is_upgrade(request)) {
    return reject(400, "INVALID_ARGUMENT", "Invalid WebSocket upgrade request");
  }
  const auto route = websocket_route(std::string{request.target()});
  if (const auto* failure = std::get_if<WebSocketFailure>(&route)) {
    return failure->code == v1::NOT_FOUND
               ? reject(404, "NOT_FOUND", "Unknown WebSocket route")
               : reject(400, "INVALID_ARGUMENT", "Invalid WebSocket route or route selector");
  }
  if (request[http::field::sec_websocket_version] != "13") {
    return reject(400, "INVALID_ARGUMENT", "Unsupported WebSocket version");
  }
  if (!websocket_key(request[http::field::sec_websocket_key])) {
    return reject(400, "INVALID_ARGUMENT", "Invalid WebSocket key");
  }
  if (!contains_upgrade_token(request[http::field::connection])) {
    return reject(400, "INVALID_ARGUMENT", "Invalid WebSocket connection header");
  }
  if (!request.body().empty() ||
      (request.has_content_length() && request.payload_size().value_or(0) > 0)) {
    return reject(400, "INVALID_ARGUMENT", "WebSocket upgrade must not carry a body");
  }
  std::string origin;
  std::size_t origins = 0;
  for (const auto& field : request.base()) {
    if (field.name() == http::field::origin) {
      ++origins;
      origin = std::string{field.value()};
    }
  }
  if (origins > 1) {
    return reject(400, "INVALID_ARGUMENT", "Ambiguous Origin header");
  }
  if (!websocket_origin(origin, origins == 1, config_.origins, config_.allow_missing_origin)) {
    return reject(403, "PERMISSION_DENIED", "Origin is not allowed for the control WebSocket");
  }
  if (active_.load(std::memory_order_acquire) >= config_.maximum_active_sockets) {
    return reject(429, "RATE_LIMITED", "Too many active control WebSocket sockets");
  }
  result.selection = std::get<WebSocketSelection>(route);
  return result;
}

void WebSocketEndpoint::accept(WebSocketTransportStream&& stream,
                               http::request<http::string_body>&& request,
                               WebSocketSelection selection) {
  if (finished_) {
    Error ignored;
    beast::get_lowest_layer(stream).socket().close(ignored);
    return;
  }
  auto session = make_web_socket_connection(
      WebSocketStream(std::move(stream)), std::move(request), std::move(selection), store_, source_,
      workers_, config_.session, [this](WebSocketConnection* completed) { erase(completed); });
  sessions_.insert(session);
  active_.fetch_add(1, std::memory_order_acq_rel);
  session->start();
}

void WebSocketEndpoint::prepare_shutdown() {
  source_->cancel();
  asio::post(io_, [this] { close_all_on_io(1001); });
}

void WebSocketEndpoint::finish_shutdown() {
  if (finished_) {
    return;
  }
  finished_ = true;
  sessions_.clear();
  active_.store(0, std::memory_order_release);
  workers_.stop();
  workers_.join();
  store_->clear();
}

void WebSocketEndpoint::close_all_on_io(std::uint16_t close_code) {
  const std::vector<std::shared_ptr<WebSocketConnection>> sessions(sessions_.begin(),
                                                                   sessions_.end());
  for (const auto& session : sessions) {
    session->terminate_now(close_code);
  }
}

void WebSocketEndpoint::erase(WebSocketConnection* completed) {
  const auto found =
      std::find_if(sessions_.begin(), sessions_.end(),
                   [completed](const auto& owned) { return owned.get() == completed; });
  if (found != sessions_.end()) {
    sessions_.erase(found);
    active_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

std::size_t WebSocketEndpoint::active_sockets() const {
  return active_.load(std::memory_order_acquire);
}

std::size_t WebSocketEndpoint::retained_sessions() const {
  return store_->retained_sessions();
}

} // namespace kiarabot::control::target
