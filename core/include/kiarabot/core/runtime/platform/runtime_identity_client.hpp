#pragma once

#include <memory>
#include <string>

#include <grpcpp/channel.h>
#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/runtime_host_application.hpp"
#include "kiarabot/core/domain/target/context.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/v1/identity.grpc.pb.h"

namespace kiarabot::core::platform {

struct RuntimeIdentityClientOptions {
  // Identity owner gRPC endpoint, host:port; validated by the service configuration.
  std::string endpoint;
  // Production composition passes mTLS channel credentials built from the Runtime service TLS
  // material; tests may pass insecure credentials for a loopback fake Identity server.
  std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
  // Total per-check budget for the hot-path CheckAuthorization; production is 500ms/1s.
  std::chrono::milliseconds budget{500};
  std::chrono::milliseconds maximum_budget{1000};
  // ExchangeDelegation performs two online authorization passes plus signing; it must not reuse the
  // 500ms hot-path budget or the second pass can exceed the request deadline (Identity then returns
  // InvalidArgument before audit, even though the wire/envelope/certificate are valid). Keep a
  // separate, bounded delegation budget.
  std::chrono::milliseconds delegation_budget{2000};
  std::chrono::milliseconds delegation_maximum_budget{5000};
  // Wire RequestContext deadline of ExchangeDelegation. Identity also bounds the issued child
  // delegation lifetime by it, so it must cover the longest business call (Model stream max 30s)
  // while the gRPC network deadline above stays short. Bounded by the RPC max_deadline_ms=30000.
  std::chrono::milliseconds delegation_lifetime{30000};
};

// Decoded, NOT cryptographically verified delegation claims. Runtime treats these as request
// metadata only: the authoritative signature/audience/origin/current-authority decision is
// Identity.CheckAuthorization, called for every command before any effect. Structural validation
// here rejects malformed/expired/foreign-audience proofs and a client-certificate binding that does
// not match the actual inbound TLS leaf, so self-reported identity never authorizes anything.
// audience defaults to Runtime (P05/P06); the Scheduler authorizer passes Scheduler so the same
// structural/binding validation applies to task delegation proofs.
target::Outcome<target::AuthenticationContext>
decode_delegation_claims(std::string_view proof, const target::Sha256Digest& leaf_fingerprint,
                         target::TimePoint now,
                         target::Service audience = target::Service::Runtime);

// Real Identity owner client for Runtime. Used by the production composition as both the
// RuntimeAuthorizationPort (online current authority) and the PeerAuthorizer (verified delegation
// metadata + actual inbound certificate binding). Failures are stable, fail-closed and never
// become an allow; availability reflects the transport channel only.
struct ExchangedDelegation {
  std::string proof;
  target::TimePoint expires_at;
};
class RuntimeIdentityClient final : public target::RuntimeAuthorizationPort {
public:
  static target::Outcome<std::unique_ptr<RuntimeIdentityClient>>
  create(RuntimeIdentityClientOptions options);
  ~RuntimeIdentityClient() override;
  RuntimeIdentityClient(const RuntimeIdentityClient&) = delete;
  RuntimeIdentityClient& operator=(const RuntimeIdentityClient&) = delete;

  bool available() const noexcept override;
  target::Outcome<target::RuntimeAuthorizationDecision>
  check(const target::RuntimeAuthorizationRequest& request) override;
  // PeerAuthorizer implementation for the Runtime gRPC transport. Extracts the real inbound
  // certificate, decodes the delegation, and binds it to that certificate; online Identity checks
  // still decide every business permission in check().
  target::Outcome<target::AuthenticationContext> authenticate(grpc::ServerContext& transport,
                                                              const v1::RequestContext& request);
  // P05b-5: Runtime uses its real mTLS leaf as the Exchange caller certificate and asks Identity
  // to turn a short-lived instance-parent proof (subject=plugin instance, audience=runtime, hops1)
  // into an audience=scheduler hops2 child proof. The returned proof is opaque; the caller keeps
  // it in memory only and the Scheduler still verifies/authorizes it online.
  target::Outcome<ExchangedDelegation>
  exchange_delegation(std::string_view parent_proof, target::Service audience,
                      const std::vector<target::ResourceRef>& scope,
                      const std::vector<target::Permission>& permissions,
                      const target::Sha256Digest& caller_certificate);
  // HOST-03: Runtime uses its own actual mTLS leaf as the caller of the narrow v1.1 Host gate
  // check. Only the exact RESOURCE_HOST and the real Host leaf fingerprint are sent; the returned
  // version is a current decision, never a reusable ticket, and denial is a stable failure.
  target::Outcome<target::ResourceVersion>
  check_host_gate(const target::ResourceRef& host, const target::Sha256Digest& host_certificate,
                  target::TimePoint deadline);

private:
  explicit RuntimeIdentityClient(RuntimeIdentityClientOptions options);
  RuntimeIdentityClientOptions options_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::IdentityService::Stub> stub_;
};

// Adapter from the Runtime Host gate application port to the real Identity client. No local
// authorization, cache or default-allow path: every call crosses Identity.
class RuntimeHostGateAuthorization final : public target::HostGateAuthorizationPort {
public:
  explicit RuntimeHostGateAuthorization(RuntimeIdentityClient& client) : client_(client) {}
  target::Outcome<target::ResourceVersion> check(const target::ResourceRef& host,
                                                 const target::Sha256Digest& host_certificate,
                                                 target::TimePoint deadline) override {
    return client_.check_host_gate(host, host_certificate, deadline);
  }

private:
  RuntimeIdentityClient& client_;
};

} // namespace kiarabot::core::platform
