#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
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
  // Snapshot directory polls combine per-page session validation, delegation and the owner list
  // call; the frozen session contract allows a total under one second, so directory polling gets a
  // wider ceiling than the single-call identity budget while each upstream call stays bounded.
  std::chrono::milliseconds snapshot_poll_budget{900};
  std::chrono::milliseconds authorization_stale{750};
  // Bounded stale-retention window for transient snapshot-owner failures. A single poll timeout
  // keeps the session and retries; a prolonged outage still fails closed with 1011 (no cache).
  std::chrono::milliseconds snapshot_transient_grace{2000};
  std::chrono::milliseconds write_timeout{5000};
  std::chrono::milliseconds close_timeout{1000};
  std::size_t maximum_pending_frames = 128;
  std::size_t maximum_pending_bytes = 1024 * 1024;
};

// True when SESSION_READY for this channel can be emitted from current Identity validation alone:
// the ready frame carries no owner cursor, so the first owner page/snapshot poll runs after ready.
// Operations need the owner head cursor in OperationSessionReady and keep the pre-ready poll.
bool websocket_ready_without_owner_data(std::string_view channel);

// Snapshot channels (health/adapters/plugins) keep the established session and the last known
// values on transient owner/session-availability failures; the next poll retries and emits any
// recovered value. Authentication/authorization/scope/snapshot-integrity failures still close.
bool websocket_snapshot_transient(std::string_view channel, v1::ErrorCode code);

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
    boost::asio::io_context& io, WebSocketSessionConfig config,
    std::function<void(WebSocketConnection*)> completed);

} // namespace kiarabot::control::target
