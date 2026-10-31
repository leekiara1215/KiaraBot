#include "kiarabot/control/http_server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "kiarabot/control/configuration.hpp"
#include "kiarabot/control/event_stream.hpp"
#include "kiarabot/control/http_mapper.hpp"
#include "kiarabot/core/application/control_service.hpp"
#include "kiarabot/core/runtime/request_digest.hpp"

namespace kiarabot::control {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = net::ip::tcp;
using Json = core::ControlJson;
using Clock = std::chrono::steady_clock;
namespace websocket = beast::websocket;

constexpr std::size_t kMaxConnections = 128;
constexpr std::size_t kMaxSockets = 128;
constexpr std::size_t kMaxPendingFrames = 64;
constexpr std::uint32_t kShutdownCloseCode = 1001;

std::string header(const HttpRequest& request, const std::string& key) {
  const auto found = request.headers.find(key);
  return found == request.headers.end() ? "" : found->second;
}

bool bearer_authorization(const HttpRequest& request) {
  const auto auth = header(request, "authorization");
  if (auth.size() <= 7) {
    return false;
  }
  return std::equal(auth.begin(), auth.begin() + 7, "Bearer ", [](char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
  });
}

std::string query_free_target(const std::string& target) {
  return target.substr(0, target.find('?'));
}

struct RateBucket {
  double tokens{0.0};
  Clock::time_point updated{};
};

struct ServerState {
  TransportSettings settings;
  std::atomic<bool> shutting_down{false};
  std::atomic<std::uint32_t> in_flight{0};
  std::atomic<std::uint32_t> websockets{0};
  std::unordered_map<std::string, RateBucket> buckets;
  Clock::time_point pruned{};

