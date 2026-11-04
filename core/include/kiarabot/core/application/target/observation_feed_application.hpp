#pragma once

#include <functional>
#include <memory>

#include "kiarabot/core/application/target/context.hpp"
#include "kiarabot/core/ports/target/observation_feed_ports.hpp"
#include "kiarabot/core/ports/target/runtime_authorization.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

namespace kiarabot::core::target {

struct ObservationFeedCommand {
  CommandContext context{};
  ObservationFeedQuery query;
};

struct ObservationFeedDependencies {
  Service owner = Service::Runtime;
  ObservationFamily family = ObservationFamily::Logs;
  // Adapts an owner-specific factory without sharing databases or retaining a UoW.
  std::function<Outcome<std::unique_ptr<UnitOfWork>>()> begin;
  // Reads the feed journal on the already-open read-only UoW. Supplied by the infrastructure
  // adapter that implements ObservationFeedPort on the owner transaction.
  std::function<Outcome<ObservationFeedPage>(UnitOfWork&, const ObservationFeedQuery&)> query;
  // Reuses the existing pure exact-scope/current-decision vocabulary. The method and audience
  // identify the owner; the adapter must call Identity online.
  RuntimeAuthorizationPort& authorization;
  std::function<TimePoint()> now;
};

// Bounded read-only query over one (owner, family) journal. Current online authorization happens
// before the owner-local read-only UoW is opened; the returned page is revalidated before it can
// reach the wire.
class ObservationFeedApplication {
public:
  virtual ~ObservationFeedApplication() = default;
  virtual Outcome<ObservationFeedPage> query(const ObservationFeedCommand& command) = 0;
};

Outcome<std::unique_ptr<ObservationFeedApplication>>
make_observation_feed_application(ObservationFeedDependencies dependencies);

// Frozen owner/family binding: method name, permission and exact request scope kinds. A cursor is
// valid only for the same (owner, family) durable dataset.
std::string_view observation_feed_method(Service owner, ObservationFamily family);
std::string_view observation_feed_permission_text(Service owner, ObservationFamily family);
bool observation_feed_owner_family(Service owner, ObservationFamily family);
// Exact-scope shape only, canonicalized by (kind,id). Log feeds require exactly the authorized
// RESOURCE_CATALOG/control; task feeds exact RESOURCE_TASK|RESOURCE_INSTANCE; message feeds exact
// RESOURCE_ACCOUNT.
bool observation_feed_scope(Service owner, ObservationFamily family,
                            std::vector<ResourceRef>& scope);

} // namespace kiarabot::core::target
