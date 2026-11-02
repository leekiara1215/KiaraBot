#pragma once

#include "kiarabot/core/domain/target/runtime_records.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/platform.pb.h"

namespace kiarabot::core::platform::runtime_fence_codec {

// Adapter frames (registration/heartbeat and P06 Bridge connection frames) carry the wire Fence
// subset: stable adapter instance, current runtime epoch and a positive connection generation.
// `execution_generation` is explicitly REJECTED when present: an external adapter has no trusted
// process execution identity in P05 (that binding arrives with P08 Host-managed executions), and
// silently ignoring the field would let a client imply an execution authority it does not have.
inline target::Outcome<target::AdapterFenceClaim> adapter_claim(const v1::Fence& fence) {
  if (fence.instance_id().empty() || fence.runtime_epoch().empty() ||
      !fence.has_connection_generation() || fence.connection_generation() == 0 ||
      fence.has_execution_generation()) {
    return target::Failure{target::ErrorCode::InvalidArgument, target::Acceptance::NotAccepted,
                           target::Recovery::CorrectInput, std::nullopt, std::nullopt};
  }
  return target::AdapterFenceClaim{target::InstanceId{fence.instance_id()},
                                   target::RuntimeEpoch{fence.runtime_epoch()},
                                   target::ConnectionGeneration{fence.connection_generation()}};
}

// Renders the current adapter connection for a response: execution_generation is left absent.
inline target::Outcome<v1::Fence> to_wire(const target::AdapterFence& fence) {
  if (!target::valid_adapter_fence(fence)) {
    return target::Failure{target::ErrorCode::InvalidArgument, target::Acceptance::NotAccepted,
                           target::Recovery::CorrectInput, std::nullopt, std::nullopt};
  }
  v1::Fence result;
  result.set_instance_id(fence.instance.value);
  result.set_runtime_epoch(fence.runtime_epoch.value);
  result.set_connection_generation(fence.connection_generation.value);
  return result;
}

} // namespace kiarabot::core::platform::runtime_fence_codec
