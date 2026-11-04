// WS-04 retained store unit evidence: replay boundary, resume binding, cursor checks, bounded
// capacity/retention eviction and single-active-socket takeover. Uses an injected clock; no
// network, TLS, PG or owner process is involved.
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "kiarabot/control/target/websocket_protocol.hpp"
#include "kiarabot/control/target/websocket_stream_store.hpp"

namespace {
namespace target = kiarabot::control::target;
namespace wire = kiarabot::v1;
namespace core = kiarabot::core;

constexpr char kAccount[] = "11111111-1111-4111-8111-111111111111";

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error("WS-04 store assertion: " + std::string{message});
  }
}
wire::ResourceScope scope() {
  wire::ResourceScope result;
  auto* resource = result.add_resources();
  resource->set_kind(wire::RESOURCE_ACCOUNT);
  resource->set_id(kAccount);
  return result;
}
wire::StreamCursor cursor(std::uint64_t sequence) {
  wire::StreamCursor result;
  result.set_source_service("runtime");
  result.set_epoch("dataset-1");
  *result.mutable_scope() = scope();
  result.set_sequence(sequence);
  return result;
}
target::WebSocketIdentity identity(std::uint64_t version = 1) {
  target::WebSocketIdentity result;
  result.principal = "33333333-3333-4333-8333-333333333333";
  result.identity_session = "11111111-1111-4111-8111-111111111111";
  result.authorization_version = version;
  result.checked_at =
      core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
          std::chrono::seconds{1790000000})};
  result.expires_at =
      core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
          std::chrono::seconds{1790000900})};
  return result;
}
target::WebSocketSelection selection() {
  return {"operations", "runtime", scope()};
}
std::string event_json(std::uint64_t sequence) {
  return R"({"schema_version":"kiarabot.stream.v1","event_type":"operation.updated","sequence":)" +
         std::to_string(sequence) +
         R"(,"occurred_at":"2026-09-21T15:33:20Z","payload":{},"owner_cursor":{"source_service":"runtime","epoch":"dataset-1","scope":{"resources":[{"kind":"RESOURCE_ACCOUNT","id":"11111111-1111-4111-8111-111111111111"}]},"sequence":")" +
         std::to_string(sequence) + R"("}})";
}
class Handle final : public target::WebSocketSocketHandle {
public:
  void terminate(std::uint16_t close_code) override {
    terminated.push_back(close_code);
  }
  std::vector<std::uint16_t> terminated;
};
struct Clock {
  std::chrono::steady_clock::time_point value{};
  void advance(std::chrono::seconds seconds) {
    value += seconds;
  }
};
std::shared_ptr<target::WebSocketStreamStore> store(std::shared_ptr<Clock> clock,
                                                    target::WebSocketRetainedLimits limits = {}) {
  return std::make_shared<target::WebSocketStreamStore>(limits, [clock] { return clock->value; });
}
struct Stream {
  std::shared_ptr<target::WebSocketStreamStore> store;
  std::shared_ptr<Handle> socket = std::make_shared<Handle>();
  std::string id;
};
Stream create_stream(const std::shared_ptr<target::WebSocketStreamStore>& stream_store,
                     std::uint64_t head = 0) {
  Stream stream;
  stream.store = stream_store;
  const auto created = stream_store->create(identity(), selection(), cursor(head), stream.socket);
  require(std::holds_alternative<std::string>(created), "sessions are created");
  stream.id = std::get<std::string>(created);
  return stream;
}
void append(Stream& stream, std::uint64_t sequence) {
  const auto head = stream.store->head(stream.id, stream.socket);
  require(head.has_value() && *head + 1 == sequence, "expected append sequence");
  require(stream.store
              ->append(stream.id, stream.socket, sequence, event_json(sequence), cursor(sequence))
              .has_value(),
          "events are appended");
}

