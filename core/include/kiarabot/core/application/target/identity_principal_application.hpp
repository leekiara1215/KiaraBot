#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/domain/target/identity_registration_snapshot.hpp"
#include "kiarabot/core/ports/target/identity_crypto.hpp"
#include "kiarabot/core/ports/target/identity_principal_fingerprint.hpp"
#include "kiarabot/core/ports/target/identity_repository.hpp"

namespace kiarabot::core::target {

// Public application boundary for the five Identity principal/credential management RPCs.
// No protobuf/HTTP/JWT/SQL object enters. Peer identity is provided by the transport boundary as
// real TLS evidence; this application re-reads current registration and authorization.
struct RegisterPrincipalCommand {
  IdentityRequestContext context;
  VerifiedPeer actor;
  PrincipalKind kind;
  ResourceRef binding;
  std::vector<Service> allowed_services;
};
struct RotateCredentialCommand {
  IdentityRequestContext context;
  VerifiedPeer actor;
  CredentialId credential;
  // Always required for a management mutation; the secret is server-generated and never input.
  ResourceVersion expected_version;
};
struct RevokeCredentialCommand {
  IdentityRequestContext context;
  VerifiedPeer actor;
  CredentialId credential;
  ResourceVersion expected_version;
};
struct GetPrincipalStatusQuery {
  IdentityRequestContext context;
  VerifiedPeer actor;
  SubjectId principal;
};
struct GetCredentialStatusQuery {
  IdentityRequestContext context;
  VerifiedPeer actor;
  CredentialId credential;
};
// One-time workload bootstrap secret. The receipt identifier is a keyed correlation value, never
// the secret or its verifier; replaying the same request returns the same delivery.
struct CredentialDeliveryReply {
  CredentialVersionRef credential;
  OwnedSecret secret;
  std::string delivery_receipt_id;
  TimePoint expires_at;
  TimePoint checked_at;
};
struct RevokeCredentialReply {
  ResourceVersion version;
  TimePoint checked_at;
};
struct PrincipalStatusReply {
  Principal principal;
  TimePoint checked_at;
};
using CredentialStatusReply = CredentialStatusSnapshot;

struct IdentityPrincipalOptions {
  // Trusted composition/configuration only. Empty keys/capacities/lifetimes reject construction;
  // no unlimited budget, weak random source or fake readiness is substituted.
  CryptoKeyRef token_key;
  CryptoKeyRef request_commitment_key;
  CryptoKeyRef receipt_encryption_key;
  std::chrono::nanoseconds bootstrap_credential_lifetime{};
  // Trusted password lifetime for human password credential rotation; never a request field.
  std::chrono::nanoseconds password_credential_lifetime{};
  SecretReceiptCapacity secret_receipts{0};
  std::size_t maximum_allowed_services = 0;
};
struct IdentityPrincipalDependencies {
  // Borrowed for the entire application lifetime; one physical Identity transaction per UoW.
  IdentitySecurityUnitOfWorkFactory& transactions;
  TokenCryptoPort& tokens;
  PasswordCryptoPort& passwords;
  SecretReceiptCryptoPort& receipts;
  RandomPort& random;
  IdentityPrincipalFingerprintPort& fingerprints;
};
// Methods may run concurrently; each isolates its own UoWs. Registration and rotation are
// idempotent on their recorded identity: replay re-checks current authorization first and returns
// the original success snapshot, never re-executes a side effect or re-issues a secret.
class IdentityPrincipalApplication {
public:
  virtual ~IdentityPrincipalApplication() = default;
  virtual Outcome<RegisteredPrincipalSnapshot>
  register_principal(const RegisterPrincipalCommand& command) = 0;
  // Rotates an unissued Pending or Active credential into the next Active version:
  // WorkloadBootstrap issues a new opaque bootstrap token; Password re-derives a new password
  // verifier from a server-generated one-time password returned in the delivery. Certificate
  // binding rotation is not exposed by this RPC (CapabilityNotSupported; enrollment owns it).
  // On the first successful rotation of a currently Disabled principal, the same transaction
  // activates the principal; later rotations keep an Active principal unchanged. Revoked or
  // non-live principals are Conflict with ReadCurrent. Crypto preparation runs outside every UoW.
  virtual Outcome<CredentialDeliveryReply>
  rotate_credential(const RotateCredentialCommand& command) = 0;
  virtual Outcome<RevokeCredentialReply>
  revoke_credential(const RevokeCredentialCommand& command) = 0;
  virtual Outcome<PrincipalStatusReply>
  get_principal_status(const GetPrincipalStatusQuery& query) = 0;
  virtual Outcome<CredentialStatusReply>
  get_credential_status(const GetCredentialStatusQuery& query) = 0;
};
Outcome<std::unique_ptr<IdentityPrincipalApplication>>
make_identity_principal_application(IdentityPrincipalDependencies dependencies,
                                    IdentityPrincipalOptions options);

} // namespace kiarabot::core::target
