#include "kiarabot/control/target/websocket_session.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
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
using Timer = asio::steady_timer;

std::uint16_t close_code(const WebSocketFailure& failure) {
  return websocket_close_code(failure);
}

class Connection final : public WebSocketConnection,
                         public std::enable_shared_from_this<Connection> {
public:
  Connection(WebSocketStream stream, http::request<http::string_body> request,
             WebSocketSelection route, std::shared_ptr<WebSocketStreamStore> store,
             std::shared_ptr<WebSocketSourcePort> source, asio::thread_pool& workers,
             WebSocketSessionConfig config, std::function<void(WebSocketConnection*)> completed)
      : ws_(std::move(stream)), request_(std::move(request)), route_(std::move(route)),
        store_(std::move(store)), source_(std::move(source)), workers_(workers), config_(config),
        completed_(std::move(completed)), executor_(ws_.get_executor()),
        first_frame_timer_(executor_), poll_timer_(executor_), close_timer_(executor_) {
    ws_.read_message_max(kMaximumFirstFrameBytes);
    ws_.text(true);
    ws_.auto_fragment(true);
  }
  ~Connection() override {
    if (!finished_ && !session_id_.empty()) {
      store_->detach(session_id_, handle_);
    }
  }

  void start() override {
    asio::post(executor_, [self = shared_from_this()] { self->begin_accept(); });
  }

  void terminate(std::uint16_t code) override {
    asio::post(executor_, [self = shared_from_this(), code] {
      if (!self->finished_) {
        self->close(code);
      }
    });
  }

  void terminate_now(std::uint16_t code) override {
    if (!finished_) {
      close(code);
    }
  }

private:
  std::shared_ptr<WebSocketSocketHandle> handle() {
    if (!handle_) {
      handle_ = shared_from_this();
    }
    return handle_;
  }
  void begin_accept() {
    if (finished_) {
      return;
    }
    // The TLS/HTTP handshake deadline from the acceptor no longer applies; the first-frame
    // deadline is enforced by the explicit timer below and idle reads have no socket deadline.
    beast::get_lowest_layer(ws_).expires_never();
    first_frame_timer_.expires_after(config_.first_frame_timeout);
    first_frame_timer_.async_wait([self = shared_from_this()](const Error& error) {
      if (!error && !self->finished_) {
        self->close(4400);
      }
    });
    ws_.async_accept(request_, [self = shared_from_this()](const Error& error) {
      if (error) {
        self->finish();
        return;
      }
      self->read_first_frame();
    });
  }
  void read_first_frame() {
    beast::get_lowest_layer(ws_).expires_never();
    ws_.async_read(buffer_, [self = shared_from_this()](const Error& error, std::size_t) {
      self->first_frame_timer_.cancel();
      if (error) {
        self->close(4400);
        return;
      }
      auto text = beast::buffers_to_string(self->buffer_.data());
      self->buffer_.consume(self->buffer_.size());
      const auto frame = websocket_first_frame(text, self->route_);
      if (const auto* failure = std::get_if<WebSocketFailure>(&frame)) {
        self->close(close_code(*failure));
        return;
      }
      auto selected = std::get<WebSocketFirstFrame>(frame);
      // The URL route only selects channel/owner; the authenticated scope arrives in the first
      // frame and is the exact scope every owner poll re-authorizes.
      self->route_.scope = selected.scope;
      self->authenticate(std::move(selected));
    });
  }
  void authenticate(WebSocketFirstFrame frame) {
    authenticating_ = true;
    access_token_ = frame.access_token;
    const auto deadline = std::chrono::steady_clock::now() + config_.authorization_budget;
    asio::post(workers_, [self = shared_from_this(), frame = std::move(frame), deadline]() mutable {
      auto result = self->source_->poll(self->route_, frame.access_token, std::nullopt, deadline);
      asio::post(self->executor_,
                 [self, frame = std::move(frame), result = std::move(result)]() mutable {
                   self->on_authenticated(std::move(frame), std::move(result));
                 });
    });
  }
  void on_authenticated(WebSocketFirstFrame frame, WebSocketResult<WebSocketPoll> result) {
    authenticating_ = false;
    if (finished_) {
      return;
    }
    if (const auto* failure = std::get_if<WebSocketFailure>(&result)) {
      close(close_code(*failure));
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
    if (!poll.operations.has_next_cursor()) {
      close(1011);
      return;
    }
    const auto head = poll.operations.next_cursor();
    const auto created = store_->create(poll.identity, route_, head, handle());
    if (const auto* failure = std::get_if<WebSocketFailure>(&created)) {
      close(close_code(*failure));
      return;
    }
    session_id_ = std::get<std::string>(created);
    const auto ready = websocket_session_ready(session_id_, 0, head);
    if (const auto* failure = std::get_if<WebSocketFailure>(&ready)) {
      close(close_code(*failure));
      return;
    }
    established_ = true;
    enqueue({std::get<std::string>(ready), 0, {}});
    start_io_loops();
  }
  void establish_resume(WebSocketFirstFrame frame, WebSocketPoll poll) {
    const auto resumed = store_->resume(frame.session_id, poll.identity, route_,
                                        frame.last_sequence, frame.owner_cursor, handle());
    switch (resumed.status) {
    case WebSocketResumeResult::Status::Rejected:
      close(close_code(resumed.failure));
      return;
    case WebSocketResumeResult::Status::Resync: {
      const auto resync = websocket_resync_required(resumed.resync);
      if (const auto* failure = std::get_if<WebSocketFailure>(&resync)) {
        close(close_code(*failure));
        return;
      }
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
      close(close_code(*failure));
      return;
    }
    established_ = true;
    enqueue({std::get<std::string>(ready), 0, {}});
    start_io_loops();
  }
  void resync_then_close(std::string frame) {
    established_ = false;
    enqueue({std::move(frame), 0, [self = shared_from_this()] { self->close(1000); }});
  }
  void start_io_loops() {
    read_next();
    schedule_poll(std::chrono::milliseconds{0});
    pump_events();
  }
  void read_next() {
    if (finished_) {
      return;
    }
    beast::get_lowest_layer(ws_).expires_never();
    ws_.async_read(buffer_, [self = shared_from_this()](const Error& error, std::size_t size) {
      if (error) {
        self->finish();
        return;
      }
      self->buffer_.consume(size);
      // After authentication the client sends no application data frames; a repeat handshake or
      // binary frame is an invalid first-frame-class error, not a new session.
      self->close(4400);
    });
  }
  void schedule_poll(std::chrono::milliseconds delay) {
    if (finished_ || !established_) {
      return;
    }
    poll_timer_.expires_after(delay);
    poll_timer_.async_wait([self = shared_from_this()](const Error& error) {
      if (error || self->finished_ || !self->established_) {
        return;
      }
      self->begin_poll();
    });
  }
  void begin_poll() {
    if (poll_in_flight_ || authenticating_ || finished_ || !established_ || backpressure_) {
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
      asio::post(self->executor_, [self, result = std::move(result)]() mutable {
        self->on_poll(std::move(result));
      });
    });
  }
  void on_poll(WebSocketResult<WebSocketPoll> result) {
    poll_in_flight_ = false;
    if (finished_) {
      return;
    }
    if (const auto* failure = std::get_if<WebSocketFailure>(&result)) {
      if (failure->cursor_expired) {
        const auto resync = websocket_resync_required(WebSocketResyncReason::CursorExpired);
        if (const auto* rejected = std::get_if<WebSocketFailure>(&resync)) {
          close(close_code(*rejected));
        } else {
          resync_then_close(std::get<std::string>(resync));
        }
      } else {
        close(close_code(*failure));
      }
      return;
    }
    auto poll = std::get<WebSocketPoll>(std::move(result));
    // The stream capability is bound to principal/Identity session/authorization version. A
    // changed authorization context invalidates the retained replay instead of extending it.
    if (poll.identity.principal != established_identity_.principal ||
        poll.identity.identity_session != established_identity_.identity_session ||
        poll.identity.authorization_version != established_identity_.authorization_version) {
      close(4403);
      return;
    }
    last_confirmed_ = std::chrono::steady_clock::now();
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
  bool apply_page(const v1::OperationChangesResponse& page) {
    for (const auto& change : page.changes()) {
      const auto head = store_->head(session_id_, handle());
      if (!head) {
        close(1011);
        return false;
      }
      const auto sequence = *head + 1;
      const auto event = websocket_operation_event(change, sequence);
      if (const auto* failure = std::get_if<WebSocketFailure>(&event)) {
        close(close_code(*failure));
        return false;
      }
      const auto appended = store_->append(session_id_, handle(), sequence,
                                           std::get<std::string>(event), change.cursor());
      if (!appended) {
        close(1011);
        return false;
      }
    }
    if (page.has_next_cursor() &&
        !store_->set_owner_cursor(session_id_, handle(), page.next_cursor())) {
      close(1011);
      return false;
    }
    return true;
  }
  struct WriteFrame {
    std::string json;
    std::uint64_t sequence = 0;
    std::function<void()> after;
  };
  void enqueue(WriteFrame frame) {
    if (finished_) {
      return;
    }
    frames_.push_back(std::move(frame));
    pump_writes();
  }
  void pump_writes() {
    if (writing_ || frames_.empty() || finished_) {
      return;
    }
    writing_ = true;
    beast::get_lowest_layer(ws_).expires_after(config_.write_timeout);
    ws_.async_write(
        asio::buffer(frames_.front().json),
        [self = shared_from_this()](const Error& error, std::size_t) { self->on_write(error); });
  }
  void on_write(const Error& error) {
    writing_ = false;
    if (!error) {
      auto frame = std::move(frames_.front());
      frames_.pop_front();
      if (frame.sequence != 0) {
        store_->acknowledge(session_id_, handle(), frame.sequence);
      }
      if (frame.after) {
        frame.after();
      }
      if (!finished_) {
        // A write deadline must not leak onto the concurrently outstanding idle read.
        beast::get_lowest_layer(ws_).expires_never();
        if (backpressure_ && !writing_ && frames_.empty()) {
          close(1013);
          return;
        }
        pump_writes();
        if (!writing_ && frames_.empty()) {
          asio::post(executor_, [self = shared_from_this()] {
            if (!self->finished_) {
              self->pump_events();
            }
          });
        }
      }
      return;
    }
    if (error == beast::error::timeout) {
      close(1013);
    } else if (error == websocket::error::closed) {
      finish();
    } else {
      close(1011);
    }
  }
  void pump_events() {
    if (finished_ || !established_) {
      return;
    }
    // Backpressure is evaluated on every poll completion even while a write is still outstanding:
    // a slow consumer must be shed by the pending gap, not only between successful writes.
    const auto pending = store_->pending(session_id_, handle());
    if (pending.frames > config_.maximum_pending_frames ||
        pending.bytes > config_.maximum_pending_bytes) {
      if (writing_) {
        backpressure_ = true;
        return;
      }
      close(1013);
      return;
    }
    if (!frames_.empty() || writing_) {
      return;
    }
    if (std::chrono::steady_clock::now() - last_confirmed_ > config_.authorization_stale) {
      // Never deliver a notification that the current confirm window no longer covers.
      close(1011);
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
  void close(std::uint16_t code) {
    if (finished_ || closing_) {
      return;
    }
    closing_ = true;
    access_token_.clear();
    first_frame_timer_.cancel();
    poll_timer_.cancel();
    close_timer_.cancel();
    if (!ws_.is_open()) {
      finish();
      return;
    }
    if (writing_) {
      // Beast allows only one write operation at a time. If a frame is still in flight the close
      // frame cannot be started; terminating the socket keeps the one-second revocation bound.
      finish();
      return;
    }
    close_timer_.expires_after(config_.close_timeout);
    close_timer_.async_wait([self = shared_from_this()](const Error& error) {
      if (!error) {
        self->finish();
      }
    });
    ws_.async_close(websocket::close_reason{static_cast<websocket::close_code>(code), ""},
                    [self = shared_from_this()](const Error&) { self->finish(); });
  }
  void finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    established_ = false;
    access_token_.clear();
    first_frame_timer_.cancel();
    poll_timer_.cancel();
    close_timer_.cancel();
    frames_.clear();
    if (!session_id_.empty()) {
      store_->detach(session_id_, handle());
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
  asio::any_io_executor executor_;
  Timer first_frame_timer_;
  Timer poll_timer_;
  Timer close_timer_;
  beast::flat_buffer buffer_;
  std::shared_ptr<WebSocketSocketHandle> handle_;
  std::string access_token_;
  std::string session_id_;
  WebSocketIdentity established_identity_;
  std::chrono::steady_clock::time_point last_confirmed_{};
  std::deque<WriteFrame> frames_;
  bool authenticating_ = false;
  bool poll_in_flight_ = false;
  bool established_ = false;
  bool writing_ = false;
  bool closing_ = false;
  bool backpressure_ = false;
  bool finished_ = false;
};
} // namespace

std::shared_ptr<WebSocketConnection>
make_web_socket_connection(WebSocketStream stream, http::request<http::string_body> request,
                           WebSocketSelection route, std::shared_ptr<WebSocketStreamStore> store,
                           std::shared_ptr<WebSocketSourcePort> source, asio::thread_pool& workers,
                           WebSocketSessionConfig config,
                           std::function<void(WebSocketConnection*)> completed) {
  return std::make_shared<Connection>(std::move(stream), std::move(request), std::move(route),
                                      std::move(store), std::move(source), workers, config,
                                      std::move(completed));
}

} // namespace kiarabot::control::target
