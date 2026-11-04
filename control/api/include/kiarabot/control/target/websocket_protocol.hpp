#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/control/target/websocket_types.hpp"

namespace kiarabot::control::target {

// Application framing only. RFC6455 framing, masking, UTF-8 and Upgrade are Beast's responsibility.
WebSocketResult<WebSocketSelection> websocket_route(std::string_view target);
WebSocketResult<WebSocketFirstFrame> websocket_first_frame(std::string_view text,
                                                           const WebSocketSelection& route);
bool websocket_origin(std::string_view origin, bool present,
                      const std::vector<std::string>& allowed, bool allow_missing);
bool websocket_same_scope(const v1::ResourceScope& left, const v1::ResourceScope& right);
bool websocket_same_cursor(const v1::StreamCursor& left, const v1::StreamCursor& right);
WebSocketResult<std::string> websocket_cursor_json(const v1::StreamCursor& cursor);
WebSocketResult<std::string> websocket_operation_event(const v1::OperationChange& change,
                                                       std::uint64_t sequence);
unsigned short websocket_close_code(WebSocketFailure failure);

} // namespace kiarabot::control::target
