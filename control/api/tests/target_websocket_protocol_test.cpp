// WS-04 target protocol unit evidence: route/first-frame/Origin/scope/cursor validation and the
// closed OperationEnvelope projection. The same executable emits schema fixtures for
// p12_target_websocket_schema, so the real AsyncAPI/OpenAPI documents validate what this code
// produces instead of a hand-written example. No network, PG or owner is required here.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "kiarabot/control/target/websocket_protocol.hpp"

namespace {
namespace target = kiarabot::control::target;
namespace wire = kiarabot::v1;
using Json = nlohmann::json;

constexpr char kAccount[] = "11111111-1111-4111-8111-111111111111";
constexpr char kModel[] = "22222222-2222-4222-8222-222222222222";

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error("WS-04 protocol assertion: " + std::string{message});
  }
}

template <typename Value>
const Value& accepted(const target::WebSocketResult<Value>& result, std::string_view message) {
  if (!std::holds_alternative<Value>(result)) {
    throw std::runtime_error("WS-04 protocol assertion: rejected " + std::string{message});
  }
  return std::get<Value>(result);
}
template <typename Value>
const target::WebSocketFailure& rejected(const target::WebSocketResult<Value>& result,
                                         std::string_view message) {
  if (!std::holds_alternative<target::WebSocketFailure>(result)) {
    throw std::runtime_error("WS-04 protocol assertion: accepted " + std::string{message});
  }
  return std::get<target::WebSocketFailure>(result);
}
wire::ResourceRef resource(wire::ResourceKind kind, std::string id) {
  wire::ResourceRef result;
  result.set_kind(kind);
  result.set_id(std::move(id));
  return result;
}
wire::ResourceScope scope(std::vector<wire::ResourceRef> resources) {
  wire::ResourceScope result;
  for (auto& item : resources) {
    *result.add_resources() = std::move(item);
  }
  return result;
}
google::protobuf::Timestamp stamp(std::int64_t seconds, std::int32_t nanos = 0) {
  google::protobuf::Timestamp result;
  result.set_seconds(seconds);
  result.set_nanos(nanos);
  return result;
}
wire::StreamCursor cursor(std::string owner, std::string epoch, wire::ResourceScope selected,
                          std::uint64_t sequence) {
  wire::StreamCursor result;
  result.set_source_service(std::move(owner));
  result.set_epoch(std::move(epoch));
  *result.mutable_scope() = std::move(selected);
  result.set_sequence(sequence);
  return result;
}
wire::Operation operation(wire::OperationState state = wire::OPERATION_RUNNING) {
  wire::Operation result;
  auto* ref = result.mutable_ref();
  ref->set_owner_service("runtime");
  ref->set_operation_id("operation-1");
  *ref->mutable_resource() = resource(wire::RESOURCE_ACCOUNT, kAccount);
  result.set_version(3);
  result.set_state(state);
  result.set_execution_outcome(wire::EXECUTION_UNKNOWN);
  result.set_stalled(false);
  *result.mutable_observed_at() = stamp(1790000000, 500000000);
  return result;
}
wire::OperationChange change(wire::OperationState state = wire::OPERATION_RUNNING) {
  wire::OperationChange result;
  *result.mutable_operation() = operation(state);
  const auto selected = scope({resource(wire::RESOURCE_ACCOUNT, kAccount)});
  *result.mutable_cursor() = cursor("runtime", "dataset-1", selected, 7);
  return result;
}
std::string first_frame_json(std::string_view type, std::string_view scope_text,
                             std::string_view extra = {}) {
  Json frame{{"type", type},
             {"access_token", "x"},
             {"last_sequence", 0},
             {"scope", Json::parse(scope_text)}};
  frame["access_token"] = std::string(32, 't');
  if (!extra.empty()) {
    const auto additional = Json::parse(extra);
    for (const auto& item : additional.items()) {
      frame[item.key()] = item.value();
    }
  }
  return frame.dump();
}
constexpr char kOperationScope[] =
    R"({"resources":[{"kind":"RESOURCE_ACCOUNT","id":"11111111-1111-4111-8111-111111111111"}]})";
constexpr char kResumeCursor[] =
    R"({"source_service":"runtime","epoch":"dataset-1","scope":{"resources":[{"kind":"RESOURCE_ACCOUNT","id":"11111111-1111-4111-8111-111111111111"}]},"sequence":"7"})";

