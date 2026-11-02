#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/scheduler_service.hpp"
#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {

struct SchedulerIdentityClientOptions {
  // Identity owner gRPC endpoint, validated by the Scheduler service configuration.
  std::string endpoint;
  // Production composition reuses the Scheduler service TLS material as outbound mTLS identity;
  // tests may inject insecure credentials for a loopback fake Identity.
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  std::chrono::milliseconds budget{500};
  std::chrono::milliseconds maximum_budget{1000};
};

// P10b Scheduler authorizer. Verifies the actual inbound mTLS leaf against the delegation proof,
// derives the task owner instance from the delegated subject and the installation from the
// delegation scope bindings, then asks Identity.CheckAuthorization for the current grant/version.
// It is fail-closed: malformed/foreign-audience proofs, missing installation binding, transport or
// Identity failures never become an allow. Missing required claim fields stop authorization and
// are reported rather than guessed.
class SchedulerIdentityClient {
public:
  static target::Outcome<std::unique_ptr<SchedulerIdentityClient>>
  create(SchedulerIdentityClientOptions options);
  ~SchedulerIdentityClient();
  SchedulerIdentityClient(const SchedulerIdentityClient&) = delete;
  SchedulerIdentityClient& operator=(const SchedulerIdentityClient&) = delete;

  bool available() const noexcept;

  // Transport-independent verification used by the gRPC authorizer and by tests. `method` is the
  // full gRPC method used for Identity method-policy resolution; `scope` is the optional request
  // ResourceScope (never caller-constructed bindings).
  target::Outcome<target::SchedulerCommandContext>
  verify(std::string_view proof, const target::Sha256Digest& leaf_fingerprint,
         const v1::RequestContext& request, std::string_view method,
         const v1::ResourceScope* scope);

  // Production peer authorizer: extracts the real inbound certificate leaf before verify().
  target::Outcome<target::SchedulerCommandContext> authenticate(grpc::ServerContext& transport,
                                                                const v1::RequestContext& request,
                                                                std::string_view method,
                                                                const v1::ResourceScope* scope);

private:
  explicit SchedulerIdentityClient(SchedulerIdentityClientOptions options);
  SchedulerIdentityClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::IdentityService::Stub> stub_;
};

} // namespace kiarabot::core::platform
