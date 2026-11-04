#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/http.hpp>

#include "kiarabot/control/target/identity_auth_client.hpp"
#include "kiarabot/control/target/websocket_session.hpp"
#include "kiarabot/control/target/websocket_source_client.hpp"
#include "kiarabot/control/target/websocket_stream_store.hpp"

namespace kiarabot::control::target {

struct WebSocketUpgradeConfig {
  // Exact same-origin HTTPS allowlist. An empty list fails closed even when missing origins are
  // explicitly permitted; the deployment must name the browser origins it accepts.
  std::vector<std::string> origins;
  bool allow_missing_origin = false;
  std::size_t maximum_active_sockets = 64;
  WebSocketSessionConfig session;
  WebSocketRetainedLimits retained;
};

struct WebSocketUpgradeCheck {
  std::optional<AuthHttpResponse> rejection;
  WebSocketSelection selection;
  bool accepted() const {
    return !rejection.has_value();
  }
};

// Target Upgrade boundary. Owns the retained store, the separate WS worker budget and the single
// bounded owner source; REST sessions and their four workers never enter this object. All
// application handlers run on the transport io_context; the object itself is created/destroyed by
// the HTTPS endpoint on its owning thread.
class WebSocketEndpoint {
public:
  WebSocketEndpoint(WebSocketUpgradeConfig config, std::shared_ptr<IdentityAuthClient> identity,
                    const core::platform::ProcessConfig& process, boost::asio::io_context& io);
  ~WebSocketEndpoint();
  WebSocketEndpoint(const WebSocketEndpoint&) = delete;
  WebSocketEndpoint& operator=(const WebSocketEndpoint&) = delete;

  // HTTP-level decision; the caller keeps the request as plain HTTP when rejected.
  WebSocketUpgradeCheck
  check(const boost::beast::http::request<boost::beast::http::string_body>& request);
  // Takes ownership of the TLS stream and completes the Upgrade asynchronously on the IO thread.
  void accept(WebSocketTransportStream&& stream,
              boost::beast::http::request<boost::beast::http::string_body>&& request,
              WebSocketSelection selection);
  // Called by the owning endpoint before its IO context stops: cancels in-flight owner RPCs and
  // asks every established socket to send close 1001. The IO context must still be running.
  void prepare_shutdown();
  // Called after the IO thread joined: joins the WS worker budget and releases retained memory.
  void finish_shutdown();
  std::size_t active_sockets() const;
  std::size_t retained_sessions() const;

private:
  void close_all_on_io(std::uint16_t close_code);
  void erase(WebSocketConnection* completed);

  WebSocketUpgradeConfig config_;
  std::shared_ptr<WebSocketStreamStore> store_;
  std::shared_ptr<WebSocketSourcePort> source_;
  boost::asio::io_context& io_;
  boost::asio::thread_pool workers_;
  std::set<std::shared_ptr<WebSocketConnection>> sessions_;
  std::atomic<std::size_t> active_{0};
  bool finished_ = false;
};

} // namespace kiarabot::control::target
