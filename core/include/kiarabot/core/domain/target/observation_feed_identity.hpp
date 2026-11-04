#pragma once

#include <string>
#include <string_view>

namespace kiarabot::core::target {

// v1.2 received-message feed identity. The frozen contract requires KiaraBot-internal stable
// message/conversation identities, never a platform raw id and not a reversible mapping. These
// helpers derive deterministic UUIDv5 values from a fixed in-repo namespace:
//   message_id      = UUIDv5(kMessageFeedNamespace,      account|adapter_instance|event_id)
//   conversation_id = UUIDv5(kConversationFeedNamespace, account|adapter_instance|conversation)
// The '|' separator here is the unit separator (0x1f) so concatenation is unambiguous. Changing a
// namespace or the input order is a breaking feed identity change: the frozen vectors in
// core/tests/architecture/observation_feed_identity_test.cpp must be updated together with docs.
std::string message_feed_id(std::string_view account_id, std::string_view adapter_instance_id,
                            std::string_view platform_event_id);
std::string conversation_feed_id(std::string_view account_id, std::string_view adapter_instance_id,
                                 std::string_view platform_conversation_id);

} // namespace kiarabot::core::target
