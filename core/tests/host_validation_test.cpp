#include "kiarabot/core/host_validation.hpp"
#include "kiarabot/core/ports.hpp"

#include <cassert>
#include <chrono>

int main() {
  using namespace std::chrono_literals;
  const auto now = std::chrono::system_clock::time_point{100s};
  kiarabot::v1::HostRequest request;
  request.set_request_id("request-1");
  request.set_trace_id("trace-1");
  request.mutable_deadline()->set_seconds(130);
  request.mutable_storage_set()->set_key("key");
  request.set_idempotency_key("0123456789abcdef");
  assert(!kiarabot::core::validate_host_request(request, now));
  request.clear_idempotency_key();
  auto error = kiarabot::core::validate_host_request(request, now);
  assert(error && error->code() == kiarabot::v1::INVALID_ARGUMENT);
  assert(error->request_id() == "request-1");
  request.mutable_deadline()->set_seconds(99);
  error = kiarabot::core::validate_host_request(request, now);
  assert(error && error->code() == kiarabot::v1::TIMEOUT);
  request.mutable_deadline()->set_seconds(401);
  error = kiarabot::core::validate_host_request(request, now);
  assert(error && error->code() == kiarabot::v1::INVALID_ARGUMENT);
  request.mutable_deadline()->set_seconds(130);
  request.mutable_model_chat()->set_model("model");
  request.mutable_model_chat()->add_messages()->set_role("user");
  assert(!kiarabot::core::validate_host_request(request, now));
  request.mutable_model_chat()->mutable_messages(0)->set_role("unknown");
  error = kiarabot::core::validate_host_request(request, now);
  assert(error && error->code() == kiarabot::v1::INVALID_ARGUMENT);
  return 0;
}
