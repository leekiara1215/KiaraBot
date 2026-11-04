#pragma once

#include <memory>

#include "kiarabot/control/target/identity_auth_client.hpp"
#include "kiarabot/control/target/websocket_types.hpp"

namespace kiarabot::control::target {

class WebSocketSourcePort {
public:
  virtual ~WebSocketSourcePort() = default;
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