  bool origin_allowed(const std::string& origin) const {
    return std::find(settings.cors_origins.begin(), settings.cors_origins.end(), origin) !=
           settings.cors_origins.end();
  }
  bool rate_allow(const std::string& key, double rate, double burst, Clock::time_point now) {
    auto& bucket = buckets[key];
    if (bucket.updated.time_since_epoch().count() == 0) {
      bucket.tokens = burst;
      bucket.updated = now;
    }
    const auto elapsed = std::chrono::duration<double>(now - bucket.updated).count();
    bucket.tokens = std::min(burst, bucket.tokens + elapsed * rate);
    bucket.updated = now;
    if (bucket.tokens < 1.0) {
      return false;
    }
    bucket.tokens -= 1.0;
    return true;
  }
  void prune_buckets(Clock::time_point now) {
    if (now - pruned < std::chrono::minutes(1)) {
      return;
    }
    pruned = now;
    std::erase_if(buckets, [&](const auto& entry) {
      return entry.second.updated + std::chrono::minutes(1) <= now;
    });
  }
};

HttpReply transport_error(int status, const std::string& code) {
  const auto request_id = core::random_instance_id();
  const auto trace_id = core::random_instance_id();
  HttpReply reply;
  reply.status = status;
  reply.body = Json{{"code", code},         {"message", code},    {"request_id", request_id},
                    {"trace_id", trace_id}, {"retryable", false}, {"details", Json::object()}}
                   .dump();
  reply.headers["X-Request-ID"] = request_id;
  reply.headers["X-Trace-ID"] = trace_id;
  return reply;
}

HttpReply rate_limited() {
  auto reply = transport_error(429, "RATE_LIMITED");
  reply.headers["Retry-After"] = "1";
  return reply;
}

void apply_cors(const ServerState& state, const HttpRequest& request, HttpReply& reply) {
  const auto origin = header(request, "origin");
  if (origin.empty() || !state.origin_allowed(origin)) {
    return;
  }
  reply.headers["Access-Control-Allow-Origin"] = origin;
  reply.headers["Vary"] = "Origin";
  if (request.method == "OPTIONS") {
    reply.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, HEAD, OPTIONS";
    reply.headers["Access-Control-Allow-Headers"] =
        "Authorization, Content-Type, Idempotency-Key, X-Request-ID, X-Trace-ID, Traceparent";
    reply.headers["Access-Control-Max-Age"] = "600";
  }
}

void log_access(const std::string& remote, const HttpRequest& request, const HttpReply& reply,
                std::chrono::steady_clock::duration elapsed) {
  const auto* request_id =
      reply.headers.contains("X-Request-ID") ? &reply.headers.at("X-Request-ID") : nullptr;
  const auto* trace_id =
      reply.headers.contains("X-Trace-ID") ? &reply.headers.at("X-Trace-ID") : nullptr;
  const auto status = reply.status;
  std::cerr << Json{{"timestamp", core::control_timestamp(std::chrono::system_clock::now())},
                    {"level", status >= 500   ? "ERROR"
                              : status >= 400 ? "WARN"
                                              : "INFO"},
                    {"component", "control-api"},
                    {"operation", "http_request"},
                    {"outcome", status < 400 ? "ok" : "rejected"},
                    {"method", request.method},
                    {"path", query_free_target(request.target)},
                    {"status", status},
                    {"duration_ms",
                     std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()},
                    {"request_id", request_id ? *request_id : std::string()},
                    {"trace_id", trace_id ? *trace_id : std::string()},
                    {"remote", remote},
                    {"bytes", reply.body.size()}}
                   .dump()
            << '\n';
}

class WebSocket : public std::enable_shared_from_this<WebSocket> {
public:
  WebSocket(beast::tcp_stream stream, EventStream& events, std::string channel, ServerState& state)
      : socket_(std::move(stream)), timer_(socket_.get_executor()),
        force_timer_(socket_.get_executor()), events_(events), channel_(std::move(channel)),
        state_(state) {
    state_.websockets.fetch_add(1);
  }
  ~WebSocket() {
    if (!finished_) {
      finished_ = true;
      state_.websockets.fetch_sub(1);
    }
    if (!id_.empty()) {
      events_.detach(id_);
    }
  }
  void start(http::request<http::string_body> request) {
    beast::get_lowest_layer(socket_).expires_never();
    socket_.set_option(
        websocket::stream_base::timeout{std::chrono::seconds(5), std::chrono::seconds(30), true});
    socket_.read_message_max(16384);
    socket_.async_accept(request, [self = shared_from_this()](auto error) {
      if (error) {
        self->finish();
        return;
      }
      self->read();
      self->schedule(std::chrono::seconds(5));
    });
  }
  // Called from the server shutdown path (io thread): send a going-away close frame after the
  // current backlog drains, then stop polling so the socket can finish its close handshake.
  void shutdown(unsigned short code) {
    if (finished_) {
      return;
    }
    close(code);
    force_timer_.expires_after(std::chrono::milliseconds(300));
    force_timer_.async_wait([self = shared_from_this()](auto) {
      if (!self->finished_) {
        self->finish();
      }
    });
  }

private:
  void read() {
    socket_.async_read(buffer_, [self = shared_from_this()](auto error, auto) {
      if (error) {
        self->finish();
        return;
      }
      if (!self->id_.empty() || !self->socket_.got_text()) {
        self->close(4400);
        return;
      }
      try {
        const auto frame = Json::parse(beast::buffers_to_string(self->buffer_.data()));
        self->buffer_.consume(self->buffer_.size());
        const auto reply = self->events_.handshake(self->channel_, frame);
        for (const auto& output : reply.frames) {
          if (output.value("type", "") == "SESSION_READY") {
            self->id_ = output.at("session_id");
            self->sequence_ = output.at("last_sequence");
          }
        }
        self->enqueue(reply);
        if (!self->id_.empty() && !reply.close_code) {
          self->timer_.cancel();
          self->schedule(std::chrono::milliseconds(100));
          self->read();
        }
      } catch (...) {
        self->close(4400);
      }
    });
  }
  void schedule(std::chrono::milliseconds duration) {
    timer_.expires_after(duration);
    timer_.async_wait([self = shared_from_this()](auto error) {
      if (error || self->finished_ || self->state_.shutting_down) {
        return;
      }
      if (self->id_.empty()) {
        self->close(4400);
        return;
      }
      self->enqueue(self->events_.poll(self->id_, self->sequence_));
      if (!self->closing_) {
        self->schedule(std::chrono::milliseconds(100));
      }
    });
  }
  void enqueue(const StreamReply& reply) {
    if (finished_ || closing_) {
      return;
    }
    if (queue_.size() + reply.frames.size() > kMaxPendingFrames) {
      close(1013);
      return;
    }
    const bool writing = !queue_.empty();
    for (const auto& frame : reply.frames) {
      queue_.push_back(frame.dump());
      if (frame.contains("sequence")) {
        sequence_ = frame.at("sequence");
      }
    }
    closing_ = reply.close_code;
    if (!writing && !queue_.empty()) {
      write();
    } else if (queue_.empty() && closing_) {
      close(closing_);
    }
  }
  void write() {
    socket_.text(true);
    socket_.async_write(net::buffer(queue_.front()), [self = shared_from_this()](auto error, auto) {
      if (error) {
        self->finish();
        return;
      }
      self->queue_.pop_front();
      if (!self->queue_.empty()) {
        self->write();
      } else if (self->closing_) {
        self->close(self->closing_);
      }
    });
  }
  void close(unsigned short code) {
    if (finished_) {
      return;
    }
    closing_ = code;
    if (!queue_.empty()) {
      return;
    }
    timer_.cancel();
    socket_.async_close(websocket::close_reason(code),
                        [self = shared_from_this()](auto) { self->finish(); });
  }
  void finish() {
    if (finished_) {
      return;
    }
    finished_ = true;
    state_.websockets.fetch_sub(1);
    timer_.cancel();
    force_timer_.cancel();
    if (!id_.empty()) {
      events_.detach(id_);
      id_.clear();
    }
    boost::system::error_code ignored;
    beast::get_lowest_layer(socket_).socket().close(ignored);
  }
  websocket::stream<beast::tcp_stream> socket_;
  net::steady_timer timer_;
  net::steady_timer force_timer_;
  EventStream& events_;
  std::string channel_;
  ServerState& state_;
  std::string id_;
  std::uint64_t sequence_{0};
  beast::flat_buffer buffer_;
  std::deque<std::string> queue_;
  unsigned short closing_{0};
  bool finished_{false};
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
  Connection(Tcp::socket socket, core::IControlApplication& application, EventStream& events,
             std::vector<std::weak_ptr<WebSocket>>& sockets, ServerState& state, std::string remote)
      : stream_(std::move(socket)), application_(application), events_(events), sockets_(sockets),
        state_(state), remote_(std::move(remote)) {
    reset_parser();
  }
  void start() {
    read();
  }
  void begin_shutdown() {
    if (detached_) {
      return;
    }
    closing_ = true;
    if (in_flight_) {
      close_after_write_ = true;
      return;
    }
    close_socket();
  }

private:
  void reset_parser() {
    parser_.reset();
    parser_.emplace();
    parser_->body_limit(65536);
    parser_->header_limit(16384);
  }
  void read() {
    if (closing_ || state_.shutting_down) {
      close_socket();
      return;
    }
    stream_.expires_after(std::chrono::seconds(state_.settings.idle_timeout_seconds));
    http::async_read(stream_, buffer_, *parser_, [self = shared_from_this()](auto error, auto) {
      if (error) {
        if (error == http::error::body_limit || error == http::error::header_limit ||
            error == http::error::bad_content_length ||
            error == http::error::bad_transfer_encoding) {
          self->send_transport_error(400, "INVALID_ARGUMENT");
        } else {
          self->close_socket();
        }
        return;
      }
      self->respond();
    });
  }
  void respond() {
    const auto& input = parser_->get();
    HttpRequest request{
        std::string(input.method_string()), std::string(input.target()), {}, input.body()};
    bool duplicate = false;
    for (const auto& entry : input) {
      std::string name(entry.name_string());
      for (auto& ch : name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      }
      if (!request.headers.emplace(name, std::string(entry.value())).second) {
        duplicate = true;
      }
    }
    if (!duplicate && websocket::is_upgrade(input) && request.target.starts_with("/ws/") &&
        request.target.find_first_of("?%#", 4) == std::string::npos) {
      std::erase_if(sockets_, [](const auto& socket) { return socket.expired(); });
      if (sockets_.size() >= kMaxSockets) {
        close_socket();
        return;
      }
      auto connection = std::make_shared<WebSocket>(std::move(stream_), events_,
                                                    request.target.substr(4), state_);
      sockets_.push_back(connection);
      connection->start(parser_->release());
      detached_ = true;
      return;
    }
    const bool head = request.method == "HEAD";
    const bool keep_alive = !duplicate && !state_.shutting_down && input.keep_alive() &&
                            requests_ + 1 < state_.settings.max_requests_per_connection;
    const auto started = Clock::now();
    in_flight_ = true;
    state_.in_flight.fetch_add(1);
    HttpReply reply;
    if (duplicate) {
      reply = transport_error(400, "INVALID_ARGUMENT");
    } else {
      const auto now = Clock::now();
      const bool authenticated = bearer_authorization(request);
      const auto& settings = state_.settings;
      const auto key = authenticated ? "token:" + std::to_string(std::hash<std::string>{}(
                                                      header(request, "authorization")))
                                     : "peer:" + remote_.substr(0, remote_.rfind(':'));
      const bool allowed = authenticated
                               ? state_.rate_allow(key, settings.authenticated_requests_per_second,
                                                   settings.authenticated_burst, now)
                               : state_.rate_allow(key, settings.public_requests_per_second,
                                                   settings.public_burst, now);
      reply = allowed ? handle_http(application_, request) : rate_limited();
      apply_cors(state_, request, reply);
      log_access(remote_, request, reply, Clock::now() - started);
    }
    if (duplicate) {
      log_access(remote_, request, reply, Clock::now() - started);
    }
    send_reply(std::move(reply), head, keep_alive);
  }
  void send_transport_error(int status, const std::string& code) {
    in_flight_ = true;
    state_.in_flight.fetch_add(1);
    auto reply = transport_error(status, code);
    log_access(remote_, HttpRequest{}, reply, std::chrono::steady_clock::duration{});
    send_reply(std::move(reply), false, false);
  }
  void send_reply(HttpReply reply, bool head, bool keep_alive) {
    keep_alive_ = keep_alive;
    response_.result(static_cast<http::status>(reply.status));
    response_.version(11);
    response_.set(http::field::content_type, "application/json");
    response_.set(http::field::cache_control, "no-store");
    response_.set("X-Content-Type-Options", "nosniff");
    response_.keep_alive(keep_alive);
    for (const auto& [name, value] : reply.headers) {
      response_.set(name, value);
    }
    if (head) {
      response_.body().clear();
      response_.content_length(0);
    } else {
      response_.body() = std::move(reply.body);
      response_.prepare_payload();
    }
    stream_.expires_after(std::chrono::seconds(5));
    http::async_write(stream_, response_, [self = shared_from_this()](auto error, auto) {
      self->state_.in_flight.fetch_sub(1);
      self->in_flight_ = false;
      if (error || !self->keep_alive_ || self->close_after_write_ || self->state_.shutting_down) {
        self->close_socket();
        return;
      }
      self->requests_ += 1;
      self->reset_parser();
      self->read();
    });
  }
  void close_socket() {
    if (detached_) {
      return;
    }
    closing_ = true;
    boost::system::error_code ignored;
    stream_.socket().shutdown(Tcp::socket::shutdown_both, ignored);
    stream_.socket().close(ignored);
  }
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  http::response<http::string_body> response_;
  core::IControlApplication& application_;
  EventStream& events_;
  std::vector<std::weak_ptr<WebSocket>>& sockets_;
  ServerState& state_;
  std::string remote_;
  std::size_t requests_{0};
  bool in_flight_{false};
  bool keep_alive_{false};
  bool closing_{false};
  bool close_after_write_{false};
  bool detached_{false};
};
} // namespace

