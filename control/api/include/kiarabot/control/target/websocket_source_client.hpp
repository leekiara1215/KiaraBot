#pragma once

#include <memory>

#include "kiarabot/control/target/identity_auth_client.hpp"
#include "kiarabot/control/target/websocket_types.hpp"

namespace kiarabot::control::target {

class WebSocketSourcePort {
public:
  virtual ~WebSocketSourcePort() = default;
  // Identity-only reauthentication used to emit SESSION_READY before the first owner page for
  // channels whose ready frame carries no owner cursor (v1.2 feeds and snapshot channels).
  // Unsupported channels stay an explicit pre-ready capability failure.
  virtual WebSocketResult<WebSocketIdentity> authenticate(const WebSocketSelection& selection,
                                                          std::string_view access_token,
                                                          WebSocketDeadline deadline) = 0;
  // Every idle/replay/live poll validates the human session and current owner grant.
  // Missing source is an explicit failure, never an empty successful stream.
  virtual WebSocketResult<WebSocketPoll> poll(const WebSocketSelection& selection,
                                              std::string_view access_token,
                                              const std::optional<v1::StreamCursor>& cursor,
                                              WebSocketDeadline deadline) = 0;
  virtual void cancel() noexcept = 0;
};
std::shared_ptr<WebSocketSourcePort>
make_websocket_source_client(std::shared_ptr<IdentityAuthClient> identity,
                             const core::platform::ProcessConfig& config);

} // namespace kiarabot::control::target
