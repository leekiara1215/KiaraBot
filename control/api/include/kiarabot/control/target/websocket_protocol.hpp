#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/control/target/websocket_types.hpp"

namespace kiarabot::control::target {

// Wire limits shared with api/asyncapi.yaml. Beast owns RFC6455 framing, close handshake,
// masking, UTF-8 and the Upgrade itself; these values only bound the Control application layer.
constexpr std::size_t kMaximumFirstFrameBytes = 65536;
constexpr std::size_t kMaximumEventBytes = 65536;
constexpr std::size_t kMaximumJsonDepth = 8;
constexpr std::uint64_t kMaximumSafeSequence = 9007199254740991ULL;

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
// v1.2 scoped feed events: StreamEnvelope + the frozen AsyncAPI payload shape. The owners' safe
// metadata is projected one-to-one; malformed/missing fields are an internal producer failure.
WebSocketResult<std::string> websocket_feed_event(const v1::LogChange& change,
                                                  std::uint64_t sequence);
WebSocketResult<std::string> websocket_feed_event(const v1::TaskChange& change,
                                                  std::uint64_t sequence);
WebSocketResult<std::string> websocket_feed_event(const v1::MessageMetadataChange& change,
                                                  std::uint64_t sequence);
// Snapshot channels. health events never fabricate component health: when Identity is not
// confirmed the payload withholds component details and reports UNHEALTHY. Adapter/plugin events
// project only the frozen target record fields.
WebSocketResult<std::string> websocket_health_event(const WebSocketHealthObservation& observation,
                                                    std::uint64_t sequence);
WebSocketResult<std::string> websocket_adapter_event(const v1::AdapterRecord& record,
                                                     std::uint64_t sequence);
WebSocketResult<std::string> websocket_plugin_event(const v1::InstanceRecord& record,
                                                    std::uint64_t sequence);
// Stable value signature for change detection: the frozen target fields minus observed_at.
std::optional<std::string> websocket_adapter_signature(const v1::AdapterRecord& record);
std::optional<std::string> websocket_plugin_signature(const v1::InstanceRecord& record);
// Server frames. owner_cursor is required for the operations channel (OperationSessionReady);
// channels without an owner cursor emit the plain SessionReady shape.
WebSocketResult<std::string>
websocket_session_ready(std::string_view session_id, std::uint64_t last_sequence,
                        const std::optional<v1::StreamCursor>& owner_cursor);
// reason must be one of the frozen RESYNC_REQUIRED reasons: CURSOR_EXPIRED, INVALID_CURSOR,
// SERVER_RESTART. Free-form text is refused.
WebSocketResult<std::string> websocket_resync_required(WebSocketResyncReason reason);
unsigned short websocket_close_code(WebSocketFailure failure);
// Closed-shape validation of a delivered OperationEnvelope (schema_version/sequence/occurred_at,
// uint64 strings, BLOCKED reason+next_attempt_at, participant entries and its owner_cursor).
// Used by tests and by the transport's pre-delivery assertion; it never re-authorizes a source.
bool websocket_valid_event(std::string_view json);

} // namespace kiarabot::control::target
