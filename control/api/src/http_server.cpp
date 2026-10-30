#include "kiarabot/control/http_server.hpp"

#include <charconv>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "kiarabot/control/event_stream.hpp"
#include "kiarabot/control/http_mapper.hpp"

namespace kiarabot::control {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = net::ip::tcp;
namespace websocket = beast::websocket;
class WebSocket : public std::enable_shared_from_this<WebSocket> {
public:
  WebSocket(beast::tcp_stream stream, EventStream& events, std::string channel)
      : socket_(std::move(stream)), timer_(socket_.get_executor()), events_(events),
        channel_(std::move(channel)) {}
  ~WebSocket() {
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
      if (!error) {
        self->read();
        self->schedule(std::chrono::seconds(5));
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
        const auto frame = core::ControlJson::parse(beast::buffers_to_string(self->buffer_.data()));
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
      if (error || self->finished_) {
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
    if (queue_.size() + reply.frames.size() > 64) {
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
    timer_.cancel();
    if (!id_.empty()) {
      events_.detach(id_);
      id_.clear();
    }
    boost::system::error_code ignored;
    beast::get_lowest_layer(socket_).socket().close(ignored);
  }
  websocket::stream<beast::tcp_stream> socket_;
  net::steady_timer timer_;
  EventStream& events_;
  std::string channel_;
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
             std::vector<std::weak_ptr<WebSocket>>& sockets)
      : stream_(std::move(socket)), application_(application), events_(events), sockets_(sockets) {
    parser_.body_limit(65536);
    parser_.header_limit(16384);
  }
  void start() {
    stream_.expires_after(std::chrono::seconds(5));
    http::async_read(stream_, buffer_, parser_, [self = shared_from_this()](auto error, auto) {
      if (!error) {
        self->respond();
      } else if (error == http::error::body_limit || error == http::error::header_limit ||
                 error == http::error::bad_content_length ||
                 error == http::error::bad_transfer_encoding) {
        self->send_reply(
            {400,
             R"({"code":"INVALID_ARGUMENT","message":"INVALID_ARGUMENT","request_id":"transport","trace_id":"transport","retryable":false})",
             {}});
      }
    });
  }

private:
  void respond() {
    const auto& input = parser_.get();
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
      if (sockets_.size() >= 128) {
        return;
      }
      auto connection =
          std::make_shared<WebSocket>(std::move(stream_), events_, request.target.substr(4));
      sockets_.push_back(connection);
      connection->start(parser_.release());
      return;
    }
    const auto output =
        duplicate
            ? HttpReply{400,
                        R"({"code":"INVALID_ARGUMENT","message":"INVALID_ARGUMENT","request_id":"transport","trace_id":"transport","retryable":false})",
                        {}}
            : handle_http(application_, request);
    send_reply(output);
  }
  void send_reply(const HttpReply& output) {
    response_.result(static_cast<http::status>(output.status));
    response_.version(11);
    response_.set(http::field::content_type, "application/json");
    response_.set(http::field::cache_control, "no-store");
    response_.set("X-Content-Type-Options", "nosniff");
    response_.keep_alive(false);
    for (const auto& [name, value] : output.headers) {
      response_.set(name, value);
    }
    response_.body() = output.body;
    response_.prepare_payload();
    stream_.expires_after(std::chrono::seconds(5));
    http::async_write(stream_, response_, [self = shared_from_this()](auto, auto) {
      boost::system::error_code ignored;
      self->stream_.socket().shutdown(Tcp::socket::shutdown_both, ignored);
    });
  }
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  http::response<http::string_body> response_;
  core::IControlApplication& application_;
  EventStream& events_;
  std::vector<std::weak_ptr<WebSocket>>& sockets_;
};
} // namespace
struct HttpServer::Impl {
  core::IControlApplication& application;
  EventStream events;
  net::io_context io;
  net::steady_timer ticker{io};
  Tcp::acceptor acceptor{io};
  std::jthread worker;
  unsigned short selected_port{0};
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
      if (!error) {
        events.tick();
        tick();
      }
    });
  }
  void accept() {
    acceptor.async_accept([this](auto error, Tcp::socket socket) {
      if (!error) {
        std::erase_if(connections, [](const auto& connection) { return connection.expired(); });
        if (connections.size() < 128) {
          auto connection =
              std::make_shared<Connection>(std::move(socket), application, events, sockets);
          connections.push_back(connection);
          connection->start();
        }
      }
      if (acceptor.is_open()) {
        accept();
      }
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
    impl_->acceptor.open(Tcp::v4());
    impl_->acceptor.bind({net::ip::address_v4::loopback(), static_cast<unsigned short>(port)});
    impl_->acceptor.listen(128);
    impl_->selected_port = impl_->acceptor.local_endpoint().port();
    impl_->accept();
    impl_->tick();
    impl_->worker = std::jthread([this] { impl_->io.run(); });
    return true;
  } catch (...) {
    return false;
  }
}
void HttpServer::stop() {
  impl_->io.stop();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}
unsigned short HttpServer::port() const {
  return impl_->selected_port;
}
} // namespace kiarabot::control
