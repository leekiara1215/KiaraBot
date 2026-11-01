#pragma once

#include <string>
#include <string_view>

#include "kiarabot/v1/events.pb.h"

namespace kiarabot::core {
// Session/ordering identity: length-prefixed (adapter_id, account_id, conversation_id,
// conversation_type). The conversation type keeps a group and a private conversation
// that reuse the same platform numeric id in separate ordering domains; see
// doc/01-runtime-semantics.md "顺序和背压".
//
// This is a private Core key derived from the envelope, not a wire field: the type is
// read from the message payload ConversationRef, and events without a conversation
// (for example AdapterStatusChanged) use ConversationRef::TYPE_UNSPECIFIED.
inline int session_conversation_type(const kiarabot::v1::EventEnvelope& event) {
  switch (event.payload_case()) {
  case kiarabot::v1::EventEnvelope::kMessageReceived:
    return static_cast<int>(event.message_received().conversation().type());
  case kiarabot::v1::EventEnvelope::kMessageSent:
    return static_cast<int>(event.message_sent().conversation().type());
  default:
    return static_cast<int>(kiarabot::v1::ConversationRef::TYPE_UNSPECIFIED);
  }
}

// Length-prefixed components keep the key unambiguous for arbitrary identifiers and
// for conversation types created in the future. The final component is the decimal
// ConversationRef.Type value, itself length-prefixed so the format is uniform.
inline std::string session_key(std::string_view adapter_id, std::string_view account_id,
                               std::string_view conversation_id, int conversation_type) {
  std::string result;
  for (const std::string_view value : {adapter_id, account_id, conversation_id}) {
    result += std::to_string(value.size()) + ':' + std::string(value);
  }
  const std::string encoded_type = std::to_string(conversation_type);
  result += std::to_string(encoded_type.size()) + ':' + encoded_type;
  return result;
}

inline std::string session_key(const kiarabot::v1::EventEnvelope& event) {
  return session_key(event.source().adapter_id(), event.source().account_id(),
                     event.subject().conversation_id(), session_conversation_type(event));
}
} // namespace kiarabot::core
