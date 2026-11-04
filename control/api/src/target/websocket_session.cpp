#include "kiarabot/control/target/websocket_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include "kiarabot/control/target/websocket_protocol.hpp"

namespace kiarabot::control::target {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using Error = boost::system::error_code;
using Strand = asio::strand<asio::io_context::executor_type>;

std::uint16_t close_code(const WebSocketFailure& failure) {
  return websocket_close_code(failure);
}

// One target WSS connection. Every asynchronous completion is dispatched on a private strand and
// the lifecycle is a single explicit state machine: a connection is only destroyed after all
// accept/read/write/close operations have completed and re-entered the strand. Beast's tcp_stream
// aborts the process when destroyed with a read and a write both pending, so teardown cancels the
// socket and waits for the aborted handlers instead of dropping the object under live operations.
class Connection final : public WebSocketConnection,
                         public std::enable_shared_from_this<Connection> {
public:
  Connection(WebSocketStream stream, http::request<http::string_body> request,
             WebSocketSelection route, std::shared_ptr<WebSocketStreamStore> store,
             std::shared_ptr<WebSocketSourcePort> source, asio::thread_pool& workers,
             asio::io_context& io, WebSocketSessionConfig config,
             std::function<void(WebSocketConnection*)> completed)
      : ws_(std::move(stream)), request_(std::move(request)), route_(std::move(route)),
        store_(std::move(store)), source_(std::move(source)), workers_(workers), config_(config),
        completed_(std::move(completed)), strand_(asio::make_strand(io)),
        first_frame_timer_(strand_), poll_timer_(strand_), force_timer_(strand_) {
    ws_.read_message_max(kMaximumFirstFrameBytes);
    ws_.text(true);
    ws_.auto_fragment(true);
  }
  ~Connection() override {
    // Reaching the destructor with a live Beast operation would abort in basic_stream::reset;
    // cancel the transport synchronously as a last-resort guard only (the state machine never
    // destroys the object while its own operations are pending).
    if (state_ != State::Finished) {
      Error ignored;
      auto& socket = beast::get_lowest_layer(ws_).socket();
      socket.cancel(ignored);
      socket.close(ignored);
    }
  }

  void start() override {
    asio::post(strand_, [self = shared_from_this()] { self->begin_accept(); });
  }
  void terminate(std::uint16_t code) override {
    asio::post(strand_, [self = shared_from_this(), code] { self->begin_close(code); });
  }
  void terminate_now(std::uint16_t code) override {
    asio::post(strand_, [self = shared_from_this(), code] { self->begin_close(code); });
  }

private:
  enum class State {
    Idle,
    Handshaking,
    ReadingFirstFrame,
    Authenticating,
    Established,
    Closing,
    Finished,
  };
  struct WriteFrame {
    std::string json;
    std::uint64_t sequence = 0;
    std::function<void()> after;
  };

  std::shared_ptr<WebSocketSocketHandle> handle() {
    if (!handle_) {
      handle_ = shared_from_this();
    }
    return handle_;
  }
  bool active() const {
    return state_ != State::Closing && state_ != State::Finished;
  }
  void cancel_timers() {
    first_frame_timer_.cancel();
    poll_timer_.cancel();
    force_timer_.cancel();
  }

  // ---- Upgrade handshake -------------------------------------------------
  void begin_accept() {
    if (state_ != State::Idle) {
      return;
    }
    state_ = State::Handshaking;
    accept_pending_ = true;
    beast::get_lowest_layer(ws_).expires_never();
    first_frame_timer_.expires_after(config_.first_frame_timeout);
    first_frame_timer_.async_wait(
        asio::bind_executor(strand_, [self = shared_from_this()](const Error& error) {
          if (!error && self->active()) {
            self->begin_close(4400);
          }
        }));
    ws_.async_accept(request_,
                     asio::bind_executor(strand_, [self = shared_from_this()](const Error& error) {
                       self->accept_pending_ = false;
                       self->on_accept(error);
                     }));
  }
  void on_accept(const Error& error) {
    if (state_ == State::Closing) {
      if (!close_sent_ && !force_started_) {
        start_close_write();
      } else {
        maybe_finalize();
      }
      return;
    }
    if (state_ != State::Handshaking || error) {
      abort_now();
      return;
    }
    read_first_frame();
  }
  void read_first_frame() {
    if (state_ != State::Handshaking) {
      return;
    }
    state_ = State::ReadingFirstFrame;
    beast::get_lowest_layer(ws_).expires_never();
    if (!read_start()) {
      abort_now();
    }
  }
  // ---- frame reading -----------------------------------------------------
  bool read_start() {
    if (read_pending_ || state_ == State::Closing || state_ == State::Finished) {
      return false;
    }
    read_pending_ = true;
    beast::get_lowest_layer(ws_).expires_never();
    ws_.async_read(buffer_, asio::bind_executor(strand_, [self = shared_from_this()](
                                                             const Error& error, std::size_t size) {
                     self->on_read(error, size);
                   }));
    return true;
  }
  void on_read(const Error& error, std::size_t size) {
    read_pending_ = false;
    if (state_ == State::Finished) {
      return;
    }
    if (state_ == State::Closing) {
      maybe_finalize();
      return;
    }
    if (error) {
      abort_now();
      return;
    }
    auto text = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    if (state_ == State::ReadingFirstFrame) {
      first_frame_timer_.cancel();
      const auto frame = websocket_first_frame(text, route_);
      if (const auto* failure = std::get_if<WebSocketFailure>(&frame)) {
        begin_close(close_code(*failure));
        return;
      }
      auto selected = std::get<WebSocketFirstFrame>(frame);
      // The URL route only selects channel/owner; the authenticated scope arrives in the first
      // frame and is the exact scope every owner poll re-authorizes.
      route_.scope = selected.scope;
      authenticate(std::move(selected));
      return;
    }
    static_cast<void>(size);
    if (state_ == State::Established) {
      // After authentication the client sends no application data frames; a repeated handshake
      // or a binary frame is an invalid first-frame-class error, not a new session.
      begin_close(4400);
    }
  }
  // ---- authentication ----------------------------------------------------
  void authenticate(WebSocketFirstFrame frame) {
    state_ = State::Authenticating;
    authenticating_ = true;
    access_token_ = frame.access_token;
    const auto deadline = std::chrono::steady_clock::now() + config_.authorization_budget;
    asio::post(workers_, [self = shared_from_this(), frame = std::move(frame), deadline]() mutable {
      auto result = self->source_->poll(self->route_, frame.access_token, std::nullopt, deadline);
      asio::post(self->strand_,
                 [self, frame = std::move(frame), result = std::move(result)]() mutable {
                   self->on_authenticated(std::move(frame), std::move(result));
                 });
    });
  }
  void on_authenticated(WebSocketFirstFrame frame, WebSocketResult<WebSocketPoll> result) {
    authenticating_ = false;
    if (state_ != State::Authenticating) {
      return;
    }
    if (const auto* failure = std::get_if<WebSocketFailure>(&result)) {
      begin_close(close_code(*failure));
      return;
    }
    auto poll = std::get<WebSocketPoll>(std::move(result));
    established_identity_ = poll.identity;
    last_confirmed_ = std::chrono::steady_clock::now();
    if (frame.resume) {
      establish_resume(std::move(frame), std::move(poll));
    } else {
      establish_hello(std::move(poll));
    }
  }
  void establish_hello(WebSocketPoll poll) {
    if (poll.scoped) {
      // v1.2 scoped feed channels have no client-visible owner cursor: SESSION_READY is the plain
      // shape and the owner page boundary stays in the retained server session for replay-safe
      // continuation.
      const auto created = store_->create(poll.identity, route_, std::nullopt, handle());
      if (const auto* failure = std::get_if<WebSocketFailure>(&created)) {
        begin_close(close_code(*failure));
        return;
      }
      session_id_ = std::get<std::string>(created);
      if (!store_->set_owner_cursor(session_id_, handle(), poll.scoped->next_cursor)) {
        abort_now();
        return;
      }
      const auto ready = websocket_session_ready(session_id_, 0, std::nullopt);
      if (const auto* failure = std::get_if<WebSocketFailure>(&ready)) {
        begin_close(close_code(*failure));
        return;
      }
      state_ = State::Established;
      enqueue({std::get<std::string>(ready), 0, {}});
      start_established();
      return;
    }
    if (poll.snapshot) {
      // health/adapters/plugins have no client-visible owner cursor and emit value changes only.
      const auto created = store_->create(poll.identity, route_, std::nullopt, handle());
      if (const auto* failure = std::get_if<WebSocketFailure>(&created)) {
        begin_close(close_code(*failure));
        return;
      }
      session_id_ = std::get<std::string>(created);
      const auto ready = websocket_session_ready(session_id_, 0, std::nullopt);
      if (const auto* failure = std::get_if<WebSocketFailure>(&ready)) {
        begin_close(close_code(*failure));
        return;
      }
      state_ = State::Established;
      enqueue({std::get<std::string>(ready), 0, {}});
      if (!apply_snapshot_page(*poll.snapshot)) {
        return;
      }
      start_established();
      return;
    }
    if (!poll.operations.has_next_cursor()) {
      abort_now();
      return;
    }
    const auto head = poll.operations.next_cursor();
    const auto created = store_->create(poll.identity, route_, head, handle());
    if (const auto* failure = std::get_if<WebSocketFailure>(&created)) {
      begin_close(close_code(*failure));
      return;
    }
    session_id_ = std::get<std::string>(created);
    const auto ready = websocket_session_ready(session_id_, 0, head);
    if (const auto* failure = std::get_if<WebSocketFailure>(&ready)) {
      begin_close(close_code(*failure));
      return;
    }
    state_ = State::Established;
    enqueue({std::get<std::string>(ready), 0, {}});
    start_established();
  }
  void establish_resume(WebSocketFirstFrame frame, WebSocketPoll poll) {
    const auto resumed = store_->resume(frame.session_id, poll.identity, route_,
                                        frame.last_sequence, frame.owner_cursor, handle());
    switch (resumed.status) {
    case WebSocketResumeResult::Status::Rejected:
      begin_close(close_code(resumed.failure));
      return;
    case WebSocketResumeResult::Status::Resync: {
      const auto resync = websocket_resync_required(resumed.resync);
      if (const auto* failure = std::get_if<WebSocketFailure>(&resync)) {
        begin_close(close_code(*failure));
        return;
      }
      state_ = State::Established;
      resync_then_close(std::get<std::string>(resync));
      return;
    }
    case WebSocketResumeResult::Status::Accepted:
      break;
    }
    session_id_ = frame.session_id;
    const auto ready =
        websocket_session_ready(session_id_, frame.last_sequence, frame.owner_cursor);
    if (const auto* failure = std::get_if<WebSocketFailure>(&ready)) {
      begin_close(close_code(*failure));
      return;
    }
    state_ = State::Established;
    enqueue({std::get<std::string>(ready), 0, {}});
    if (poll.snapshot) {
      seed_snapshot(*poll.snapshot);
    }
    start_established();
  }
  void resync_then_close(std::string frame) {
    accept_events_ = false;
    enqueue({std::move(frame), 0, [self = shared_from_this()] { self->begin_close(1000); }});
  }
  void start_established() {
    accept_events_ = true;
    if (!read_start()) {
      abort_now();
      return;
    }
    schedule_poll(std::chrono::milliseconds{0});
    pump_events();
  }
  // ---- owner polling -----------------------------------------------------
  void schedule_poll(std::chrono::milliseconds delay) {
    if (!active() || !accept_events_) {
      return;
    }
    poll_timer_.expires_after(delay);
    poll_timer_.async_wait(
        asio::bind_executor(strand_, [self = shared_from_this()](const Error& error) {
          if (!error && self->active() && self->accept_events_) {
            self->begin_poll();
          }
        }));
  }
  void begin_poll() {
    if (!active() || !accept_events_ || poll_in_flight_ || authenticating_ || backpressure_) {
      schedule_poll(config_.poll_interval);
      return;
    }
    poll_in_flight_ = true;
    const auto deadline = std::chrono::steady_clock::now() + config_.authorization_budget;
    asio::post(workers_, [self = shared_from_this(), deadline]() {
      std::optional<v1::StreamCursor> cursor;
      if (const auto current = self->store_->owner_cursor(self->session_id_, self->handle())) {
        cursor = *current;
      }
      auto result = self->source_->poll(self->route_, self->access_token_, cursor, deadline);
      asio::post(self->strand_, [self, result = std::move(result)]() mutable {
        self->on_poll(std::move(result));
      });
    });
  }
  void on_poll(WebSocketResult<WebSocketPoll> result) {
    poll_in_flight_ = false;
    if (!active() || !accept_events_) {
      return;
    }
    if (const auto* failure = std::get_if<WebSocketFailure>(&result)) {
      if (failure->cursor_expired) {
        const auto resync = websocket_resync_required(WebSocketResyncReason::CursorExpired);
        if (const auto* rejected = std::get_if<WebSocketFailure>(&resync)) {
          begin_close(close_code(*rejected));
        } else {
          resync_then_close(std::get<std::string>(resync));
        }
      } else {
        begin_close(close_code(*failure));
      }
      return;
    }
    auto poll = std::get<WebSocketPoll>(std::move(result));
    // The stream capability is bound to principal/Identity session/authorization version. A
    // changed authorization context invalidates the retained replay instead of extending it.
    if (poll.identity.principal != established_identity_.principal ||
        poll.identity.identity_session != established_identity_.identity_session ||
        poll.identity.authorization_version != established_identity_.authorization_version) {
      begin_close(4403);
      return;
    }
    last_confirmed_ = std::chrono::steady_clock::now();
    if (poll.snapshot) {
      if (!apply_snapshot_page(*poll.snapshot)) {
        return;
      }
      pump_events();
      schedule_poll(config_.poll_interval);
      return;
    }
    if (poll.scoped) {
      if (!apply_scoped_page(*poll.scoped)) {
        return;
      }
      pump_events();
      if (backpressure_) {
        schedule_poll(config_.poll_interval);
        return;
      }
      if (poll.scoped->has_more) {
        begin_poll();
        return;
      }
      schedule_poll(config_.poll_interval);
      return;
    }
    if (!apply_page(poll.operations)) {
      return;
    }
    pump_events();
    if (backpressure_) {
      schedule_poll(config_.poll_interval);
      return;
    }
    if (poll.operations.has_more()) {
      begin_poll();
      return;
    }
    schedule_poll(config_.poll_interval);
  }
  // Append one already-built snapshot event at the next session sequence. Snapshot channels carry
  // no owner cursor; replay is the retained Control store only.
  bool append_snapshot_event(std::string json) {
    const auto head = store_->head(session_id_, handle());
    if (!head) {
      abort_now();
      return false;
    }
    const auto sequence = *head + 1;
    if (!store_->append(session_id_, handle(), sequence, std::move(json), std::nullopt)) {
      abort_now();
      return false;
    }
    return true;
  }
  // health/adapters/plugins emit only on a value change. observed_at-only refreshes are ignored;
  // unavailable owner queries leave the last known value (unknown/stale) without an event.
  bool apply_snapshot_page(const WebSocketSnapshotBatch& batch) {
    if (batch.health) {
      const auto& health = *batch.health;
      const std::string signature =
          std::string{health.identity_confirmed ? "1" : "0"} + (health.runtime_ready ? "1" : "0") +
          (health.scheduler_ready ? "1" : "0") + (health.model_ready ? "1" : "0");
      if (signature != health_signature_) {
        const auto head = store_->head(session_id_, handle());
        if (!head) {
          abort_now();
          return false;
        }
        const auto event = websocket_health_event(health, *head + 1);
        if (const auto* failure = std::get_if<WebSocketFailure>(&event)) {
          begin_close(close_code(*failure));
          return false;
        }
        if (!append_snapshot_event(std::get<std::string>(event))) {
          return false;
        }
        health_signature_ = signature;
      }
    }
    for (const auto& observation : batch.adapters) {
      if (!observation.available) {
        continue;
      }
      const auto signature = websocket_adapter_signature(observation.record);
      if (!signature) {
        begin_close(1011);
        return false;
      }
      const auto& id = observation.record.resource().id();
      const auto found = adapter_signatures_.find(id);
      if (found != adapter_signatures_.end() && found->second == *signature) {
        continue;
      }
      const auto head = store_->head(session_id_, handle());
      if (!head) {
        abort_now();
        return false;
      }
      const auto event = websocket_adapter_event(observation.record, *head + 1);
      if (const auto* failure = std::get_if<WebSocketFailure>(&event)) {
        begin_close(close_code(*failure));
        return false;
      }
      if (!append_snapshot_event(std::get<std::string>(event))) {
        return false;
      }
      adapter_signatures_[id] = *signature;
    }
    for (const auto& observation : batch.plugins) {
      if (!observation.available) {
        continue;
      }
      const auto signature = websocket_plugin_signature(observation.record);
      if (!signature) {
        begin_close(1011);
        return false;
      }
      const auto& id = observation.record.instance_id();
      const auto found = plugin_signatures_.find(id);
      if (found != plugin_signatures_.end() && found->second == *signature) {
        continue;
      }
      const auto head = store_->head(session_id_, handle());
      if (!head) {
        abort_now();
        return false;
      }
      const auto event = websocket_plugin_event(observation.record, *head + 1);
      if (const auto* failure = std::get_if<WebSocketFailure>(&event)) {
        begin_close(close_code(*failure));
        return false;
      }
      if (!append_snapshot_event(std::get<std::string>(event))) {
        return false;
      }
      plugin_signatures_[id] = *signature;
    }
    return true;
  }
  // A resume already replayed retained events; seed the value signatures so the next poll does not
  // duplicate unchanged snapshot facts.
  void seed_snapshot(const WebSocketSnapshotBatch& batch) {
    if (batch.health) {
      const auto& health = *batch.health;
      health_signature_ = std::string{health.identity_confirmed ? "1" : "0"} +
                          (health.runtime_ready ? "1" : "0") +
                          (health.scheduler_ready ? "1" : "0") + (health.model_ready ? "1" : "0");
    }
    for (const auto& observation : batch.adapters) {
      if (observation.available) {
        if (const auto signature = websocket_adapter_signature(observation.record)) {
          adapter_signatures_[observation.record.resource().id()] = *signature;
        }
      }
    }
    for (const auto& observation : batch.plugins) {
      if (observation.available) {
        if (const auto signature = websocket_plugin_signature(observation.record)) {
          plugin_signatures_[observation.record.instance_id()] = *signature;
        }
      }
    }
  }
  // Append exactly the owner-confirmed changes of one v1.2 feed page at increasing session
  // sequences. The owner cursor stays attached to every event so replay/resume can continue the
  // owner journal without inventing cursor state.
  bool apply_scoped_page(const WebSocketScopedBatch& batch) {
    const auto emit = [&](const auto& change) -> bool {
      const auto head = store_->head(session_id_, handle());
      if (!head) {
        abort_now();
        return false;
      }
      const auto sequence = *head + 1;
      const auto event = websocket_feed_event(change, sequence);
      if (const auto* failure = std::get_if<WebSocketFailure>(&event)) {
        begin_close(close_code(*failure));
        return false;
      }
      if (!store_->append(session_id_, handle(), sequence, std::get<std::string>(event),
                          change.cursor())) {
        abort_now();
        return false;
      }
      return true;
    };
    for (const auto& change : batch.logs) {
      if (!emit(change)) {
        return false;
      }
    }
    for (const auto& change : batch.tasks) {
      if (!emit(change)) {
        return false;
      }
    }
    for (const auto& change : batch.messages) {
      if (!emit(change)) {
        return false;
      }
    }
    if (!store_->set_owner_cursor(session_id_, handle(), batch.next_cursor)) {
      abort_now();
      return false;
    }
    return true;
  }
  bool apply_page(const v1::OperationChangesResponse& page) {
    for (const auto& change : page.changes()) {
      const auto head = store_->head(session_id_, handle());
      if (!head) {
        abort_now();
        return false;
      }
      const auto sequence = *head + 1;
      const auto event = websocket_operation_event(change, sequence);
      if (const auto* failure = std::get_if<WebSocketFailure>(&event)) {
        begin_close(close_code(*failure));
        return false;
      }
      const auto appended = store_->append(session_id_, handle(), sequence,
                                           std::get<std::string>(event), change.cursor());
      if (!appended) {
        abort_now();
        return false;
      }
    }
    if (page.has_next_cursor() &&
        !store_->set_owner_cursor(session_id_, handle(), page.next_cursor())) {
      abort_now();
      return false;
    }
    return true;
  }
  // ---- outbound writes ---------------------------------------------------
  void enqueue(WriteFrame frame) {
    if (state_ != State::Established) {
      return;
    }
    frames_.push_back(std::move(frame));
    pump_writes();
  }
  void pump_writes() {
    if (write_pending_ || frames_.empty() || state_ != State::Established) {
      return;
    }
    write_pending_ = true;
    beast::get_lowest_layer(ws_).expires_after(config_.write_timeout);
    ws_.async_write(
        asio::buffer(frames_.front().json),
        asio::bind_executor(strand_, [self = shared_from_this()](const Error& error, std::size_t) {
          self->on_write(error);
        }));
  }
  void on_write(const Error& error) {
    write_pending_ = false;
    beast::get_lowest_layer(ws_).expires_never();
    if (state_ == State::Closing) {
      if (pending_close_write_ && !close_sent_ && !force_started_) {
        pending_close_write_ = false;
        start_close_write();
        return;
      }
      maybe_finalize();
      return;
    }
    if (state_ != State::Established) {
      return;
    }
    if (error) {
      // A write error leaves the current connection unusable; the pending read is cancelled by
      // teardown and the connection finishes once that handler re-enters the strand.
      abort_now();
      return;
    }
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    if (frame.sequence != 0) {
      store_->acknowledge(session_id_, handle(), frame.sequence);
    }
    if (frame.after) {
      frame.after();
      if (state_ != State::Established) {
        return;
      }
    }
    if (backpressure_ && frames_.empty()) {
      begin_close(1013);
      return;
    }
    pump_writes();
    if (!write_pending_ && frames_.empty()) {
      asio::post(strand_, [self = shared_from_this()] {
        if (self->active()) {
          self->pump_events();
        }
      });
    }
  }
  void pump_events() {
    if (!active() || !accept_events_) {
      return;
    }
    const auto pending = store_->pending(session_id_, handle());
    if (pending.frames > config_.maximum_pending_frames ||
        pending.bytes > config_.maximum_pending_bytes) {
      if (write_pending_) {
        backpressure_ = true;
        return;
      }
      begin_close(1013);
      return;
    }
    if (!frames_.empty() || write_pending_) {
      return;
    }
    if (std::chrono::steady_clock::now() - last_confirmed_ > config_.authorization_stale) {
      // Never deliver a notification that the current confirm window no longer covers.
      begin_close(1011);
      return;
    }
    const auto accepted = store_->accepted(session_id_, handle());
    if (!accepted) {
      return;
    }
    const auto event = store_->next_event(session_id_, handle(), *accepted);
    if (!event) {
      return;
    }
    enqueue({event->json, event->sequence, {}});
  }
  // ---- teardown ----------------------------------------------------------
  void begin_close(std::uint16_t code) {
    if (state_ == State::Closing || state_ == State::Finished) {
      return;
    }
    state_ = State::Closing;
    close_code_ = code;
    accept_events_ = false;
    access_token_.clear();
    cancel_timers();
    force_timer_.expires_after(config_.close_timeout);
    force_timer_.async_wait(
        asio::bind_executor(strand_, [self = shared_from_this()](const Error& error) {
          if (!error && self->state_ == State::Closing) {
            self->force_abort();
          }
        }));
    if (write_pending_ || accept_pending_) {
      pending_close_write_ = true;
      return;
    }
    start_close_write();
  }
  void start_close_write() {
    if (close_sent_ || state_ != State::Closing) {
      return;
    }
    close_sent_ = true;
    pending_close_write_ = false;
    if (!ws_.is_open()) {
      maybe_finalize();
      return;
    }
    write_pending_ = true;
    ws_.async_close(websocket::close_reason{static_cast<websocket::close_code>(close_code_), ""},
                    asio::bind_executor(strand_, [self = shared_from_this()](const Error&) {
                      self->on_close_write();
                    }));
  }
  void on_close_write() {
    write_pending_ = false;
    maybe_finalize();
  }
  void force_abort() {
    if (state_ == State::Finished) {
      return;
    }
    force_started_ = true;
    cancel_timers();
    Error ignored;
    auto& socket = beast::get_lowest_layer(ws_).socket();
    socket.cancel(ignored);
    if (!read_pending_ && !write_pending_ && !accept_pending_) {
      finish();
    }
  }
  void abort_now() {
    if (state_ == State::Closing || state_ == State::Finished) {
      return;
    }
    state_ = State::Closing;
    accept_events_ = false;
    authenticating_ = false;
    access_token_.clear();
    cancel_timers();
    force_started_ = true;
    Error ignored;
    auto& socket = beast::get_lowest_layer(ws_).socket();
    socket.cancel(ignored);
    if (!read_pending_ && !write_pending_ && !accept_pending_) {
      finish();
    }
  }
  void maybe_finalize() {
    if (state_ != State::Closing) {
      return;
    }
    if (read_pending_ || write_pending_ || accept_pending_) {
      return;
    }
    finish();
  }
  void finish() {
    if (state_ == State::Finished) {
      return;
    }
    state_ = State::Finished;
    cancel_timers();
    accept_events_ = false;
    authenticating_ = false;
    access_token_.clear();
    frames_.clear();
    if (!session_id_.empty()) {
      store_->detach(session_id_, handle_);
    }
    Error ignored;
    auto& socket = beast::get_lowest_layer(ws_).socket();
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    auto completed = std::move(completed_);
    completed_ = nullptr;
    if (completed) {
      completed(this);
    }
  }

  WebSocketStream ws_;
  http::request<http::string_body> request_;
  WebSocketSelection route_;
  std::shared_ptr<WebSocketStreamStore> store_;
  std::shared_ptr<WebSocketSourcePort> source_;
  asio::thread_pool& workers_;
  WebSocketSessionConfig config_;
  std::function<void(WebSocketConnection*)> completed_;
  Strand strand_;
  asio::steady_timer first_frame_timer_;
  asio::steady_timer poll_timer_;
  asio::steady_timer force_timer_;
  beast::flat_buffer buffer_;
  std::shared_ptr<WebSocketSocketHandle> handle_;
  std::string access_token_;
  std::optional<std::string> health_signature_;
  std::map<std::string, std::string> adapter_signatures_;
  std::map<std::string, std::string> plugin_signatures_;
  std::string session_id_;
  WebSocketIdentity established_identity_;
  std::chrono::steady_clock::time_point last_confirmed_{};
  std::deque<WriteFrame> frames_;
  State state_ = State::Idle;
  std::uint16_t close_code_ = 0;
  bool read_pending_ = false;
  bool write_pending_ = false;
  bool accept_pending_ = false;
  bool close_sent_ = false;
  bool pending_close_write_ = false;
  bool force_started_ = false;
  bool poll_in_flight_ = false;
  bool authenticating_ = false;
  bool accept_events_ = false;
  bool backpressure_ = false;
};
} // namespace

std::shared_ptr<WebSocketConnection>
make_web_socket_connection(WebSocketStream stream, http::request<http::string_body> request,
                           WebSocketSelection route, std::shared_ptr<WebSocketStreamStore> store,
                           std::shared_ptr<WebSocketSourcePort> source, asio::thread_pool& workers,
                           asio::io_context& io, WebSocketSessionConfig config,
                           std::function<void(WebSocketConnection*)> completed) {
  return std::make_shared<Connection>(std::move(stream), std::move(request), std::move(route),
                                      std::move(store), std::move(source), workers, io, config,
                                      std::move(completed));
}

} // namespace kiarabot::control::target
