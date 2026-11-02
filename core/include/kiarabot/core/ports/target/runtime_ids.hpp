#pragma once

#include <string>

#include "kiarabot/core/domain/target/foundation.hpp"

namespace kiarabot::core::target {

// Trusted owner-boundary id allocation. The domain never generates ids; the composition root
// supplies a CSPRNG-backed UUIDv4 adapter. Each getter returns one canonical lowercase UUIDv4 or a
// stable Failure (never an empty success that would become a synthetic id).
class RuntimeIdPort {
public:
  virtual ~RuntimeIdPort() = default;
  virtual Outcome<ResourceId> account_id() = 0;
  virtual Outcome<InstallationId> installation_id() = 0;
  virtual Outcome<InstanceId> instance_id() = 0;
  virtual Outcome<InstanceId> adapter_instance_id() = 0;
  virtual Outcome<ConnectionId> connection_id() = 0;
  virtual Outcome<OperationId> operation_id() = 0;
  virtual Outcome<RuntimeEpoch> runtime_epoch() = 0;
  virtual Outcome<std::string> snapshot_id() = 0;
};

} // namespace kiarabot::core::target