void create_and_resume() {
  const auto clock = std::make_shared<Clock>();
  const auto stream_store = store(clock);
  auto stream = create_stream(stream_store);
  require(stream_store->retained_sessions() == 1, "one retained session");
  require(stream_store->head(stream.id, stream.socket).value() == 0, "initial head");
  require(stream_store->accepted(stream.id, stream.socket).value() == 0, "initial accepted");
  require(stream_store->owner_cursor(stream.id, stream.socket)->sequence() == 0,
          "initial owner cursor");

  append(stream, 1);
  append(stream, 2);
  append(stream, 3);
  require(stream_store->head(stream.id, stream.socket).value() == 3, "head advances");
  require(stream_store->retained_bytes() == event_json(1).size() * 3, "retained bytes accounted");

  require(stream_store->acknowledge(stream.id, stream.socket, 1), "ack advances once");
  require(!stream_store->acknowledge(stream.id, stream.socket, 1), "ack cannot regress");
  require(!stream_store->acknowledge(stream.id, stream.socket, 4), "ack beyond head refused");
  const auto replay =
      stream_store->resume(stream.id, identity(), selection(), 1, cursor(1), stream.socket);
  require(replay.status == target::WebSocketResumeResult::Status::Accepted,
          "resume below head replays");
  require(replay.accepted_sequence == 1, "resume echoes the accepted boundary");
  const auto next = stream_store->next_event(stream.id, stream.socket, 1);
  require(next.has_value() && next->sequence == 2, "replay starts after the accepted sequence");
  const auto pending = stream_store->pending(stream.id, stream.socket);
  require(pending.frames == 2 && pending.bytes == event_json(2).size() + event_json(3).size(),
          "pending accounting");
  const auto accepted =
      stream_store->resume(stream.id, identity(), selection(), 3, cursor(3), stream.socket);
  require(accepted.status == target::WebSocketResumeResult::Status::Accepted,
          "resume at head is accepted");
  require(!stream_store->next_event(stream.id, stream.socket, 3).has_value(),
          "no event beyond head");
}

void resume_failure_paths() {
  const auto clock = std::make_shared<Clock>();
  const auto stream_store = store(clock);
  auto stream = create_stream(stream_store, 5);
  append(stream, 1);
  const auto unknown =
      stream_store->resume("unknown-session", identity(), selection(), 0, cursor(5), stream.socket);
  require(unknown.status == target::WebSocketResumeResult::Status::Resync &&
              unknown.resync == target::WebSocketResyncReason::ServerRestart,
          "unknown session is SERVER_RESTART");
  const auto future =
      stream_store->resume(stream.id, identity(), selection(), 9, cursor(5), stream.socket);
  require(future.status == target::WebSocketResumeResult::Status::Resync &&
              future.resync == target::WebSocketResyncReason::InvalidCursor,
          "future sequence is INVALID_CURSOR");
  const auto wrong_cursor =
      stream_store->resume(stream.id, identity(), selection(), 0, cursor(6), stream.socket);
  require(wrong_cursor.status == target::WebSocketResumeResult::Status::Resync &&
              wrong_cursor.resync == target::WebSocketResyncReason::InvalidCursor,
          "mismatched owner cursor is INVALID_CURSOR");
  target::WebSocketSelection other = selection();
  *other.scope.mutable_resources(0) = [] {
    wire::ResourceRef resource;
    resource.set_kind(wire::RESOURCE_ADAPTER);
    resource.set_id("22222222-2222-4222-8222-222222222222");
    return resource;
  }();
  const auto cross_scope =
      stream_store->resume(stream.id, identity(), other, 1, cursor(1), stream.socket);
  require(cross_scope.status == target::WebSocketResumeResult::Status::Rejected &&
              cross_scope.failure.code == wire::PERMISSION_DENIED,
          "binding mismatch is PERMISSION_DENIED");
  const auto other_version =
      stream_store->resume(stream.id, identity(2), selection(), 1, cursor(1), stream.socket);
  require(other_version.status == target::WebSocketResumeResult::Status::Rejected,
          "authorization version change rejects resume");
  require(stream_store->resume(stream.id, identity(), selection(), 1, cursor(1), stream.socket)
                  .status == target::WebSocketResumeResult::Status::Accepted,
          "matching binding resumes");
}

void single_socket_takeover() {
  const auto clock = std::make_shared<Clock>();
  const auto stream_store = store(clock);
  auto stream = create_stream(stream_store);
  append(stream, 1);
  auto replacement = std::make_shared<Handle>();
  const auto resumed =
      stream_store->resume(stream.id, identity(), selection(), 1, cursor(1), replacement);
  require(resumed.status == target::WebSocketResumeResult::Status::Accepted, "takeover accepted");
  require(stream.socket->terminated.size() == 1 && stream.socket->terminated.front() == 1001,
          "previous socket is terminated with 1001");
  require(!stream_store->append(stream.id, stream.socket, 2, event_json(2), cursor(2)).has_value(),
          "stale socket cannot append");
  require(stream_store->append(stream.id, replacement, 2, event_json(2), cursor(2)).has_value(),
          "current socket appends");
  require(!stream_store->next_event(stream.id, stream.socket, 1).has_value(),
          "stale socket cannot read replay");
  stream_store->detach(stream.id, stream.socket);
  require(stream_store->head(stream.id, replacement).has_value(),
          "stale detach cannot clear the current socket");
}

