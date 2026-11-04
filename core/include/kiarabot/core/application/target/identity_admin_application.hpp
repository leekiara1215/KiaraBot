#pragma once

#include "kiarabot/core/application/target/identity_security_application.hpp"

namespace kiarabot::core::target {

// Trusted offline enrollment of one non-Control platform service peer
// (Runtime/Scheduler/Model/Host). The CA/SAN/chain verification happens in the offline CLI adapter
// before this struct is built; the pure application still re-validates kind, binding, service,
// certificate shape and assignment uniqueness so a malformed composition cannot persist an
// unauthenticated peer.
struct PlatformPeerEnrollment {
  Service service;
  SubjectId principal;
  CredentialId credential;
  CertificateBinding certificate;
  std::vector<ExactAssignment> assignments;
  // Host only: explicit business resource binding, independent from principal/credential IDs.
  std::optional<ResourceId> host{};
};

// Only trusted OFFLINE composition can construct this request. Password derivation and enrolled
// Control certificate CA/SAN validation have completed before entry. Never bind this to an RPC.
struct OfflineIdentityBootstrap {
  IdentityRequestContext context;
  ResourceVersion expected_marker;
  SubjectId administrator;
  CredentialId password_credential;
  std::string normalized_login;
  PasswordVerifier password;
  TimePoint password_expires_at;
  SubjectId control;
  CredentialId control_credential;
  CertificateBinding control_certificate;
  std::vector<ExactAssignment> administrator_assignments;
  std::vector<ExactAssignment> control_assignments;
  // Optional platform peers enrolled in the same once-only bootstrap transaction. Empty for the
  // historical administrator+Control-only deployments.
  std::vector<PlatformPeerEnrollment> platform_peers;
};
struct IdentityBootstrapReply {
  AdministratorBootstrap marker;
  TimePoint checked_at;
};
struct PeerRegistrationRequest {
  // Extracted by mature TLS adapter from actual transport certificate, never from request JSON.
  Sha256Digest leaf_fingerprint;
  TimePoint deadline;
};
class IdentityAdminApplication {
public:
  IdentityAdminApplication(IdentitySecurityUnitOfWorkFactory& transactions,
                           PeerCertificatePort& certificates);
  // Same physical transaction for all initial records and the durable once-only marker.
  Outcome<IdentityBootstrapReply> bootstrap_offline(const OfflineIdentityBootstrap& request);
  // Registration/current state only; this is NOT chain/proof-of-possession verification.
  Outcome<RegisteredPeer> registered_peer(const PeerRegistrationRequest& request);
  // Real DER transport evidence -> registration -> crypto without UoW -> current registration.
  Outcome<VerifiedPeer> authenticate_peer(const PeerRegistrationRequest& request,
                                          std::span<const std::byte> presented_chain);

private:
  IdentitySecurityUnitOfWorkFactory& transactions_;
  PeerCertificatePort& certificates_;
};

} // namespace kiarabot::core::target