struct HttpServer::Impl {
  core::IControlApplication& application;
  ServerState state;
  EventStream events;
  net::io_context io;
  net::steady_timer ticker{io};
  Tcp::acceptor acceptor{io};
  std::jthread worker;
  unsigned short selected_port{0};
  bool started{false};
  bool stopping{false};
  std::vector<std::weak_ptr<Connection>> connections;
  std::vector<std::weak_ptr<WebSocket>> sockets;
  explicit Impl(core::IControlApplication& app)
      : application(app),
        events([&app](const auto& metadata) -> core::PortResult<core::ControlJson> {
          return app.stream_snapshot(metadata);
        }) {}
  void tick() {
    ticker.expires_after(std::chrono::milliseconds(100));
    ticker.async_wait([this](auto error) {
      if (error || state.shutting_down) {
        return;
      }
      events.tick();
      state.prune_buckets(Clock::now());
      tick();
    });
  }
  void accept() {
    acceptor.async_accept([this](auto error, Tcp::socket socket) {
      if (!error && !state.shutting_down) {
        std::erase_if(connections, [](const auto& connection) { return connection.expired(); });
        if (connections.size() < kMaxConnections) {
          std::string remote;
          boost::system::error_code ignored;
          const auto endpoint = socket.remote_endpoint(ignored);
          if (!ignored) {
            remote = endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
          }
          auto connection = std::make_shared<Connection>(std::move(socket), application, events,
                                                         sockets, state, std::move(remote));
          connections.push_back(connection);
          connection->start();
        } else {
          boost::system::error_code ignored;
          socket.close(ignored);
        }
      }
      if (!state.shutting_down && acceptor.is_open()) {
        accept();
      }
    });
  }
  void begin_shutdown(std::function<void()> done) {
    state.shutting_down = true;
    boost::system::error_code ignored;
    acceptor.close(ignored);
    ticker.cancel();
    for (auto& weak : sockets) {
      if (auto socket = weak.lock()) {
        socket->shutdown(kShutdownCloseCode);
      }
    }
    for (auto& weak : connections) {
      if (auto connection = weak.lock()) {
        connection->begin_shutdown();
      }
    }
    drain(std::move(done),
          Clock::now() + std::chrono::seconds(state.settings.drain_timeout_seconds));
  }
  void drain(std::function<void()> done, Clock::time_point deadline) {
    const auto now = Clock::now();
    if (state.in_flight.load() == 0 && state.websockets.load() == 0) {
      io.stop();
      done();
      return;
    }
    if (now >= deadline) {
      io.stop();
      done();
      return;
    }
    const auto delay = std::min<Clock::duration>(std::chrono::milliseconds(20), deadline - now);
    auto timer = std::make_shared<net::steady_timer>(io, delay);
    timer->async_wait([this, done = std::move(done), deadline, timer](auto) mutable {
      drain(std::move(done), deadline);
    });
  }
};

