#include "kiarabot/control/target/https_endpoint.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include "kiarabot/control/target/identity_auth_client.hpp"
#include "kiarabot/control/target/owner_gateway.hpp"
#include "kiarabot/control/target/websocket_endpoint.hpp"

namespace kiarabot::control::target {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using Tcp = asio::ip::tcp;
using Error = boost::system::error_code;
using Stream = beast::ssl_stream<beast::tcp_stream>;
std::unique_ptr<WebSocketEndpoint> make_websocket_endpoint(
    const core::platform::ProcessConfig& config, std::shared_ptr<IdentityAuthClient> client,
    asio::io_context& io);
core::target::Failure unavailable() {
  return {core::target::ErrorCode::UpstreamUnavailable,
          core::target::Acceptance::NotAccepted,
          core::target::Recovery::Diagnose,
          {},
          {}};
}
class Session : public std::enable_shared_from_this<Session> {
public:
  Session(Tcp::socket socket, asio::ssl::context& tls, asio::thread_pool& workers,
          std::shared_ptr<IdentityAuthClient> client, WebSocketEndpoint* websocket,
          std::function<void(Session*)> completed)
      : stream_(std::move(socket), tls), workers_(workers), client_(std::move(client)),
        websocket_(websocket), completed_(std::move(completed)) {
    parser_.body_limit(1024 * 1024);
    parser_.header_limit(8192);
  }
  void start() {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds{5});
    stream_.async_handshake(asio::ssl::stream_base::server,
                            [self = shared_from_this()](Error error) {
                              if (error) {
                                self->close();
                                return;
                              }
                              self->read();
                            });
  }
  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    Error ignored;
    beast::get_lowest_layer(stream_).socket().cancel(ignored);
    beast::get_lowest_layer(stream_).socket().shutdown(Tcp::socket::shutdown_both, ignored);
    beast::get_lowest_layer(stream_).socket().close(ignored);
    completed_(this);
  }
  // The Upgrade request is already fully parsed. Ownership of the TLS stream moves to the target
  // WebSocket boundary; the REST session must not read, write or close anything afterwards.
  void upgrade(WebSocketSelection selection, http::request<http::string_body> request) {
    auto stream = std::move(stream_);
    completed_(this);
    websocket_->accept(std::move(stream), std::move(request), std::move(selection));
  }