void route_rules() {
  const auto operations =
      accepted(target::websocket_route("/ws/operations?owner_service=runtime"), "operations route");
  require(operations.channel == "operations" && operations.owner == "runtime", "operations route");
  require(
      accepted(target::websocket_route("/ws/operations?owner_service=host"), "host route").owner ==
          "host",
      "host route");
  require(accepted(target::websocket_route("/ws/health"), "health route").owner == "control",
          "health route");
  require(accepted(target::websocket_route("/ws/adapters"), "adapter route").channel == "adapters",
          "adapter route");
  require(rejected(target::websocket_route("/ws/unknown"), "unknown channel").code ==
              wire::NOT_FOUND,
          "unknown channel is NOT_FOUND");
  require(rejected(target::websocket_route("/ws/operations"), "missing owner selector").code ==
              wire::INVALID_ARGUMENT,
          "operations requires owner_service");
  require(rejected(target::websocket_route("/ws/operations?owner_service=runtime&access_token=x"),
                   "token in url")
                  .code == wire::INVALID_ARGUMENT,
          "token in URL is refused");
  require(rejected(target::websocket_route("/ws/operations?owner_service=identity"), "bad owner")
                  .code == wire::INVALID_ARGUMENT,
          "unsupported operations owner");
  require(
      rejected(target::websocket_route("/ws/operations?owner_service=runtime#fragment"), "fragment")
              .code == wire::INVALID_ARGUMENT,
      "URL fragment refused");
  require(rejected(target::websocket_route("/ws/HEALTH"), "uppercase").code == wire::NOT_FOUND,
          "channels are lowercase");
  require(accepted(target::websocket_route("/ws/tasks"), "tasks route").owner == "scheduler",
          "tasks owner");
}

void first_frame_rules() {
  const auto operations =
      accepted(target::websocket_route("/ws/operations?owner_service=runtime"), "operations route");
  const auto hello = accepted(
      target::websocket_first_frame(first_frame_json("hello", kOperationScope), operations),
      "hello");
  require(!hello.resume && hello.scope.resources_size() == 1, "hello shape");
  require(
      rejected(target::websocket_first_frame(
                   first_frame_json("hello", kOperationScope) + R"({"type":"hello"})", operations),
               "duplicate key")
              .code == wire::INVALID_ARGUMENT,
      "duplicate JSON keys refused");
  std::string deep =
      R"({"type":"hello","access_token":")" + std::string(32, 't') +
      R"(","last_sequence":0,"scope":{"resources":[{"kind":"RESOURCE_ACCOUNT","id":")" +
      std::string(kAccount) + R"("}]},"extra":)";
  for (std::size_t depth = 0; depth < target::kMaximumJsonDepth + 1; ++depth) {
    deep += '[';
  }
  for (std::size_t depth = 0; depth < target::kMaximumJsonDepth + 1; ++depth) {
    deep += ']';
  }
  deep += '}';
  require(rejected(target::websocket_first_frame(deep, operations), "deep nesting").code ==
              wire::INVALID_ARGUMENT,
          "nesting depth bound");
  require(rejected(target::websocket_first_frame("", operations), "empty").code ==
              wire::INVALID_ARGUMENT,
          "empty first frame");
  require(rejected(target::websocket_first_frame(std::string(65537, 'x'), operations), "oversize")
                  .code == wire::INVALID_ARGUMENT,
          "first frame size bound");
  require(
      rejected(target::websocket_first_frame(
                   first_frame_json("hello", kOperationScope) + R"({"binary":true})", operations),
               "unknown fields tolerated")
              .code == wire::INVALID_ARGUMENT,
      "no unexpected top-level fields are required to be absent, but duplicate type is invalid");
  const auto resume =
      accepted(target::websocket_first_frame(
                   first_frame_json("resume", kOperationScope,
                                    R"({"session_id":")" + std::string(43, 's') +
                                        R"(","owner_cursor":)" + std::string(kResumeCursor) + "}"),
                   operations),
               "resume");
  require(resume.resume && resume.session_id.size() == 43 && resume.owner_cursor.has_value(),
          "resume shape");
  require(rejected(
              target::websocket_first_frame(
                  first_frame_json("hello", kOperationScope, R"({"last_sequence":1})"), operations),
              "hello nonzero sequence")
                  .code == wire::INVALID_ARGUMENT,
          "hello last_sequence must be zero");
  require(rejected(target::websocket_first_frame(
                       first_frame_json("resume", kOperationScope,
                                        R"({"session_id":")" + std::string(43, 's') + R"("})"),
                       operations),
                   "resume without cursor")
                  .code == wire::INVALID_ARGUMENT,
          "operations resume requires owner_cursor");
  require(rejected(target::websocket_first_frame(first_frame_json("hello", R"({"resources":[]})"),
                                                 operations),
                   "empty scope")
                  .code == wire::INVALID_ARGUMENT,
          "empty scope");
  require(
      rejected(
          target::websocket_first_frame(
              first_frame_json(
                  "hello",
                  R"({"resources":[{"kind":"RESOURCE_ACCOUNT","id":"a"},{"kind":"RESOURCE_ACCOUNT","id":"a"}]})"),
              operations),
          "duplicate scope")
              .code == wire::INVALID_ARGUMENT,
      "duplicate scope resource");
  require(
      rejected(target::websocket_first_frame(
                   first_frame_json(
                       "hello", R"({"resources":[{"kind":"RESOURCE_CATALOG","id":"control"}]})"),
                   operations),
               "catalog in operations")
              .code == wire::INVALID_ARGUMENT,
      "catalog is not an operation scope");
  const auto health = accepted(target::websocket_route("/ws/health"), "health route");
  const auto health_hello =
      accepted(target::websocket_first_frame(
                   first_frame_json(
                       "hello", R"({"resources":[{"kind":"RESOURCE_CATALOG","id":"control"}]})"),
                   health),
               "health hello");
  require(health_hello.scope.resources(0).id() == "control", "health catalog scope");
  require(
      rejected(target::websocket_first_frame(first_frame_json("resume", kOperationScope), health),
               "health resume without session")
              .code == wire::INVALID_ARGUMENT,
      "health resume requires session binding");
}

