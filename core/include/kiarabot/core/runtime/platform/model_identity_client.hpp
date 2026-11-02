#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/ports/target/model_ports.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform {

struct ModelIdentityClientOptions {
  std::string endpoint;
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  std::chrono::milliseconds budget{500};
  std::chrono::milliseconds maximum_budget{1000};
};

// Model caller verification. The transport boundary structurally decodes the delegation proof
// carried in RequestContext.delegation, binds its client_certificate claim to the real inbound TLS
// leaf and derives the plugin owner only from a validated Runtime second hop. The signature,
// audience, origin, revocation and current permission decision remain Identity's authoritative
// CheckAuthorization call per operation; structural checks never authorize by themselves.
//
// The client implements BOTH ports: `authenticate` is the RPC PeerAuthenticator and `authorize`
// the ModelAuthorizationPort. Failures are stable and fail closed; `available()` is readiness only.
class ModelIdentityClient final : public target::ModelAuthorizationPort {
public:
  static target::Outcome<std::unique_ptr<ModelIdentityClient>>
  create(ModelIdentityClientOptions options);
  ~ModelIdentityClient() override;
  ModelIdentityClient(const ModelIdentityClient&) = delete;
  ModelIdentityClient& operator=(const ModelIdentityClient&) = delete;

  bool available() const noexcept;
  target::Outcome<target::ModelCaller> authenticate(grpc::ServerContext& transport,
                                                    const v1::RequestContext& request);
  target::Outcome<target::ModelAuthorizationDecision>
  authorize(const target::ModelCaller& caller, target::Permission permission,
            const target::ResourceRef& resource, std::chrono::milliseconds budget) override;

private:
  explicit ModelIdentityClient(ModelIdentityClientOptions options);
  ModelIdentityClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::IdentityService::Stub> stub_;
};

// Structural decoder exposed for boundary tests. It does NOT verify the signature and must only be
// used together with Identity.CheckAuthorization, exactly like the runtime variant.
target::Outcome<target::ModelCaller>
decode_model_delegation(std::string_view proof, const target::Sha256Digest& leaf_fingerprint,
                        target::TimePoint now);

} // namespace kiarabot::core::platform
