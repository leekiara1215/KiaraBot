#pragma once

#include <memory>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/domain/target/audit_query.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

// P12e Runtime-authoritative read-only audit/log projection for the Control aggregation layer.
// Control must present a current online delegation for the exact catalog scope; every decision is
// rechecked against Identity, and only this Runtime database's audit facts are returned.
struct RuntimeAuditCommand {
  CommandContext context;
  ResourceRef scope;
  AuditQuery query;
};
class RuntimeAuditApplication {
public:
  virtual ~RuntimeAuditApplication() = default;
  virtual Outcome<AuditPage> list_audit(const RuntimeAuditCommand& command) = 0;
  virtual Outcome<LogPage> list_logs(const RuntimeAuditCommand& command) = 0;
};
struct RuntimeAuditDependencies {
  RuntimeUnitOfWorkFactory& transactions;
  RuntimeAuthorizationPort& authorization;
};
Outcome<std::unique_ptr<RuntimeAuditApplication>>
make_runtime_audit_application(RuntimeAuditDependencies dependencies);

} // namespace kiarabot::core::target