void retention_and_capacity() {
  const auto clock = std::make_shared<Clock>();
  target::WebSocketRetainedLimits limits;
  limits.maximum_session_events = 2;
  const auto stream_store = store(clock, limits);
  auto stream = create_stream(stream_store);
  append(stream, 1);
  append(stream, 2);
  append(stream, 3);
  require(stream_store->head(stream.id, stream.socket).value() == 3, "capacity keeps the head");
  const auto expired =
      stream_store->resume(stream.id, identity(), selection(), 0, cursor(0), stream.socket);
  require(expired.status == target::WebSocketResumeResult::Status::Resync &&
              expired.resync == target::WebSocketResyncReason::CursorExpired,
          "evicted replay boundary is CURSOR_EXPIRED");
  const auto boundary =
      stream_store->resume(stream.id, identity(), selection(), 1, cursor(1), stream.socket);
  require(boundary.status == target::WebSocketResumeResult::Status::Accepted,
          "boundary-1 stays resumable");
  clock->advance(std::chrono::seconds{600});
  const auto retained =
      stream_store->resume(stream.id, identity(), selection(), 3, cursor(3), stream.socket);
  require(retained.status == target::WebSocketResumeResult::Status::Accepted,
          "client processed at head needs no replay");
  clock->advance(std::chrono::seconds{1});
  const auto pruned =
      stream_store->resume(stream.id, identity(), selection(), 2, cursor(2), stream.socket);
  require(pruned.status == target::WebSocketResumeResult::Status::Rejected ||
              pruned.status == target::WebSocketResumeResult::Status::Resync,
          "inactive session expires after retention");
}

void session_limit_and_tombstones() {
  const auto clock = std::make_shared<Clock>();
  target::WebSocketRetainedLimits limits;
  limits.maximum_sessions = 1;
  const auto stream_store = store(clock, limits);
  auto first = create_stream(stream_store);
  require(stream_store->retained_sessions() == 1, "first session retained");
  auto second = create_stream(stream_store);
  require(stream_store->retained_sessions() == 1, "session count is bounded");
  const auto evicted =
      stream_store->resume(first.id, identity(), selection(), 0, cursor(0), first.socket);
  if (!(evicted.status == target::WebSocketResumeResult::Status::Resync &&
        evicted.resync == target::WebSocketResyncReason::CursorExpired)) {
    throw std::runtime_error(
        "count eviction: status=" + std::to_string(static_cast<int>(evicted.status)) +
        " resync=" + std::to_string(static_cast<int>(evicted.resync)) +
        " failure=" + wire::ErrorCode_Name(evicted.failure.code));
  }
  require(stream_store->append(second.id, second.socket, 1, event_json(1), cursor(1)).has_value(),
          "new session still appends");
  stream_store->clear();
  require(stream_store->retained_sessions() == 0 && stream_store->retained_bytes() == 0,
          "clear releases retained memory");
}

void total_byte_eviction() {
  const auto clock = std::make_shared<Clock>();
  target::WebSocketRetainedLimits limits;
  limits.maximum_total_bytes = event_json(1).size() * 2;
  const auto stream_store = store(clock, limits);
  auto first = create_stream(stream_store);
  auto second = create_stream(stream_store);
  append(first, 1);
  append(second, 1);
  append(second, 2);
  require(stream_store->retained_bytes() <= limits.maximum_total_bytes, "total bytes bounded");
  const auto evicted =
      stream_store->resume(first.id, identity(), selection(), 0, cursor(0), first.socket);
  require(evicted.status == target::WebSocketResumeResult::Status::Resync &&
              evicted.resync == target::WebSocketResyncReason::CursorExpired,
          "global budget eviction is explicit");
}

void owner_cursor_progress() {
  const auto clock = std::make_shared<Clock>();
  const auto stream_store = store(clock);
  auto stream = create_stream(stream_store);
  require(stream_store->set_owner_cursor(stream.id, stream.socket, cursor(7)),
          "owner cursor advances after a full page");
  require(stream_store->owner_cursor(stream.id, stream.socket)->sequence() == 7,
          "owner cursor read back");
  require(!stream_store->set_owner_cursor(stream.id, stream.socket, cursor(6)),
          "owner cursor cannot regress");
  require(stream_store->set_owner_cursor(stream.id, stream.socket, cursor(8)),
          "owner cursor advances");
  require(stream_store->resume(stream.id, identity(), selection(), 0, cursor(0), stream.socket)
                  .status == target::WebSocketResumeResult::Status::Accepted,
          "resume boundary still compared with the initial cursor");
}

} // namespace

int main() {
  try {
    create_and_resume();
    resume_failure_paths();
    single_socket_takeover();
    retention_and_capacity();
    session_limit_and_tombstones();
    total_byte_eviction();
    owner_cursor_progress();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "control target websocket stream store: ok\n";
  return 0;
}