private:
  void read() {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds{5});
    http::async_read(
        stream_, buffer_, parser_, [self = shared_from_this()](Error error, std::size_t) {
          if (error) {
            if (error.category() == http::make_error_code(http::error::body_limit).category() &&
                error != http::error::end_of_stream) {
              self->write({400,
                           "{\"code\":\"INVALID_ARGUMENT\",\"message\":\"Invalid HTTP "
                           "request\",\"request_id\":\"control-limit\"}",
                           {}});
            } else {
              self->close();
            }
            return;
          }
          const auto& message = self->parser_.get();
          // Beast's is_upgrade() is deliberately conservative (GET + tokens only), but an
          // invalid method/version/headers still belongs to the structured Upgrade boundary, not
          // to the generic REST route table.
          const bool upgrade_hint =
              beast::websocket::is_upgrade(message) ||
              (!message[http::field::upgrade].empty() &&
               !message[http::field::connection].empty());
          if (upgrade_hint && self->websocket_ != nullptr) {
            auto check = self->websocket_->check(message);
            if (!check.accepted()) {
              self->write(std::move(*check.rejection));
              return;
            }
            self->upgrade(std::move(check.selection), message);
            return;
          }
          AuthHttpRequest input{std::string(message.method_string()),
                                std::string(message.target()),
                                {},
                                message.body()};
          for (const auto& field : message.base()) {
            input.headers.emplace_back(std::string(field.name_string()),
                                       std::string(field.value()));
          }
          // A connection owns at most one queued operation. The acceptor caps connections at 64.
          asio::post(self->workers_, [self, input = std::move(input)] {
            auto output = self->client_->handle(input);
            asio::post(self->stream_.get_executor(), [self, output = std::move(output)]() mutable {
              if (!self->closed_) {
                self->write(std::move(output));
              }
            });
          });
        });
  }
  void write(AuthHttpResponse output) {
    response_.result(static_cast<http::status>(output.status));
    response_.version(11);
    response_.set(http::field::content_type, "application/json");
    response_.set(http::field::cache_control, "no-store");
    response_.set("Pragma", "no-cache");
    response_.set("X-Content-Type-Options", "nosniff");
    response_.keep_alive(false);
    for (const auto& [name, value] : output.headers) {
      response_.set(name, value);
    }
    response_.body() = std::move(output.body);
    response_.prepare_payload();
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds{5});
    http::async_write(stream_, response_, [self = shared_from_this()](Error error, std::size_t) {
      if (error) {
        self->close();
        return;
      }
      beast::get_lowest_layer(self->stream_).expires_after(std::chrono::seconds{1});
      self->stream_.async_shutdown([self](Error) { self->close(); });
    });
  }
  Stream stream_;
  asio::thread_pool& workers_;
  std::shared_ptr<IdentityAuthClient> client_;
  WebSocketEndpoint* websocket_;
  std::function<void(Session*)> completed_;
  // Bound retained framing too: decoded body_limit alone cannot bound an unfinished chunk line.
  beast::flat_buffer buffer_{1024 * 1024 + 8192};
  http::request_parser<http::string_body> parser_;
  http::response<http::string_body> response_;
  bool closed_ = false;
};
class HttpsEndpoint final : public core::platform::ServiceEndpoint {
public:
  HttpsEndpoint(const core::platform::ProcessConfig& config,
                std::shared_ptr<ControlReadiness> readiness,
                std::unique_ptr<ControlBootstrapDelivery> bootstrap_delivery)
      : tls_(asio::ssl::context::tls_server), acceptor_(io_), workers_(4),
        bootstrap_delivery_(std::move(bootstrap_delivery)),
        client_(std::make_shared<IdentityAuthClient>(
            config.control_api->identity_endpoint,
            config.control_api->runtime_endpoint.value_or(""),
            config.control_api->scheduler_endpoint.value_or(""),
            config.control_api->model_endpoint.value_or(""),
            config.control_api->host_endpoint.value_or(""), config.tls,
            bootstrap_delivery_ ? bootstrap_delivery_->application.get() : nullptr,
            bootstrap_delivery_ ? bootstrap_delivery_->recipients.get() : nullptr)),
        websocket_(make_websocket_endpoint(config, client_, io_)),
        readiness_(std::move(readiness)), listen_(config.control_api->listen) {
    tls_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                     asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                     asio::ssl::context::no_tlsv1_1);
    if (SSL_CTX_set_min_proto_version(tls_.native_handle(), TLS1_2_VERSION) != 1) {
      throw std::runtime_error("tls_setup_failed");
    }
    tls_.use_certificate_chain(asio::buffer(config.tls.certificate));
    tls_.use_private_key(asio::buffer(config.tls.private_key), asio::ssl::context::pem);
    tls_.set_verify_mode(
        asio::ssl::verify_none); // Browser TLS; outbound client independently requires mTLS.
  }
  ~HttpsEndpoint() override {
    client_->cancel();
    if (websocket_) {
      websocket_->prepare_shutdown();
    }
    if (thread_.joinable()) {
      asio::post(io_, [this] {
        Error ignored;
        acceptor_.cancel(ignored);
        acceptor_.close(ignored);
        // close() erases from sessions_, so traverse a copy while all callbacks stay on io_.
        const auto sessions = sessions_;
        for (const auto& session : sessions) {
          session->close();
        }
        // Drain every target WebSocket session before the context stops. Destroying a
        // beast::tcp_stream with both a read and a write still pending aborts the process, so
        // shutdown must let the cancelled handlers run and the sessions erase themselves.
        auto drain = std::make_shared<asio::steady_timer>(io_);
        auto deadline = std::make_shared<std::chrono::steady_clock::time_point>(
            std::chrono::steady_clock::now() + std::chrono::seconds{2});
        auto wait = std::make_shared<std::function<void()>>();
        *wait = [this, drain, deadline, wait] {
          const bool drained = websocket_ == nullptr || websocket_->active_sockets() == 0;
          if (!drained && std::chrono::steady_clock::now() < *deadline) {
            drain->expires_after(std::chrono::milliseconds{25});
            drain->async_wait([wait](const Error&) { (*wait)(); });
            return;
          }
          // One quiescence turn lets the already-cancelled read/write handlers run before stop.
          drain->expires_after(std::chrono::milliseconds{150});
          drain->async_wait([this, drain](Error) { io_.stop(); });
        };
        (*wait)();
      });
      thread_.join();
    }
    if (websocket_) {
      websocket_->finish_shutdown();
      websocket_.reset();
    }
    workers_.stop();
    workers_.join();
  }
  core::target::Status start() override {
    if (started_) {
      return unavailable();
    }
    started_ = true;
    try {
      const auto colon = listen_.find(':');
      const Tcp::endpoint endpoint(
          asio::ip::make_address_v4(listen_.substr(0, colon)),
          static_cast<unsigned short>(std::stoul(listen_.substr(colon + 1))));
      acceptor_.open(endpoint.protocol());
      acceptor_.set_option(asio::socket_base::reuse_address(true));
      acceptor_.bind(endpoint);
      acceptor_.listen(64);
      const auto port = acceptor_.local_endpoint().port();
      accept();
      running_.store(true);
      if (readiness_) {
        readiness_->business.store(true, std::memory_order_release);
        readiness_->credentials.store(true, std::memory_order_release);
      }
      thread_ = std::thread([this] {
        try {
          io_.run();
        } catch (...) { /* Exposed as local endpoint failure, never exception text. */
        }
        running_.store(false);
      });
      std::cout << "control_https_port=" << port << '\n' << std::flush;
      return std::monostate{};
    } catch (...) {
      return unavailable();
    }
  }
  core::target::Status check() const override {
    if (readiness_) {
      readiness_->identity.store(client_->identity_ready(), std::memory_order_release);
    }
    return running_.load() ? core::target::Status{std::monostate{}}
                           : core::target::Status{unavailable()};
  }

