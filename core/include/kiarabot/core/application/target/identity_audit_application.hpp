#pragma once

#include <memory>

#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/domain/target/audit_query.hpp"

namespace kiarabot::core::target {

// P12e Identity-authoritative read-only audit/log projection. The caller is the verified Control
// platform peer; the application rechecks its current registration/credential and the explicit
// audit.read/log.read assignment on the Control catalog resource. No cross-owner or cross-database
// reads and no mutation.
struct IdentityAuditCommand {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  ResourceRef scope;
  AuditQuery query;
};
class IdentityAuditApplication {
public:
  virtual ~IdentityAuditApplication() = default;
  virtual Outcome<AuditPage> list_audit(const IdentityAuditCommand& command) = 0;
  virtual Outcome<LogPage> list_logs(const IdentityAuditCommand& command) = 0;
};
Outcome<std::unique_ptr<IdentityAuditApplication>>
make_identity_audit_application(IdentitySecurityUnitOfWorkFactory& transactions);

} // namespace kiarabot::core::target