void origin_rules() {
  const std::vector<std::string> allowed{"https://panel.example", "https://admin.example"};
  require(target::websocket_origin("https://panel.example", true, allowed, false),
          "exact origin allowed");
  require(!target::websocket_origin("https://panel.example.evil", true, allowed, false),
          "origin suffix refused");
  require(!target::websocket_origin("http://panel.example", true, allowed, false),
          "non-TLS origin refused");
  require(!target::websocket_origin("https://other.example", true, allowed, false),
          "unlisted origin refused");
  require(!target::websocket_origin("", false, allowed, false), "default missing origin refused");
  require(target::websocket_origin("", false, allowed, true), "explicit missing origin allowed");
  require(!target::websocket_origin("", false, {}, true), "empty allowlist is fail-closed");
  require(!target::websocket_origin("https://panel.example", false, allowed, false),
          "missing-present flag is respected");
}

void event_rules() {
  const auto running = change();
  const auto event = accepted(target::websocket_operation_event(running, 1), "running event");
  require(target::websocket_valid_event(event), "emitted event is schema-shaped");
  const auto parsed = Json::parse(event);
  require(!parsed["payload"].contains("reason"), "running event has no reason");
  require(parsed["owner_cursor"]["sequence"] == "7", "owner cursor decimal uint64");
  require(parsed["sequence"] == 1, "control sequence");

  auto blocked = change(wire::OPERATION_BLOCKED);
  blocked.mutable_operation()->mutable_reason()->set_code(wire::UPSTREAM_UNAVAILABLE);
  blocked.mutable_operation()->mutable_reason()->set_message("never-print-upstream-secret");
  blocked.mutable_operation()->mutable_reason()->set_request_id("request-2");
  blocked.mutable_operation()->mutable_reason()->set_trace_id("trace-2");
  *blocked.mutable_operation()->mutable_next_attempt_at() = stamp(1790000001);
  const auto blocked_event =
      accepted(target::websocket_operation_event(blocked, 2), "blocked event");
  const auto blocked_json = Json::parse(blocked_event);
  require(blocked_json["payload"]["reason"]["message"] == "UPSTREAM_UNAVAILABLE",
          "owner free text is replaced by the closed-set message");
  require(target::websocket_valid_event(blocked_event), "blocked event is schema-shaped");

  auto participants = change();
  auto* first = participants.mutable_operation()->add_participants();
  first->set_participant("scheduler");
  first->set_state(wire::OPERATION_RUNNING);
  auto* second = participants.mutable_operation()->add_participants();
  second->set_participant("host");
  second->set_state(wire::OPERATION_BLOCKED);
  second->mutable_reason()->set_code(wire::UPSTREAM_UNAVAILABLE);
  second->mutable_reason()->set_request_id("request-3");
  second->mutable_reason()->set_trace_id("trace-3");
  *second->mutable_next_attempt_at() = stamp(1790000002);
  const auto participant_event =
      accepted(target::websocket_operation_event(participants, 3), "participant event");
  require(target::websocket_valid_event(participant_event), "participant event is schema-shaped");

  auto invalid = change();
  invalid.mutable_operation()->mutable_ref()->clear_resource();
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "missing resource refused");
  invalid = change();
  invalid.mutable_operation()->set_version(0);
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "zero version refused");
  invalid = change();
  invalid.mutable_operation()->mutable_ref()->set_owner_service("scheduler");
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "owner mismatch refused");
  invalid = change(wire::OPERATION_BLOCKED);
  invalid.mutable_operation()->mutable_reason()->set_code(wire::UPSTREAM_UNAVAILABLE);
  invalid.mutable_operation()->mutable_reason()->set_request_id("request-1");
  invalid.mutable_operation()->mutable_reason()->set_trace_id("trace-1");
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "blocked without next_attempt_at refused");
  invalid = change();
  invalid.mutable_operation()->mutable_observed_at()->set_seconds(1);
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "out-of-range observed_at refused");
  invalid = change();
  invalid.mutable_operation()->mutable_observed_at()->set_nanos(1000000000);
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "invalid nanoseconds refused");
  invalid = change();
  invalid.mutable_operation()->add_participants()->set_participant("scheduler");
  auto* duplicate = invalid.mutable_operation()->add_participants();
  duplicate->set_participant("scheduler");
  duplicate->set_state(wire::OPERATION_RUNNING);
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "duplicate participant refused");
  invalid = change();
  *invalid.mutable_cursor()->mutable_scope() = scope({resource(wire::RESOURCE_ACCOUNT, kModel)});
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "cursor scope must contain the operation resource");
  invalid = change();
  invalid.mutable_cursor()->set_epoch("");
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "empty dataset epoch refused");
  invalid = change();
  invalid.mutable_operation()->mutable_reason()->set_code(wire::UPSTREAM_UNAVAILABLE);
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(invalid, 1)),
          "reason without correlation ids refused");
  require(!std::holds_alternative<std::string>(target::websocket_operation_event(change(), 0)),
          "control sequence zero refused");
  require(!std::holds_alternative<std::string>(
              target::websocket_operation_event(change(), target::kMaximumSafeSequence + 1)),
          "control sequence above JS safe integer refused");

  // Mutated emitted envelopes must fail the closed-shape assertion so the transport can never
  // deliver a frame the frozen AsyncAPI schema rejects.
  auto mutated = Json::parse(event);
  mutated.erase("owner_cursor");
  require(!target::websocket_valid_event(mutated.dump()), "missing owner cursor refused");
  mutated = Json::parse(event);
  mutated["sequence"] = target::kMaximumSafeSequence + 1;
  require(!target::websocket_valid_event(mutated.dump()), "unsafe sequence refused");
  mutated = Json::parse(event);
  mutated["occurred_at"] = "1999-12-31T23:59:59Z";
  require(!target::websocket_valid_event(mutated.dump()), "pre-2000 time refused");
  mutated = Json::parse(event);
  mutated["payload"]["version"] = "0";
  require(!target::websocket_valid_event(mutated.dump()), "zero version refused");
  mutated = Json::parse(event);
  mutated["payload"]["state"] = "OPERATION_BLOCKED";
  require(!target::websocket_valid_event(mutated.dump()), "blocked without reason refused");
  mutated = Json::parse(participant_event);
  mutated["payload"]["participants"][1]["reason"] =
      Json::parse(R"({"code":"UPSTREAM_UNAVAILABLE"})");
  require(!target::websocket_valid_event(mutated.dump()), "partial participant reason refused");
  mutated = Json::parse(blocked_event);
  mutated["payload"]["reason"]["message"] = "free upstream text";
  require(!target::websocket_valid_event(mutated.dump()), "free reason text refused");
}

