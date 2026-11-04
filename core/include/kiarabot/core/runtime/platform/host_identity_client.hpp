#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/identity.grpc.pb.h>

#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/ports/target/host_maintenance.hpp"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {

struct HostIdentityClientOptions {
  // Identity owner gRPC endpoint from the Host service configuration; absent keeps owner
  // management calls fail-closed (the composition never fabricates an authorization).
  std::string endpoint;
  // Production composition reuses the Host deployment TLS material as outbound mTLS identity.
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  std::chrono::milliseconds budget{1500};
  std::chrono::milliseconds maximum_budget{3000};
};

// HOST-04 Host authorizer. The transport boundary extracts the actual inbound Control leaf,
// structurally decodes the service-verified delegation envelope (audience=host) and binds it to
// that leaf; every business call is then re-authorized online through Identity.CheckAuthorization
// as the Host's own registered platform peer. Failures are stable and fail-closed.
class HostIdentityClient {
public:
  static target::Outcome<std::unique_ptr<HostIdentityClient>>
  create(HostIdentityClientOptions options);
  ~HostIdentityClient();
  HostIdentityClient(const HostIdentityClient&) = delete;
  HostIdentityClient& operator=(const HostIdentityClient&) = delete;

  bool available() const noexcept;

  // Real inbound mTLS leaf + request delegation -> current Host consumer context. No online
  // business authorization happens here; HostManagementRpc's application port does that.
  target::Outcome<target::AuthenticationContext> authenticate(grpc::ServerContext& transport,
                                                              const v1::RequestContext& request);

  // Online current-authority check for one exact Host resource. Returns the observed
  // authorization version only when Identity allows; a denial has no reusable proof.
  target::Outcome<target::ResourceVersion> check(const target::AuthenticationContext& context,
                                                 target::Permission permission,
                                                 const target::ResourceRef& resource,
                                                 target::TimePoint deadline);

private:
  explicit HostIdentityClient(HostIdentityClientOptions options);
  HostIdentityClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<kiarabot::v1::IdentityService::Stub> stub_;
};

// Adapter for the HOST-02 application authorization port; no business rules of its own.
class HostMaintenanceIdentityAuthorizer final : public target::HostMaintenanceAuthorizationPort {
public:
  explicit HostMaintenanceIdentityAuthorizer(HostIdentityClient& client) : client_(client) {}
  target::Outcome<target::ResourceVersion> check(const target::AuthenticationContext& context,
                                                 target::Permission permission,
                                                 const target::ResourceRef& resource,
                                                 target::TimePoint deadline) override {
    return client_.check(context, permission, resource, deadline);
  }

private:
  HostIdentityClient& client_;
};

} // namespace kiarabot::core::platform
