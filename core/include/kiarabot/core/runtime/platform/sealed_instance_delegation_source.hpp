#pragma once

#include <memory>
#include <string>

#include "kiarabot/core/runtime/platform/instance_delegation_provider.hpp"

namespace kiarabot::core::platform {

// B1 sealed short-lived instance-parent material. The deployment/Control refresher writes a 0600
// regular file (never a symlink) atomically; Runtime only reads and validates it, never creates or
// extends it. The bounded JSON envelope is:
// {"instance_id","installation_id","audience":"runtime",
//  "permissions":["task.schedule","task.read"],"scope":[{"kind","id"}...],
//  "issued_at","expires_at","token"}
// Timestamps are RFC3339 UTC strings; expires_at-issued_at must be <=56s. The token is the opaque
// parent delegation; Identity still verifies its signature/origin on every Exchange.
struct SealedInstanceDelegationSourceOptions {
  // Task envelope path: instance-delegation-<instance>.json
  std::string path;
  // Model envelope path: instance-delegation-<instance>-model.json. When empty, the sibling path
  // is derived from `path` by inserting "-model" before the final ".json".
  std::string model_path;
};
class SealedInstanceDelegationSource final : public InstanceParentDelegationSource {
public:
  static target::Outcome<std::unique_ptr<SealedInstanceDelegationSource>>
  create(SealedInstanceDelegationSourceOptions options);
  ~SealedInstanceDelegationSource() override;
  SealedInstanceDelegationSource(const SealedInstanceDelegationSource&) = delete;
  SealedInstanceDelegationSource& operator=(const SealedInstanceDelegationSource&) = delete;

  target::Outcome<InstanceParentDelegation> acquire(const target::SchedulerTaskContext& context,
                                                    target::Permission permission) override;
  // P05b-8: model envelope (RESOURCE_MODEL + model.chat/model.call.inspect). The model id is
  // checked against the single MODEL scope; instance/installation still bind the authenticated
  // bridge context even though the exchange scope is model-only.
  target::Outcome<InstanceParentDelegation>
  acquire_model(const target::SchedulerTaskContext& context, std::string_view model_id,
                target::Permission permission) override;

private:
  explicit SealedInstanceDelegationSource(SealedInstanceDelegationSourceOptions options);
  std::string path_;
  std::string model_path_;
};
} // namespace kiarabot::core::platform
