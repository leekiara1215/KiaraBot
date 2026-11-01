#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kiarabot/core/runtime/platform/service_config.hpp"

namespace kiarabot::control::target {

// Transport-only values. They do not enter the pure Control application or Core ports.
struct AuthHttpRequest {
  std::string method;
  std::string target;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
};
struct AuthHttpResponse {
  unsigned int status;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

// Thread-safe mTLS boundary. No session cache, local authorization, retries or business storage.
// Each HTTP operation has a total two-second upstream budget; cancel prevents new calls.
class IdentityAuthClient {
public:
  IdentityAuthClient(const std::string& endpoint, const core::platform::TlsMaterial& tls);
  ~IdentityAuthClient();
  IdentityAuthClient(const IdentityAuthClient&) = delete;
  IdentityAuthClient& operator=(const IdentityAuthClient&) = delete;
  AuthHttpResponse handle(const AuthHttpRequest& request);
  void cancel() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::control::target
