#pragma once

#include "kiarabot/core/application/target/identity_security_application.hpp"

namespace kiarabot::core::target {

struct IssueIdentityDelegationRequest {
  AccessSessionRequest source;
  Service audience;
  // Exact tuples: adapters expand the validated wire permission/scope product once.
  std::vector<PermissionBinding> permissions;
  // B1 controlled instance-parent issuance. Empty keeps the legacy admin-subject behavior; when
  // set, subject is resolved from the unique plugin principal bound to RESOURCE_INSTANCE/{id}.
  std::optional<ResourceId> instance_id;
};
struct IdentityDelegationReply {
  OwnedSecret proof;
  DelegationClaims claims;
  TimePoint checked_at;
};
struct IdentityDelegationOptions {
  std::string issuer;
  CryptoKeyRef signing_key;
};
// Online authority + short-lived signing only. No ordinary receipt contains a reusable JWT.
// Each request (including retries) checks current source and exact authorization before signing
// and after crypto, without holding a UoW across application/crypto port calls.
class IdentityDelegationApplication {
public:
  IdentityDelegationApplication(IdentitySecurityApplication& sessions,
                                IdentitySecurityUnitOfWorkFactory& transactions,
                                DelegationCryptoPort& delegations, RandomPort& random,
                                SecurityClockPort& clock, IdentityDelegationOptions options);
  Outcome<IdentityDelegationReply> issue_delegation(const IssueIdentityDelegationRequest& request);

private:
  IdentitySecurityApplication& sessions_;
  IdentitySecurityUnitOfWorkFactory& transactions_;
  DelegationCryptoPort& delegations_;
  RandomPort& random_;
  SecurityClockPort& clock_;
  IdentityDelegationOptions options_;
};

} // namespace kiarabot::core::target
