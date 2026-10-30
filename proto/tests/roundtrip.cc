#include "kiarabot/v1/actions.pb.h"
#include "kiarabot/v1/adapters.pb.h"
#include "kiarabot/v1/events.pb.h"
#include "kiarabot/v1/plugins.pb.h"

#include <cassert>
#include <google/protobuf/descriptor.h>
#include <string>

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

  kiarabot::v1::EventEnvelope plugin_event;
  auto* plugin_status = plugin_event.mutable_plugin_status_changed();
  plugin_status->set_plugin_id("plugin-1");
  assert(!plugin_status->has_lifecycle_state());
  encoded.clear();
  assert(plugin_event.SerializeToString(&encoded));
  kiarabot::v1::EventEnvelope decoded_plugin_event;
  assert(decoded_plugin_event.ParseFromString(encoded));
  assert(!decoded_plugin_event.plugin_status_changed().has_lifecycle_state());
  plugin_status->set_lifecycle_state(kiarabot::v1::PLUGIN_STATE_RUNNING);
  encoded.clear();
  assert(plugin_event.SerializeToString(&encoded));
  assert(decoded_plugin_event.ParseFromString(encoded));
  assert(decoded_plugin_event.plugin_status_changed().has_lifecycle_state());
  assert(decoded_plugin_event.plugin_status_changed().lifecycle_state() ==
         kiarabot::v1::PLUGIN_STATE_RUNNING);
  plugin_status->set_lifecycle_state(12345);
  encoded.clear();
  assert(plugin_event.SerializeToString(&encoded));
  assert(decoded_plugin_event.ParseFromString(encoded));
  assert(decoded_plugin_event.plugin_status_changed().lifecycle_state() == 12345);
  plugin_status->set_lifecycle_state(kiarabot::v1::PLUGIN_STATE_UNSPECIFIED);
  encoded.clear();
  assert(plugin_event.SerializeToString(&encoded));
  assert(decoded_plugin_event.ParseFromString(encoded));
  assert(decoded_plugin_event.plugin_status_changed().has_lifecycle_state());
  assert(decoded_plugin_event.plugin_status_changed().lifecycle_state() == 0);

  const google::protobuf::EnumDescriptor* plugin_states = kiarabot::v1::PluginState_descriptor();
  const char* const published_plugin_states[] = {
      "PLUGIN_STATE_UNSPECIFIED", "PLUGIN_STATE_DISABLED",    "PLUGIN_STATE_STARTING",
      "PLUGIN_STATE_READY",       "PLUGIN_STATE_RUNNING",     "PLUGIN_STATE_STOPPING",
      "PLUGIN_STATE_STOPPED",     "PLUGIN_STATE_UNAVAILABLE", "PLUGIN_STATE_FAILED"};
  for (int index = 0; index < 9; ++index) {
    const google::protobuf::EnumValueDescriptor* value =
        plugin_states->FindValueByName(published_plugin_states[index]);
    assert(value != nullptr);
    assert(value->number() == index);
  }

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

  kiarabot::v1::ActionResult action_result;
  action_result.set_action_id("action-roundtrip");
  action_result.set_status(kiarabot::v1::FAILED);
  action_result.mutable_error()->set_code(kiarabot::v1::INTERNAL);
  encoded.clear();
  assert(action_result.SerializeToString(&encoded));
  kiarabot::v1::ActionResult decoded_result;
  assert(decoded_result.ParseFromString(encoded));
  assert(decoded_result.status() == kiarabot::v1::FAILED);
  assert(decoded_result.error().code() == kiarabot::v1::INTERNAL);

  kiarabot::v1::AdapterToCore adapter_message;
  adapter_message.mutable_event()->set_event_id("event-roundtrip");
  assert(adapter_message.body_case() == kiarabot::v1::AdapterToCore::kEvent);
  encoded.clear();
  assert(adapter_message.SerializeToString(&encoded));
  kiarabot::v1::AdapterToCore decoded_adapter_message;
  assert(decoded_adapter_message.ParseFromString(encoded));
  assert(decoded_adapter_message.body_case() == kiarabot::v1::AdapterToCore::kEvent);

  kiarabot::v1::PluginToCore plugin_message;
  plugin_message.mutable_action()->mutable_action()->set_action_id("action-roundtrip");
  assert(plugin_message.body_case() == kiarabot::v1::PluginToCore::kAction);
  encoded.clear();
  assert(plugin_message.SerializeToString(&encoded));
  kiarabot::v1::PluginToCore decoded_plugin_message;
  assert(decoded_plugin_message.ParseFromString(encoded));
  assert(decoded_plugin_message.body_case() == kiarabot::v1::PluginToCore::kAction);
  return 0;
}
