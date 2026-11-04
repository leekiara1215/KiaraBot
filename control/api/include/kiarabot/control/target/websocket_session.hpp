#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include "kiarabot/control/target/websocket_protocol.hpp"
#include "kiarabot/control/target/websocket_source_client.hpp"
#include "kiarabot/control/target/websocket_stream_store.hpp"

namespace kiarabot::control::target {

using WebSocketTransportStream = boost::beast::ssl_stream<boost::beast::tcp_stream>;
using WebSocketStream = boost::beast::websocket::stream<WebSocketTransportStream>;

// Frozen transport budgets (api/asyncapi.yaml x-control-session-policy). Every value is a bound,
// not a target: a source that cannot answer inside the authorization budget must fail the stream
// instead of delivering stale authorized data.
struct WebSocketSessionConfig {
  std::chrono::milliseconds first_frame_timeout{5000};
  std::chrono::milliseconds poll_interval{100};
  std::chrono::milliseconds authorization_budget{500};
  std::chrono::milliseconds authorization_stale{750};
  std::chrono::milliseconds write_timeout{5000};
  std::chrono::milliseconds close_timeout{1000};
  std::size_t maximum_pending_frames = 128;
  std::size_t maximum_pending_bytes = 1024 * 1024;
};

// One upgraded socket. The store may terminate this handle when the same retained session is
// taken over by a newer socket; terminate() is thread-safe and never writes inline.
class WebSocketConnection : public WebSocketSocketHandle {
public:
  // Called once from the IO thread after the HTTP Upgrade object was handed over.
  virtual void start() = 0;
  // IO-thread-only forced close used by ordered endpoint shutdown; never posts.
  virtual void terminate_now(std::uint16_t close_code) = 0;
};

std::shared_ptr<WebSocketConnection> make_web_socket_connection(
    WebSocketStream stream, boost::beast::http::request<boost::beast::http::string_body> request,
    WebSocketSelection route, std::shared_ptr<WebSocketStreamStore> store,
    std::shared_ptr<WebSocketSourcePort> source, boost::asio::thread_pool& workers,
    WebSocketSessionConfig config, std::function<void(WebSocketConnection*)> completed);

} // namespace kiarabot::control::target
