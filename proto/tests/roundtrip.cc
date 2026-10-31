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

  kiarabot::v1::ActionEnvelope reaction;
  reaction.set_action_id("reaction-roundtrip");
  reaction.set_idempotency_key("idempotency-roundtrip");
  auto* react = reaction.mutable_react_message();
  react->set_message_id("9001");
  react->set_reaction("like");
  react->mutable_conversation()->set_id("456");
  react->mutable_conversation()->set_account_id("123");
  react->mutable_conversation()->set_type(kiarabot::v1::ConversationRef::GROUP);
  encoded.clear();
  assert(reaction.SerializeToString(&encoded));
  kiarabot::v1::ActionEnvelope decoded_reaction;
  assert(decoded_reaction.ParseFromString(encoded));
  assert(decoded_reaction.payload_case() == kiarabot::v1::ActionEnvelope::kReactMessage);
  assert(decoded_reaction.react_message().message_id() == "9001");
  assert(decoded_reaction.react_message().reaction() == "like");
  assert(decoded_reaction.react_message().conversation().id() == "456");
  assert(decoded_reaction.react_message().conversation().type() ==
         kiarabot::v1::ConversationRef::GROUP);
  assert(kiarabot::v1::ReactMessage::descriptor()->FindFieldByName("conversation")->number() == 3);

  kiarabot::v1::ActionEnvelope deletion;
  deletion.set_action_id("deletion-roundtrip");
  deletion.set_idempotency_key("idempotency-roundtrip");
  auto* remove = deletion.mutable_delete_message();
  remove->set_message_id("9002");
  remove->mutable_conversation()->set_id("456");
  remove->mutable_conversation()->set_account_id("123");
  remove->mutable_conversation()->set_type(kiarabot::v1::ConversationRef::GROUP);
  encoded.clear();
  assert(deletion.SerializeToString(&encoded));
  kiarabot::v1::ActionEnvelope decoded_deletion;
  assert(decoded_deletion.ParseFromString(encoded));
  assert(decoded_deletion.payload_case() == kiarabot::v1::ActionEnvelope::kDeleteMessage);
  assert(decoded_deletion.delete_message().message_id() == "9002");
  assert(decoded_deletion.delete_message().conversation().id() == "456");
  assert(decoded_deletion.delete_message().conversation().type() ==
         kiarabot::v1::ConversationRef::GROUP);
  assert(kiarabot::v1::DeleteMessage::descriptor()->FindFieldByName("conversation")->number() == 2);

  kiarabot::v1::ActionEnvelope mute;
  mute.set_action_id("mute-roundtrip");
  mute.set_idempotency_key("idempotency-roundtrip");
  auto* mute_payload = mute.mutable_mute_member();
  mute_payload->set_group_id("456");
  mute_payload->set_user_id("789");
  mute_payload->set_duration_seconds(600);
  encoded.clear();
  assert(mute.SerializeToString(&encoded));
  kiarabot::v1::ActionEnvelope decoded_mute;
  assert(decoded_mute.ParseFromString(encoded));
  assert(decoded_mute.payload_case() == kiarabot::v1::ActionEnvelope::kMuteMember);
  assert(decoded_mute.mute_member().group_id() == "456");
  assert(decoded_mute.mute_member().user_id() == "789");
  assert(decoded_mute.mute_member().duration_seconds() == 600);

  kiarabot::v1::ActionEnvelope kick;
  kick.set_action_id("kick-roundtrip");
  kick.set_idempotency_key("idempotency-roundtrip");
  auto* kick_payload = kick.mutable_kick_member();
  kick_payload->set_group_id("456");
  kick_payload->set_user_id("789");
  kick_payload->set_reason("spam");
  encoded.clear();
  assert(kick.SerializeToString(&encoded));
  kiarabot::v1::ActionEnvelope decoded_kick;
  assert(decoded_kick.ParseFromString(encoded));
  assert(decoded_kick.payload_case() == kiarabot::v1::ActionEnvelope::kKickMember);
  assert(decoded_kick.kick_member().reason() == "spam");

  kiarabot::v1::ActionEnvelope approve;
  approve.set_action_id("approve-roundtrip");
  approve.set_idempotency_key("idempotency-roundtrip");
  auto* approve_payload = approve.mutable_approve_request();
  approve_payload->set_request_id("request-1");
  approve_payload->set_approve(true);
  approve_payload->set_reason("ok");
  encoded.clear();
  assert(approve.SerializeToString(&encoded));
  kiarabot::v1::ActionEnvelope decoded_approve;
  assert(decoded_approve.ParseFromString(encoded));
  assert(decoded_approve.payload_case() == kiarabot::v1::ActionEnvelope::kApproveRequest);
  assert(decoded_approve.approve_request().request_id() == "request-1");
  assert(decoded_approve.approve_request().approve());

  kiarabot::v1::EventEnvelope friend_request;
  friend_request.set_event_id("friend-roundtrip");
  friend_request.set_trace_id("friend-roundtrip");
  friend_request.mutable_friend_request()->set_user_id("456");
  friend_request.mutable_friend_request()->set_comment("hello");
  friend_request.mutable_friend_request()->set_request_id("request-1");
  encoded.clear();
  assert(friend_request.SerializeToString(&encoded));
  kiarabot::v1::EventEnvelope decoded_friend_request;
  assert(decoded_friend_request.ParseFromString(encoded));
  assert(decoded_friend_request.payload_case() == kiarabot::v1::EventEnvelope::kFriendRequest);
  assert(decoded_friend_request.friend_request().request_id() == "request-1");
  assert(kiarabot::v1::FriendRequest::descriptor()->FindFieldByName("request_id")->number() == 3);

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

  kiarabot::v1::CoreToAdapter drain;
  drain.mutable_shutdown()->set_reason("deploy");
  drain.mutable_shutdown()->set_drain_timeout_ms(5000);
  encoded.clear();
  assert(drain.SerializeToString(&encoded));
  kiarabot::v1::CoreToAdapter decoded_drain;
  assert(decoded_drain.ParseFromString(encoded));
  assert(decoded_drain.body_case() == kiarabot::v1::CoreToAdapter::kShutdown);
  assert(decoded_drain.shutdown().reason() == "deploy");
  assert(decoded_drain.shutdown().drain_timeout_ms() == 5000);
  assert(kiarabot::v1::CoreToAdapter::descriptor()->FindFieldByName("shutdown")->number() == 5);

  kiarabot::v1::CoreToPlugin stream_failure;
  stream_failure.mutable_error()->set_code(kiarabot::v1::PERMISSION_DENIED);
  encoded.clear();
  assert(stream_failure.SerializeToString(&encoded));
  kiarabot::v1::CoreToPlugin decoded_stream_failure;
  assert(decoded_stream_failure.ParseFromString(encoded));
  assert(decoded_stream_failure.error().code() == kiarabot::v1::PERMISSION_DENIED);
  assert(kiarabot::v1::CoreToPlugin::descriptor()->FindFieldByName("error")->number() == 6);

  kiarabot::v1::CoreToPlugin plugin_drain;
  plugin_drain.mutable_shutdown()->set_reason("deploy");
  encoded.clear();
  assert(plugin_drain.SerializeToString(&encoded));
  kiarabot::v1::CoreToPlugin decoded_plugin_drain;
  assert(decoded_plugin_drain.ParseFromString(encoded));
  assert(decoded_plugin_drain.body_case() == kiarabot::v1::CoreToPlugin::kShutdown);
  assert(decoded_plugin_drain.shutdown().reason() == "deploy");
  assert(kiarabot::v1::CoreToPlugin::descriptor()->FindFieldByName("shutdown")->number() == 8);

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
