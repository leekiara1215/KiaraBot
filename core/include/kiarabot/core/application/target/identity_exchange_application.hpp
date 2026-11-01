#pragma once

#include "kiarabot/core/application/target/identity_delegation_application.hpp"

namespace kiarabot::core::target {
struct ExchangeIdentityDelegationRequest {
  IdentityRequestContext context;
  VerifiedPeer recipient;
  SecretView parent_proof;
  Service audience;
  std::vector<PermissionBinding> permissions;
};
// Converts a first-hop proof addressed to the actual Runtime TLS recipient into a narrowed
// second-hop proof for Model/Scheduler. Both delegating services need an explicit own-Principal
// IdentityDelegationIssue capability; the original subject independently needs each business grant.
class IdentityExchangeApplication {
public:
  IdentityExchangeApplication(IdentitySecurityUnitOfWorkFactory& transactions,
                              DelegationCryptoPort& delegations, RandomPort& random,
                              SecurityClockPort& clock, IdentityDelegationOptions options);
  // No JWT parsing, durable receipt, database write or intent. Crypto runs outside ALL UoWs;
  // current peers, origin and grants are checked again after signing before returning any proof.
  Outcome<IdentityDelegationReply>
  exchange_delegation(const ExchangeIdentityDelegationRequest& request);

private:
  IdentitySecurityUnitOfWorkFactory& transactions_;
  DelegationCryptoPort& delegations_;
  RandomPort& random_;
  SecurityClockPort& clock_;
  IdentityDelegationOptions options_;
};
} // namespace kiarabot::core::target