HttpServer::HttpServer(core::IControlApplication& application)
    : impl_(std::make_unique<Impl>(application)) {}
HttpServer::~HttpServer() {
  stop();
}
bool HttpServer::start(const std::string& address) {
  if (!address.starts_with("127.0.0.1:") || impl_->worker.joinable()) {
    return false;
  }
  unsigned port = 0;
  const auto parsed = std::from_chars(address.data() + 10, address.data() + address.size(), port);
  if (parsed.ec != std::errc{} || parsed.ptr != address.data() + address.size() || port > 65535) {
    return false;
  }
  try {
    // The composition root already validated the private file; the transport re-reads its own
    // optional settings from the same file so that no public constructor changes.
    if (const char* path = std::getenv("KIARABOT_CONTROL_CONFIG");
        path != nullptr && *path != '\0') {
      impl_->state.settings = read_configuration(path).transport;
    }
    impl_->events.set_max_streams_per_token(impl_->state.settings.max_streams_per_token);
    impl_->acceptor.open(Tcp::v4());
    impl_->acceptor.bind({net::ip::address_v4::loopback(), static_cast<unsigned short>(port)});
    impl_->acceptor.listen(static_cast<int>(kMaxConnections));
    impl_->selected_port = impl_->acceptor.local_endpoint().port();
    impl_->started = true;
    impl_->accept();
    impl_->tick();
    impl_->worker = std::jthread([this] { impl_->io.run(); });
    return true;
  } catch (...) {
    return false;
  }
}
void HttpServer::stop() {
  if (!impl_ || impl_->stopping) {
    return;
  }
  impl_->stopping = true;
  if (!impl_->started || !impl_->worker.joinable()) {
    impl_->io.stop();
    return;
  }
  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();
  net::post(impl_->io, [this, done] { impl_->begin_shutdown([done] { done->set_value(); }); });
  if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    impl_->io.stop();
  }
  impl_->worker.join();
}
unsigned short HttpServer::port() const {
  return impl_->selected_port;
}
} // namespace kiarabot::control
