#pragma once

#include <string>

namespace kiarabot::core {
// Metadata-only projection of platform messages for the control event stream.
// It never carries message content: message_id, conversation_id and direction are
// the complete public payload described by api/asyncapi.yaml MessagePayload.
struct MessageMetadata {
  std::string message_id;
  std::string conversation_id;
  std::string direction;
};
// Best-effort observation of accepted MessageReceived events. Not an audit log:
// the implementation is bounded, in-memory and lossy on restart by design.
class IMessageMetadataSink {
public:
  virtual ~IMessageMetadataSink() = default;
  virtual bool write(const MessageMetadata& record) noexcept = 0;
};
} // namespace kiarabot::core