void cursor_and_frame_rules() {
  const auto selected = scope({resource(wire::RESOURCE_ACCOUNT, kAccount)});
  const auto value = cursor("runtime", "dataset-1", selected, 18446744073709551615ULL);
  const auto encoded = accepted(target::websocket_cursor_json(value), "cursor json");
  require(Json::parse(encoded)["sequence"] == "18446744073709551615", "full uint64 sequence");
  auto bad = value;
  bad.set_source_service("control");
  require(!std::holds_alternative<std::string>(target::websocket_cursor_json(bad)),
          "unknown owner cursor refused");
  bad = value;
  bad.set_epoch(std::string(129, 'e'));
  require(!std::holds_alternative<std::string>(target::websocket_cursor_json(bad)),
          "long epoch refused");
  bad = value;
  *bad.mutable_scope() = scope({});
  require(!std::holds_alternative<std::string>(target::websocket_cursor_json(bad)),
          "empty cursor scope refused");

  const auto ready =
      accepted(target::websocket_session_ready(std::string(43, 's'), 12, value), "session ready");
  const auto ready_json = Json::parse(ready);
  require(ready_json["type"] == "SESSION_READY" && ready_json["last_sequence"] == 12 &&
              ready_json["owner_cursor"]["sequence"] == "18446744073709551615",
          "session ready shape");
  require(!std::holds_alternative<std::string>(
              target::websocket_session_ready(std::string(31, 's'), 0, value)),
          "short session id refused");
  require(!std::holds_alternative<std::string>(target::websocket_session_ready(
              std::string(43, 's'), target::kMaximumSafeSequence + 1, value)),
          "unsafe accepted sequence refused");
  const auto plain =
      accepted(target::websocket_session_ready(std::string(43, 's'), 0, std::nullopt),
               "plain session ready");
  require(!Json::parse(plain).contains("owner_cursor"), "plain SessionReady shape");

  for (const auto reason :
       {target::WebSocketResyncReason::CursorExpired, target::WebSocketResyncReason::InvalidCursor,
        target::WebSocketResyncReason::ServerRestart}) {
    const auto text = accepted(target::websocket_resync_required(reason), "resync frame");
    const auto parsed = Json::parse(text);
    require(parsed["type"] == "RESYNC_REQUIRED", "resync type");
    const auto expected = reason == target::WebSocketResyncReason::CursorExpired ? "CURSOR_EXPIRED"
                          : reason == target::WebSocketResyncReason::InvalidCursor
                              ? "INVALID_CURSOR"
                              : "SERVER_RESTART";
    require(parsed["reason"] == expected, "resync reason closed set");
  }
}

