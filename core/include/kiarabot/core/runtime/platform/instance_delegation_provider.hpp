#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/identity_permissions.hpp"
#include "kiarabot/core/domain/target/identity_policy.hpp"
#include "kiarabot/core/domain/target/service_records.hpp"
#include "kiarabot/core/ports/target/scheduler_task.hpp"
#include "kiarabot/core/runtime/platform/runtime_identity_client.hpp"

namespace kiarabot::core::platform {

// P05b-5/B1 Runtime-side boundary: a short-lived instance-parent delegation (subject=plugin
// instance, audience=runtime, business_hops=1) must be supplied by a controlled source. The exact
// delivery mechanism (internal RPC vs sealed short-lived deployment file) is a pending user
// decision; this internal port has no proto/wire change and intentionally does not manufacture a
// parent proof. Until a source is installed, production composition keeps the existing
// fail-closed Scheduler delegation provider.
struct InstanceParentDelegation {
  std::string token;
  target::TimePoint expires_at;
};
class InstanceParentDelegationSource {
public:
  virtual ~InstanceParentDelegationSource() = default;
  // The source must validate the sealed material against the authenticated bridge context and the
  // requested owner permission, and must never extend or cache a token beyond its own expiry.
  virtual target::Outcome<InstanceParentDelegation>
  acquire(const target::SchedulerTaskContext& context, target::Permission permission) = 0;
};
struct InstanceDelegationProviderOptions {
  std::shared_ptr<InstanceParentDelegationSource> source;
  // PEM certificate chain of this Runtime process; ExchangeDelegation must bind the child proof to
  // the actual Runtime leaf, not a caller-supplied string.
  std::string service_certificate_pem;
  RuntimeIdentityClient* identity = nullptr;
  std::chrono::milliseconds cache_ttl{10000};
};
class RuntimeInstanceDelegationProvider {
public:
  static target::Outcome<std::unique_ptr<RuntimeInstanceDelegationProvider>>
  create(InstanceDelegationProviderOptions options);
  ~RuntimeInstanceDelegationProvider();
  RuntimeInstanceDelegationProvider(const RuntimeInstanceDelegationProvider&) = delete;
  RuntimeInstanceDelegationProvider& operator=(const RuntimeInstanceDelegationProvider&) = delete;

  // Delegation proof used by the P10k plugin task path. The context is derived from the
  // authenticated PluginBridge session (owner instance/installation), never from wire input.
  target::Outcome<std::string>
  scheduler_task_delegation(const target::SchedulerTaskContext& context,
                            target::Permission permission);

private:
  RuntimeInstanceDelegationProvider(InstanceDelegationProviderOptions options,
                                    target::Sha256Digest leaf_fingerprint);
  target::Outcome<ExchangedDelegation> exchange(const target::SchedulerTaskContext& context,
                                                target::Permission permission);

  InstanceDelegationProviderOptions options_;
  target::Sha256Digest leaf_fingerprint_;
  std::mutex mutex_;
  struct Cached {
    std::string proof;
    target::TimePoint expires_at;
  };
  std::unordered_map<std::string, Cached> cache_;
};
target::TimePoint delegation_provider_now();

} // namespace kiarabot::core::platform
