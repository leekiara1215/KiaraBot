#pragma once

#include "kiarabot/core/application/target/runtime_resources/types.hpp"

namespace kiarabot::core::target {

// Narrow Runtime resource use cases. Each call owns its local UoW; borrowed ports outlive calls.
class RuntimeLifecycleApplication {
public:
  virtual ~RuntimeLifecycleApplication() = default;

  virtual Outcome<ResourceState> get_resource_state(const ResourceStateQuery& request) = 0;
  virtual Outcome<Operation> get_operation(const OperationQuery& request) = 0;
  virtual Outcome<Operation> begin_delete(const BeginDeleteRequest& request) = 0;
  virtual Outcome<Operation> restore_resource(const RestoreResourceRequest& request) = 0;
  virtual Outcome<LifecycleMaintenance> lifecycle_maintenance() = 0;
};

} // namespace kiarabot::core::target
