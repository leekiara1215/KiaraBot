#pragma once

#include "kiarabot/core/ports/target/service_readiness.hpp"

namespace kiarabot::core::target {

// P05 Runtime readiness inputs are explicit observations with provenance, not ad-hoc booleans:
// the composition root fills them from real probes (assembled application, Runtime private
// database/schema writer usable, injected Identity current-authorization chain verified). A missing
// observation is Unverified, never "available"; a test fake may feed the true path of the judgement
// in unit tests but must never make the composed process report ready.
struct RuntimeReadinessObservation {
  DependencyState business_assembled = DependencyState::Unverified;
  DependencyState database_schema = DependencyState::Unverified;
  DependencyState identity_authorization = DependencyState::Unverified;
};

// Composition helper: build an observation from the three real probe results only.
inline RuntimeReadinessObservation observed_runtime_ready(bool business_available,
                                                          bool database_available,
                                                          bool identity_authorization_available) {
  return {business_available ? DependencyState::Available : DependencyState::NotImplemented,
          database_available ? DependencyState::Available : DependencyState::Unavailable,
          identity_authorization_available ? DependencyState::Available
                                           : DependencyState::Unavailable};
}

inline ServiceReadiness evaluate_runtime_readiness(const RuntimeReadinessObservation& observation) {
  ServiceReadiness result;
  result.dependencies = {{Dependency::BusinessImplementation, observation.business_assembled},
                         {Dependency::DatabaseSchema, observation.database_schema},
                         {Dependency::Identity, observation.identity_authorization}};
  result.ready = observation.business_assembled == DependencyState::Available &&
                 observation.database_schema == DependencyState::Available &&
                 observation.identity_authorization == DependencyState::Available;
  return result;
}

} // namespace kiarabot::core::target
