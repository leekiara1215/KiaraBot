#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kiarabot/control/target/websocket_types.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/v1/policy.pb.h"

namespace kiarabot::control::target {

class SecretDeliveryApplication;
class BootstrapRecipientsPort;

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
  IdentityAuthClient(const std::string& endpoint, const std::string& runtime_endpoint,
                     const std::string& scheduler_endpoint, const std::string& model_endpoint,
                     const std::string& host_endpoint, const core::platform::TlsMaterial& tls,
                     SecretDeliveryApplication* secret_delivery = nullptr,
                     BootstrapRecipientsPort* bootstrap_recipients = nullptr);
  ~IdentityAuthClient();
  IdentityAuthClient(const IdentityAuthClient&) = delete;
  IdentityAuthClient& operator=(const IdentityAuthClient&) = delete;
  AuthHttpResponse handle(const AuthHttpRequest& request);
  // Same live Identity channel, cancellation registry and projection validation as HTTP.
  // The caller owns a separate bounded WS worker budget; these calls cache no grant.
  WebSocketResult<WebSocketIdentity> validate_stream(std::string_view access_token,
                                                     WebSocketDeadline deadline);
  WebSocketResult<std::string> delegate_stream(std::string_view owner, v1::Permission permission,
                                               const v1::ResourceScope& scope,
                                               std::string_view access_token,
                                               WebSocketDeadline deadline);

  // Read-only readiness observation of the Control->Identity channel (no RPC, bounded).
  bool identity_ready() const;
  void cancel() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::control::target
