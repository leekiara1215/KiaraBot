#pragma once

#include <functional>

#include "kiarabot/core/application/target/identity_audit_application.hpp"
#include "kiarabot/core/application/target/observation_feed_application.hpp"
#include "kiarabot/core/application/target/identity_security_application.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

struct IdentityFeedCommand {
  IdentityRequestContext context;
  VerifiedPeer control_peer;
  ObservationFeedQuery query;
};

// Identity-owned log feed. The registered Control platform peer is re-verified against its current
// credential and exact log.read assignment on every page; the owner-local read-only UoW is opened
// only after that decision. This is a redacted metadata feed, not the audit log.
class IdentityLogFeedApplication {
public:
  virtual ~IdentityLogFeedApplication() = default;
  virtual Outcome<ObservationFeedPage> list_log_changes(const IdentityFeedCommand& command) = 0;
};

// The query callback reads the journal on the already-open Identity transaction; the composition
// supplies the concrete UoW adapter so this application stays independent from storage types.
Outcome<std::unique_ptr<IdentityLogFeedApplication>> make_identity_log_feed_application(
    IdentitySecurityUnitOfWorkFactory& transactions,
    std::function<Outcome<ObservationFeedPage>(UnitOfWork&, const ObservationFeedQuery&)> query);

} // namespace kiarabot::core::target
