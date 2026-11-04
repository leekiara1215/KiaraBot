#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>
#include <utility>

#include <google/protobuf/util/time_util.h>

#include "internal.hpp"

namespace kiarabot::control::target::owner_gateway {
namespace {
std::string now_iso() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  google::protobuf::Timestamp stamp;
  stamp.set_seconds(seconds.count());
  stamp.set_nanos(static_cast<std::int32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch() - seconds)
          .count()));
  return json_time(stamp);
}
Json component(std::string_view name, bool ready) {
  return {{"component", std::string{name}},
          {"state", ready ? "HEALTHY" : "DEGRADED"},
          {"checked_at", now_iso()}};
}
std::string health_body(const HealthObservation& observation) {
  const bool degraded =
      !observation.runtime_ready || !observation.scheduler_ready || !observation.model_ready;
  Json value;
  value["state"] = observation.identity_ready ? (degraded ? "DEGRADED" : "HEALTHY") : "UNHEALTHY";
  value["components"] = Json::array({component("identity", observation.identity_ready),
                                     component("runtime", observation.runtime_ready),
                                     component("scheduler", observation.scheduler_ready),
                                     component("model", observation.model_ready)});
  return value.dump();
}

} // namespace

std::optional<OwnerHttpResult> handle_health(const RequestContext& context, const Ports& ports) {
  const auto& request = context.request_;
  const auto& path = context.path_;
  const auto& request_id = context.request_id_;
  if (path == "/health/live" || path == "/health/ready") {
    if (request.method != "GET" && request.method != "HEAD") {
      return context.method_not_allowed();
    }
    if (path == "/health/live") {
      return OwnerGateway::live();
    }
    const auto ready_state = ports.health_ ? ports.health_->observe().identity_ready : false;
    return OwnerGateway::ready(ready_state, request_id);
  }
  if (path == "/api/v1/health") {
    if (request.method != "GET" && request.method != "HEAD") {
      return context.method_not_allowed();
    }
    if (request.access_token.empty()) {
      return context.error(wire::UNAUTHENTICATED, false);
    }
    // health.read (Permission 50) is a sensitive read: validate the caller through the same
    // one-use Identity delegation as every other owner call before exposing component detail.
    auto delegation = context.delegate(ports.delegation_, "control", wire::PERMISSION_HEALTH_READ,
                                       control_catalog_scope());
    if (const auto* failure = std::get_if<OwnerFailure>(&delegation)) {
      return context.failure(*failure);
    }
    if (!ports.health_) {
      return OwnerHttpResult{503, health_body(HealthObservation{})};
    }
    const auto observation = ports.health_->observe();
    return OwnerHttpResult{static_cast<unsigned int>(observation.identity_ready ? 200 : 503),
                           health_body(observation)};
  }

  return std::nullopt;
}

} // namespace kiarabot::control::target::owner_gateway
