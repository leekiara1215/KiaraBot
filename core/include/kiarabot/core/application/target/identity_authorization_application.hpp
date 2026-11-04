#pragma once

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/ports/target/authorization_method_policy.hpp"

namespace kiarabot::core::target {
struct CheckAuthorizationRequest {
  IdentityRequestContext context;
  VerifiedPeer recipient;
  SecretView subject_proof;
  std::string full_method;
  std::vector<ResourceRef> scope;
  std::optional<ResourceVersion> known_authorization_version;
};
struct CheckAuthorizationReply {
  bool allowed;
  ResourceVersion authorization_version;
  TimePoint checked_at;
  std::optional<Failure> denial;
};
struct CheckHostGateAuthorizationRequest {
  IdentityRequestContext context;
  VerifiedPeer recipient;
  ResourceRef host;
  Sha256Digest host_certificate;
};
struct CheckHostGateAuthorizationReply {
  CheckAuthorizationReply decision;
  ResourceVersion host_credential_version;
  ResourceVersion host_principal_version;
};
// Online current authority only. The business owner must separately bind proof to its actual
// inbound TLS direct peer with ordinary DelegationCryptoPort::verify before business execution.
// Only Runtime/Host/Scheduler/Model may check their own methods/audience with explicit exact
// IdentityAuthorizationCheck at their own Principal. No cross-recipient Control administrator path.
// No mutation/commit/cache. All crypto and trusted policy resolution occur outside UoWs.
class IdentityAuthorizationApplication {
public:
  virtual ~IdentityAuthorizationApplication() = default;
  virtual Outcome<CheckAuthorizationReply>
  check_authorization(const CheckAuthorizationRequest&) = 0;
  // Only registered Runtime, for its fixed SetHostAdmissionGate method. Fingerprint is actual
  // inbound Host TLS evidence; stored Host binding plus exact current HostExecute are required.
  virtual Outcome<CheckHostGateAuthorizationReply>
  check_host_gate_authorization(const CheckHostGateAuthorizationRequest&) = 0;
};
// Borrowed dependencies outlive all calls; each concurrent call owns separate readonly UoWs.
Outcome<std::unique_ptr<IdentityAuthorizationApplication>>
make_identity_authorization_application(IdentitySecurityUnitOfWorkFactory& transactions,
                                        DelegationCryptoPort& crypto, SecurityClockPort& clock,
                                        AuthorizationMethodPolicyPort& methods);
} // namespace kiarabot::core::target
