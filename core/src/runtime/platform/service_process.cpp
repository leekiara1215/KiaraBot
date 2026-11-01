#include "kiarabot/core/runtime/platform/service_process.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_rpc.hpp"

namespace kiarabot::core::platform {
namespace {

class Socket {
public:
  explicit Socket(int descriptor) : descriptor_(descriptor) {}
  ~Socket() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int get() const {
    return descriptor_;
  }

private:
  int descriptor_;
};
class StopSignals {
public:
  StopSignals() {
    sigemptyset(&signals_);
    sigaddset(&signals_, SIGINT);
    sigaddset(&signals_, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals_, &previous_) != 0) {
      throw std::runtime_error("signal_setup_failed");
    }
  }
  ~StopSignals() {
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }
  StopSignals(const StopSignals&) = delete;
  StopSignals& operator=(const StopSignals&) = delete;
  bool requested() const {
    sigset_t pending;
    if (sigpending(&pending) != 0) {
      throw std::runtime_error("signal_check_failed");
    }
    if (!sigismember(&pending, SIGINT) && !sigismember(&pending, SIGTERM)) {
      return false;
    }
    int signal = 0;
    if (sigwait(&signals_, &signal) != 0) {
      throw std::runtime_error("signal_wait_failed");
    }
    return true;
  }

private:
  sigset_t signals_{};
  sigset_t previous_{};
};
bool nonblocking(int descriptor) {
  const auto flags = fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}
int listen_probe(int descriptor, const std::string& endpoint) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port =
      htons(static_cast<std::uint16_t>(std::stoul(endpoint.substr(endpoint.find(':') + 1))));
  const int reuse = 1;
  setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (!nonblocking(descriptor) ||
      bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(descriptor, 16) != 0) {
    return 0;
  }
  socklen_t size = sizeof(address);
  if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}
void serve_probe(int listener, const target::ServiceReadinessPort& application) {
  const Socket client(accept(listener, nullptr, nullptr));
  if (client.get() < 0) {
    return;
  }
  if (!nonblocking(client.get())) {
    return;
  }
#ifdef SO_NOSIGPIPE
  const int no_sigpipe = 1;
  setsockopt(client.get(), SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  std::string request;
  std::array<char, 1024> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos) {
    if (request.size() >= 8192 || std::chrono::steady_clock::now() >= deadline) {
      return;
    }
    pollfd ready{client.get(), POLLIN, 0};
    if (poll(&ready, 1, 20) < 0) {
      return;
    }
    const auto count = recv(client.get(), buffer.data(), buffer.size(), 0);
    if (count == 0) {
      return;
    }
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        continue;
      }
      return;
    }
    request.append(buffer.data(), static_cast<std::size_t>(count));
  }
  int status = 404;
  std::string_view reason = "Not Found";
  std::string body = "{}";
  std::string extra_headers;
  const auto line_end = request.find("\r\n");
  const auto line = request.substr(0, line_end);
  const auto first_space = line.find(' ');
  const auto last_space = line.rfind(' ');
  if (first_space != std::string::npos && last_space > first_space) {
    const auto method = line.substr(0, first_space);
    const auto path = line.substr(first_space + 1, last_space - first_space - 1);
    const bool live = path == "/health/live";
    const bool ready = path == "/health/ready";
    if (live || ready) {
      if (method != "GET") {
        status = 405;
        reason = "Method Not Allowed";
        extra_headers = "Allow: GET\r\n";
      } else {
        // The readiness port supplies business truth. P02 applications always return false.
        const bool healthy = live || application.readiness().ready;
        status = healthy ? 200 : 503;
        reason = healthy ? "OK" : "Service Unavailable";
        body = healthy ? "{\"state\":\"HEALTHY\"}" : "{\"state\":\"UNHEALTHY\"}";
      }
    } else if (application.service() == target::Service::Control &&
               (path.starts_with("/api/v1/") || path.starts_with("/ws/"))) {
      status = 503;
      reason = "Service Unavailable";
      body = "{\"code\":\"CAPABILITY_NOT_SUPPORTED\",\"message\":\"Business implementation "
             "unavailable\",\"request_id\":\"p02-process\"}";
    }
  }
  const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                               "\r\nContent-Type: application/json\r\nConnection: close\r\n" +
                               extra_headers + "Content-Length: " + std::to_string(body.size()) +
                               "\r\n\r\n" + body;
  std::size_t offset = 0;
  while (offset < response.size() && std::chrono::steady_clock::now() < deadline) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    const auto count =
        send(client.get(), response.data() + offset, response.size() - offset, flags);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
    } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      pollfd writable{client.get(), POLLOUT, 0};
      if (poll(&writable, 1, 20) < 0) {
        return;
      }
    } else {
      return;
    }
  }
}

} // namespace

int run_service(const target::ServiceReadinessPort& application, int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Usage: kiarabot-" << service_name(application.service()) << " --config PATH\n";
    return 0;
  }
  if (argc != 3 || std::string_view(argv[1]) != "--config" || std::string_view(argv[2]).empty()) {
    std::cerr << "operation=start outcome=rejected reason=invalid_arguments\n";
    return 2;
  }
  try {
    const auto loaded = load_service_config(argv[2], application.service());
    if (const auto* failure = std::get_if<ConfigFailure>(&loaded)) {
      std::cerr << "operation=start outcome=rejected reason=" << failure->reason << '\n';
      return failure->exit_code;
    }
    const auto& config = std::get<ProcessConfig>(loaded);
    // Created before gRPC workers; they inherit blocked stop signals. No async handler callbacks.
    const StopSignals signals;
    const Socket listener(socket(AF_INET, SOCK_STREAM, 0));
    const int probe_port =
        listener.get() < 0 ? 0 : listen_probe(listener.get(), config.probe_listen);
    if (probe_port == 0) {
      std::cerr << "operation=start outcome=failed reason=probe_bind_failed\n";
      return 1;
    }
    ServiceRpc rpc;
    if (application.service() != target::Service::Control && !rpc.start(config)) {
      std::cerr << "operation=start outcome=failed reason=grpc_bind_failed\n";
      return 1;
    }
    std::cout << "service=" << service_name(application.service()) << " probe_port=" << probe_port
              << " grpc_port=" << rpc.port() << " ready=false\n"
              << std::flush;
    while (!signals.requested()) {
      pollfd readable{listener.get(), POLLIN, 0};
      const auto count = poll(&readable, 1, 100);
      if (count < 0 && errno != EINTR) {
        throw std::runtime_error("probe_poll_failed");
      }
      if (count > 0 && (readable.revents & POLLIN) != 0) {
        serve_probe(listener.get(), application);
      }
    }
    return 0;
  } catch (const std::exception&) {
    std::cerr << "operation=start outcome=failed reason=process_failure\n";
    return 1;
  }
}

} // namespace kiarabot::core::platform