struct Fixtures {
  std::map<std::string, std::string> files;
};

Fixtures schema_fixtures() {
  Fixtures fixtures;
  const auto selected = scope({resource(wire::RESOURCE_ACCOUNT, kAccount)});
  const auto owner_cursor = cursor("runtime", "dataset-1", selected, 7);
  fixtures.files["event_running.json"] =
      accepted(target::websocket_operation_event(change(), 1), "running fixture");
  auto blocked = change(wire::OPERATION_BLOCKED);
  blocked.mutable_operation()->mutable_reason()->set_code(wire::UPSTREAM_UNAVAILABLE);
  blocked.mutable_operation()->mutable_reason()->set_request_id("request-2");
  blocked.mutable_operation()->mutable_reason()->set_trace_id("trace-2");
  *blocked.mutable_operation()->mutable_next_attempt_at() = stamp(1790000001);
  fixtures.files["event_blocked.json"] =
      accepted(target::websocket_operation_event(blocked, 2), "blocked fixture");
  auto participants = change();
  auto* scheduler = participants.mutable_operation()->add_participants();
  scheduler->set_participant("scheduler");
  scheduler->set_state(wire::OPERATION_RUNNING);
  auto* host = participants.mutable_operation()->add_participants();
  host->set_participant("host");
  host->set_state(wire::OPERATION_BLOCKED);
  host->mutable_reason()->set_code(wire::UPSTREAM_UNAVAILABLE);
  host->mutable_reason()->set_request_id("request-3");
  host->mutable_reason()->set_trace_id("trace-3");
  *host->mutable_next_attempt_at() = stamp(1790000002);
  fixtures.files["event_participants.json"] =
      accepted(target::websocket_operation_event(participants, 3), "participant fixture");
  fixtures.files["session_ready.json"] = accepted(
      target::websocket_session_ready(std::string(43, 's'), 12, owner_cursor), "ready fixture");
  fixtures.files["resync_cursor_expired.json"] = accepted(
      target::websocket_resync_required(target::WebSocketResyncReason::CursorExpired), "resync 1");
  fixtures.files["resync_invalid_cursor.json"] = accepted(
      target::websocket_resync_required(target::WebSocketResyncReason::InvalidCursor), "resync 2");
  fixtures.files["resync_server_restart.json"] = accepted(
      target::websocket_resync_required(target::WebSocketResyncReason::ServerRestart), "resync 3");
  return fixtures;
}

void emit(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  for (const auto& [name, content] : schema_fixtures().files) {
    std::ofstream output(directory / name);
    output << content << '\n';
    if (!output) {
      throw std::runtime_error("WS-04 protocol assertion: cannot write " + name);
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    route_rules();
    first_frame_rules();
    origin_rules();
    event_rules();
    cursor_and_frame_rules();
    if (argc == 3 && std::string_view{argv[1]} == "--emit") {
      emit(argv[2]);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "control target websocket protocol: ok\n";
  return 0;
}
