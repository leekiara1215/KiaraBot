#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/runtime_bridge_application.hpp"
#include "kiarabot/core/application/target/runtime_storage_application.hpp"
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
struct PluginBridgeOptions {
  // P09b: optional storage application. When present the bridge consumes PluginToCore.host_request
  // storage frames and answers CoreToPlugin.host_response; when null those frames stay
  // CAPABILITY_NOT_SUPPORTED. Authorization for plugin frames comes from the trusted P06 bridge
  // session/profile, never from client fields.
  target::RuntimeStorageApplication* storage = nullptr;
  // P11c: optional Runtime->Model chat port for PluginToCore.host_request model_chat frames. When
  // null model_chat stays CAPABILITY_NOT_SUPPORTED; other host operations are unaffected.
  target::RuntimeModelPort* model = nullptr;
};
target::Outcome<std::unique_ptr<v1::PluginBridge::Service>>
make_plugin_bridge(target::RuntimeBridgeApplication& application, PluginBridgeOptions options = {});

} // namespace kiarabot::core::platform
