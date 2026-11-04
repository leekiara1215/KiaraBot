#pragma once

#include "kiarabot/core/application/target/runtime_resource_application.hpp"

namespace kiarabot::core::target::runtime_resources_detail {

class SnapshotPages;
std::unique_ptr<RuntimeAccountAdapterApplication>
make_account_adapter(const RuntimeResourceDependencies& dependencies, SnapshotPages& pages);
std::unique_ptr<RuntimeInstanceApplication>
make_instances(const RuntimeResourceDependencies& dependencies, SnapshotPages& pages);
std::unique_ptr<RuntimeLifecycleApplication>
make_lifecycle(const RuntimeResourceDependencies& dependencies);
std::unique_ptr<RuntimeHostCommandApplication>
make_host_commands(const RuntimeResourceDependencies& dependencies);
std::unique_ptr<RuntimeTimerApplication>
make_timer(const RuntimeResourceDependencies& dependencies);

} // namespace kiarabot::core::target::runtime_resources_detail
