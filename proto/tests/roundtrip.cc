#include "kiarabot/v1/actions.pb.h"
#include "kiarabot/v1/events.pb.h"
#include "kiarabot/v1/plugins.pb.h"

#include <cassert>
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
  return 0;
}
