#include <cassert>
#include <string>

#include <google/protobuf/descriptor.h>

#include "kiarabot/v1/actions.pb.h"
#include "kiarabot/v1/adapters.pb.h"
#include "kiarabot/v1/events.pb.h"
#include "kiarabot/v1/plugins.pb.h"

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

  kiarabot::v1::CoreToAdapter confirmation;
  confirmation.mutable_event_ack()->set_event_id("event-roundtrip");
  confirmation.mutable_event_ack()->set_status(kiarabot::v1::AdapterEventAck::PERSISTED);
  encoded.clear();
  assert(confirmation.SerializeToString(&encoded));
  kiarabot::v1::CoreToAdapter decoded_confirmation;
  assert(decoded_confirmation.ParseFromString(encoded));
  assert(decoded_confirmation.event_ack().status() == kiarabot::v1::AdapterEventAck::PERSISTED);
  assert(kiarabot::v1::CoreToAdapter::descriptor()->FindFieldByName("event_ack")->number() == 4);

  kiarabot::v1::CoreToPlugin stream_failure;
  stream_failure.mutable_error()->set_code(kiarabot::v1::PERMISSION_DENIED);
  encoded.clear();
  assert(stream_failure.SerializeToString(&encoded));
  kiarabot::v1::CoreToPlugin decoded_stream_failure;
  assert(decoded_stream_failure.ParseFromString(encoded));
  assert(decoded_stream_failure.error().code() == kiarabot::v1::PERMISSION_DENIED);
  assert(kiarabot::v1::CoreToPlugin::descriptor()->FindFieldByName("error")->number() == 6);

  kiarabot::v1::PluginToCore plugin_message;
  plugin_message.mutable_action()->mutable_action()->set_action_id("action-roundtrip");
  assert(plugin_message.body_case() == kiarabot::v1::PluginToCore::kAction);
  encoded.clear();
  assert(plugin_message.SerializeToString(&encoded));
  kiarabot::v1::PluginToCore decoded_plugin_message;
  assert(decoded_plugin_message.ParseFromString(encoded));
  assert(decoded_plugin_message.body_case() == kiarabot::v1::PluginToCore::kAction);
  kiarabot::v1::PluginToCore request;
  auto* host = request.mutable_host_request();
  host->set_request_id("request-1");
  host->set_trace_id("trace-1");
  host->set_idempotency_key("idempotency-key-1");
  host->mutable_task_create()->set_task_id("task-1");
  host->mutable_task_create()->mutable_local_time()->set_time_zone("Asia/Shanghai");
  encoded.clear();
  assert(request.SerializeToString(&encoded));
  kiarabot::v1::PluginToCore decoded_request;
  assert(decoded_request.ParseFromString(encoded));
  assert(decoded_request.host_request().task_create().local_time().time_zone() == "Asia/Shanghai");

  kiarabot::v1::CoreToPlugin response;
  response.mutable_host_response()->set_request_id("request-1");
  response.mutable_host_response()->mutable_error()->set_code(kiarabot::v1::RATE_LIMITED);
  encoded.clear();
  assert(response.SerializeToString(&encoded));
  kiarabot::v1::CoreToPlugin decoded_response;
  assert(decoded_response.ParseFromString(encoded));
  assert(decoded_response.host_response().error().code() == kiarabot::v1::RATE_LIMITED);
  assert(decoded_response.host_response().result_case() ==
         kiarabot::v1::HostResponse::RESULT_NOT_SET);
  kiarabot::v1::PluginToCore action_call;
  action_call.mutable_register_()->add_supported_features("action.result.v1");
  assert(action_call.SerializeToString(&encoded));
  kiarabot::v1::PluginToCore parsed_call;
  assert(parsed_call.ParseFromString(encoded));
  assert(parsed_call.register_().supported_features(0) == "action.result.v1");
  action_call.mutable_action()->set_request_id("submit-1");
  action_call.mutable_action()->mutable_action()->set_action_id("action-1");
  assert(action_call.SerializeToString(&encoded));
  assert(parsed_call.ParseFromString(encoded));
  assert(parsed_call.action().request_id() == "submit-1");
  auto* lookup = action_call.mutable_action_query();
  lookup->set_request_id("query-1");
  lookup->set_action_id("action-1");
  lookup->set_idempotency_key("key-1");
  assert(lookup->selector_case() == kiarabot::v1::PluginActionQuery::kIdempotencyKey);
  assert(action_call.SerializeToString(&encoded));
  assert(parsed_call.ParseFromString(encoded));
  assert(parsed_call.action_query().idempotency_key() == "key-1");
  action_call.mutable_action_cancel()->set_action_id("action-1");
  assert(action_call.SerializeToString(&encoded));
  assert(parsed_call.ParseFromString(encoded));
  assert(parsed_call.action_cancel().action_id() == "action-1");
  response.mutable_register_result()->add_enabled_features("action.result.v1");
  assert(response.SerializeToString(&encoded));
  assert(decoded_response.ParseFromString(encoded));
  assert(decoded_response.register_result().enabled_features(0) == "action.result.v1");
  response.mutable_action_response()->set_request_id("query-1");
  response.mutable_action_response()->mutable_result()->set_status(
      static_cast<kiarabot::v1::ActionStatus>(12345));
  assert(response.SerializeToString(&encoded));
  assert(decoded_response.ParseFromString(encoded));
  assert(decoded_response.action_response().result().status() == 12345);
  response.mutable_action_response()->mutable_error()->set_code(kiarabot::v1::TIMEOUT);
  assert(!response.action_response().has_result());
  assert(response.SerializeToString(&encoded));
  assert(decoded_response.ParseFromString(encoded));
  assert(decoded_response.action_response().error().code() == kiarabot::v1::TIMEOUT);
  assert(!decoded_response.action_response().has_result());
  assert(kiarabot::v1::PluginToCore::descriptor()->FindFieldByName("action")->number() == 3);
  assert(kiarabot::v1::PluginActionRequest::descriptor()->FindFieldByName("action")->number() == 1);
  assert(kiarabot::v1::PluginToCore::descriptor()->FindFieldByName("action_query")->number() == 6);
  assert(kiarabot::v1::CoreToPlugin::descriptor()->FindFieldByName("action_response")->number() ==
         7);
  return 0;
}
