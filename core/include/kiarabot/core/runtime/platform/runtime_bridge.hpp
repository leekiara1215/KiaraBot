#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/runtime_bridge_application.hpp"
#include "kiarabot/core/ports/target/runtime_model.hpp"
#include "kiarabot/v1/services.grpc.pb.h"

namespace kiarabot::core::platform {
namespace target = kiarabot::core::target;

// External AdapterBridge/PluginBridge transport adapters. They only convert wire values and map
// stable failures; all connection authority, connection registry state and durable registration
// effects live in RuntimeBridgeApplication. Registration succeeds with the frozen register_result
// frame; post-registration frames require the current complete Fence and connection_proof, and a
// rejected old connection is terminated with UNAUTHENTICATED. No secret is ever logged.
target::Outcome<std::unique_ptr<v1::AdapterBridge::Service>>
make_adapter_bridge(target::RuntimeBridgeApplication& application);
target::Outcome<std::unique_ptr<v1::PluginBridge::Service>>
make_plugin_bridge(target::RuntimeBridgeApplication& application,
                   target::RuntimeModelPort* model = nullptr);

} // namespace kiarabot::core::platform
