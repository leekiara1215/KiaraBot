#pragma once

#include <memory>
#include <optional>
#include <string>

#include "kiarabot/control/target/identity_auth_client.hpp"
#include "kiarabot/core/runtime/platform/service_config.hpp"

namespace kiarabot::control::target {

// Real P12c owner adapters for the target Control process: Identity.IssueDelegation with the
// caller's access token as subject proof, then the exact owner RPC over mTLS. The gateway itself
// stays transport-neutral (unit tested); this class only supplies live ports.
class OwnerClients {
public:
  OwnerClients(const std::string& identity_endpoint, const std::string& runtime_endpoint,
               const std::string& scheduler_endpoint, const std::string& model_endpoint,
               const std::string& host_endpoint, const core::platform::TlsMaterial& tls);
  ~OwnerClients();
  OwnerClients(const OwnerClients&) = delete;
  OwnerClients& operator=(const OwnerClients&) = delete;
  // nullopt means the path is not an owner route handled here.
  std::optional<AuthHttpResponse> handle(const AuthHttpRequest& request);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace kiarabot::control::target
