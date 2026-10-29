#include <cassert>
#include <string>

#include "kiarabot/v1/actions.pb.h"
#include "kiarabot/v1/events.pb.h"

int main() {
  kiarabot::v1::EventEnvelope event;
  event.set_event_id("event-roundtrip");
  event.set_schema_version("kiarabot.v1");
  event.mutable_message_received()->set_message_id("message-1");

  std::string encoded;
  assert(event.SerializeToString(&encoded));
  kiarabot::v1::EventEnvelope decoded_event;
  assert(decoded_event.ParseFromString(encoded));
  assert(decoded_event.event_id() == "event-roundtrip");
  assert(decoded_event.payload_case() == kiarabot::v1::EventEnvelope::kMessageReceived);

  kiarabot::v1::ActionEnvelope action;
  action.set_action_id("action-roundtrip");
  action.set_idempotency_key("idempotency-roundtrip");
  action.mutable_send_message()->mutable_message()->set_plain_text("hello");
  encoded.clear();
  assert(action.SerializeToString(&encoded));
  kiarabot::v1::ActionEnvelope decoded_action;
  assert(decoded_action.ParseFromString(encoded));
  assert(decoded_action.action_id() == "action-roundtrip");
  assert(decoded_action.payload_case() == kiarabot::v1::ActionEnvelope::kSendMessage);
  return 0;
}
