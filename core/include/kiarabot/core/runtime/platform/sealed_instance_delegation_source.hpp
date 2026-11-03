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
  std::string path;
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

private:
  explicit SealedInstanceDelegationSource(SealedInstanceDelegationSourceOptions options);
  std::string path_;
};
} // namespace kiarabot::core::platform