private:
  void accept() {
    acceptor_.async_accept([this](Error error, Tcp::socket socket) {
      if (error) {
        if (error != asio::error::operation_aborted) {
          running_.store(false);
        }
        return;
      }
      if (sessions_.size() < 64) {
        auto session = std::make_shared<Session>(
            std::move(socket), tls_, workers_, client_, websocket_.get(),
            [this](Session* completed) {
              const auto found =
                  std::find_if(sessions_.begin(), sessions_.end(),
                               [completed](const auto& owned) { return owned.get() == completed; });
              if (found != sessions_.end()) {
                sessions_.erase(found);
              }
            });
        sessions_.insert(session);
        session->start();
      } else {
        Error ignored;
        socket.close(ignored);
      }
      accept();
    });
  }
  asio::io_context io_;
  asio::ssl::context tls_;
  Tcp::acceptor acceptor_;
  asio::thread_pool workers_;
  // Declared before client_ so the controlled-delivery application/recipients outlive the
  // IdentityAuthClient -> OwnerClients -> OwnerGateway that holds raw pointers into them.
  std::unique_ptr<ControlBootstrapDelivery> bootstrap_delivery_;
  std::shared_ptr<IdentityAuthClient> client_;
  std::unique_ptr<WebSocketEndpoint> websocket_;
  std::shared_ptr<ControlReadiness> readiness_;
  std::string listen_;
  std::set<std::shared_ptr<Session>> sessions_;
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::thread thread_;
};
std::unique_ptr<WebSocketEndpoint> make_websocket_endpoint(
    const core::platform::ProcessConfig& config, std::shared_ptr<IdentityAuthClient> client,
    asio::io_context& io) {
  WebSocketUpgradeConfig websocket;
  websocket.origins = config.control_api->websocket_origins;
  websocket.allow_missing_origin = config.control_api->websocket_allow_missing_origin;
  return std::make_unique<WebSocketEndpoint>(std::move(websocket), std::move(client), config, io);
}
} // namespace
std::unique_ptr<core::platform::ServiceEndpoint>
make_https_endpoint(const core::platform::ProcessConfig& config,
                    std::shared_ptr<ControlReadiness> readiness) {
  if (!config.control_api || config.service != core::target::Service::Control) {
    return {};
  }
  std::unique_ptr<ControlBootstrapDelivery> bootstrap_delivery;
  if (config.control_api->secret_delivery_recipients) {
    auto composed =
        make_control_bootstrap_delivery(*config.control_api->secret_delivery_recipients);
    if (std::holds_alternative<core::target::Failure>(composed)) {
      return {}; // registry/root/owner invalid: fail startup, never silently disable
    }
    bootstrap_delivery = std::make_unique<ControlBootstrapDelivery>(
        std::move(std::get<ControlBootstrapDelivery>(composed)));
  }
  return std::make_unique<HttpsEndpoint>(config, std::move(readiness),
                                         std::move(bootstrap_delivery));
}
} // namespace kiarabot::control::target
