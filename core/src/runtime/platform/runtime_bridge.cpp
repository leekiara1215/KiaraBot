#include "kiarabot/core/runtime/platform/runtime_bridge.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/adapters.pb.h"
#include "kiarabot/v1/plugins.pb.h"
#include "kiarabot/v1/services.pb.h"

namespace kiarabot::core::platform {
namespace {
namespace target = kiarabot::core::target;
namespace codec = kiarabot::core::platform::target_codec;
using target::Failure;
using target::Outcome;
using target::Status;

constexpr std::string_view kAuthorizationMetadata = "authorization";

Failure failure(target::ErrorCode code) {
  return {code, target::Acceptance::NotAccepted, target::Recovery::Diagnose, {}, {}};
}
grpc::StatusCode status_of(target::ErrorCode code) {
  auto mapped = codec::grpc_status(code);
  if (const auto* status = std::get_if<grpc::StatusCode>(&mapped)) {
    return *status;
  }
  return grpc::StatusCode::UNKNOWN;
}
v1::Error wire_error(target::ErrorCode code, std::string_view request_id,
                     std::string_view trace_id) {
  auto made = codec::make_error(code, request_id, trace_id, false);
  if (const auto* value = std::get_if<v1::Error>(&made)) {
    return *value;
  }
  v1::Error fallback;
  fallback.set_code(v1::INTERNAL);
  return fallback;
}
// Exactly one bearer bootstrap credential; any missing/duplicate/foreign metadata fails closed.
bool bearer_metadata(grpc::ServerContext& context, std::string& credential) {
  bool seen = false;
  for (const auto& entry : context.client_metadata()) {
    const std::string_view key{entry.first.data(), entry.first.size()};
    const std::string_view value{entry.second.data(), entry.second.size()};
    if (key != kAuthorizationMetadata) {
      continue;
    }
    if (seen) {
      return false;
    }
    seen = true;
    if (!value.starts_with("Bearer ")) {
      return false;
    }
    credential = std::string{value.substr(7)};
  }
  return seen && !credential.empty();
}
bool fence_to_wire(const target::BridgeFence& value, v1::Fence* wire) {
  if (!wire) {
    return false;
  }
  wire->set_instance_id(value.instance.value);
  wire->set_runtime_epoch(value.runtime_epoch.value);
  wire->set_execution_generation(value.execution_generation.value);
  wire->set_connection_generation(value.connection_generation.value);
  return true;
}
target::Outcome<target::BridgeFence> fence_from_wire(const v1::Fence& wire) {
  if (wire.instance_id().empty() || wire.runtime_epoch().empty() ||
      !wire.has_execution_generation() || !wire.has_connection_generation() ||
      wire.execution_generation() == 0 || wire.connection_generation() == 0) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  return target::BridgeFence{target::InstanceId{wire.instance_id()},
                             target::RuntimeEpoch{wire.runtime_epoch()},
                             target::ExecutionGeneration{wire.execution_generation()},
                             target::ConnectionGeneration{wire.connection_generation()}};
}
target::Outcome<std::array<std::byte, 32>> proof_from_wire(const std::string& value) {
  if (value.size() != 32) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  std::array<std::byte, 32> proof{};
  for (std::size_t index = 0; index < proof.size(); ++index) {
    proof[index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
  }
  return proof;
}
void authorization_to_wire(const target::BridgeAuthorization& value,
                           v1::ConnectionAuthorization* wire) {
  if (!wire) {
    return;
  }
  fence_to_wire(value.fence, wire->mutable_fence());
  wire->set_proof(reinterpret_cast<const char*>(value.proof.data()), value.proof.size());
  wire->set_refresh_credential(reinterpret_cast<const char*>(value.refresh_credential.data()),
                               value.refresh_credential.size());
  wire->set_refresh_generation(value.refresh_generation);
  wire->set_credential_version(value.credential_version);
  wire->set_authorization_version(value.authorization_version);
  if (auto issued = codec::to_wire(value.issued_at);
      std::holds_alternative<google::protobuf::Timestamp>(issued)) {
    *wire->mutable_issued_at() = std::get<google::protobuf::Timestamp>(issued);
  }
  if (auto expires = codec::to_wire(value.expires_at);
      std::holds_alternative<google::protobuf::Timestamp>(expires)) {
    *wire->mutable_expires_at() = std::get<google::protobuf::Timestamp>(expires);
  }
  wire->set_principal_id(value.fence.instance.value);
}
template <typename Ack> void fill_heartbeat_ack(Ack* ack) {
  ack->set_received_at_unix_ms(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count()));
}

// Shared post-registration loop state for one accepted stream.
struct StreamState {
  target::BridgePeerKind kind = target::BridgePeerKind::Adapter;
  target::BridgeAuthorization authorization;
  target::TimePoint sampled_at;
  bool sampled = false;
};

target::Outcome<target::BridgeFence> frame_fence(const v1::Fence& wire, const std::string& proof,
                                                 std::array<std::byte, 32>& parsed_proof) {
  auto fence = fence_from_wire(wire);
  if (const auto* rejected = std::get_if<target::Failure>(&fence)) {
    return *rejected;
  }
  auto parsed = proof_from_wire(proof);
  if (const auto* rejected = std::get_if<target::Failure>(&parsed)) {
    return *rejected;
  }
  parsed_proof = std::get<std::array<std::byte, 32>>(parsed);
  return std::get<target::BridgeFence>(fence);
}
target::Status authorize_frame(target::RuntimeBridgeApplication& application, StreamState& state,
                               const v1::Fence& fence, const std::string& proof) {
  std::array<std::byte, 32> parsed{};
  auto expected = frame_fence(fence, proof, parsed);
  if (const auto* rejected = std::get_if<target::Failure>(&expected)) {
    return *rejected;
  }
  auto sampled = application.current_time();
  if (const auto* rejected = std::get_if<target::Failure>(&sampled)) {
    return *rejected;
  }
  state.sampled_at = std::get<target::TimePoint>(sampled);
  state.sampled = true;
  return application.authorize_frame(state.kind, state.authorization,
                                     std::get<target::BridgeFence>(expected), parsed,
                                     state.sampled_at);
}
grpc::Status rejected_status(const target::Failure& rejected) {
  return {status_of(rejected.code), "bridge request rejected"};
}
target::Outcome<target::TimePoint> register_deadline(const v1::RequestContext& context) {
  if (!context.has_deadline()) {
    return failure(target::ErrorCode::InvalidArgument);
  }
  auto deadline = codec::from_wire(context.deadline());
  if (const auto* value = std::get_if<target::TimePoint>(&deadline)) {
    return *value;
  }
  return failure(target::ErrorCode::InvalidArgument);
}
std::vector<target::Capability>
capabilities_from_wire(const google::protobuf::RepeatedPtrField<v1::Capability>& wire) {
  std::vector<target::Capability> result;
  result.reserve(static_cast<std::size_t>(wire.size()));
  for (const auto& capability : wire) {
    result.push_back({capability.name(), capability.version()});
  }
  return result;
}
void capabilities_to_wire(const std::vector<target::Capability>& values,
                          google::protobuf::RepeatedPtrField<v1::Capability>* wire) {
  for (const auto& value : values) {
    auto* capability = wire->Add();
    capability->set_name(value.name);
    capability->set_version(value.version);
    capability->set_supported(true);
  }
}
target::Outcome<target::BridgeAuthorization>
refresh(target::RuntimeBridgeApplication& application, StreamState& state,
        const v1::RefreshConnectionAuthorization& wire);

class AdapterBridgeImpl final : public v1::AdapterBridge::Service {
public:
  explicit AdapterBridgeImpl(target::RuntimeBridgeApplication& application)
      : application_(application) {}

  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<v1::CoreToAdapter, v1::AdapterToCore>* stream) override {
    std::string bearer;
    if (!bearer_metadata(*context, bearer)) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bridge authorization missing"};
    }
    v1::AdapterToCore first;
    if (!stream->Read(&first) || first.body_case() != v1::AdapterToCore::kRegister) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "registration frame required"};
    }
    const auto& registered = first.register_();
    auto deadline = register_deadline(registered.context());
    if (const auto* rejected = std::get_if<target::Failure>(&deadline)) {
      return rejected_status(*rejected);
    }
    target::BridgeAdapterRegistration command;
    command.request_id = registered.context().request_id();
    command.trace_id = registered.context().trace_id();
    command.deadline = std::get<target::TimePoint>(deadline);
    command.adapter = target::ResourceId{registered.adapter_id()};
    command.implementation = registered.implementation();
    command.implementation_version = registered.implementation_version();
    command.protocol_version = registered.protocol_version();
    if (!registered.adapter_instance_id().empty()) {
      command.presented_instance = target::InstanceId{registered.adapter_instance_id()};
    }
    for (const auto& account : registered.accounts()) {
      command.declared_accounts.push_back(target::ResourceId{account.account_id()});
    }
    command.declared_capabilities = capabilities_from_wire(registered.capabilities());
    command.bearer_credential = bearer;
    auto outcome = application_.register_adapter(command);
    if (const auto* rejected = std::get_if<target::Failure>(&outcome)) {
      return rejected_status(*rejected);
    }
    const auto& result = std::get<target::BridgeRegistrationResult>(outcome);
    StreamState state{target::BridgePeerKind::Adapter, result.authorization, {}, false};
    {
      v1::CoreToAdapter response;
      auto* registered_response = response.mutable_register_result();
      registered_response->set_accepted(true);
      registered_response->set_assigned_instance_id(result.assigned_instance_id.value);
      registered_response->set_required_protocol_version(result.required_protocol_version);
      registered_response->set_heartbeat_interval_seconds(result.heartbeat_interval_seconds);
      fence_to_wire(result.authorization.fence, registered_response->mutable_fence());
      capabilities_to_wire(result.effective_capabilities,
                           registered_response->mutable_effective_capabilities());
      authorization_to_wire(result.authorization, registered_response->mutable_authorization());
      fence_to_wire(result.authorization.fence, response.mutable_fence());
      if (!stream->Write(response)) {
        return grpc::Status::OK;
      }
    }
    grpc::Status status = grpc::Status::OK;
    while (!context->IsCancelled()) {
      v1::AdapterToCore frame;
      if (!stream->Read(&frame)) {
        break;
      }
      auto authorized =
          authorize_frame(application_, state, frame.fence(), frame.connection_proof());
      if (const auto* rejected = std::get_if<target::Failure>(&authorized)) {
        status = rejected_status(*rejected);
        break;
      }
      const auto body = frame.body_case();
      if (body == v1::AdapterToCore::kHeartbeat) {
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        fill_heartbeat_ack(response.mutable_heartbeat_ack());
        state.sampled = false;
        if (!stream->Write(response)) {
          break;
        }
      } else if (body == v1::AdapterToCore::kRefreshAuthorization) {
        auto refreshed = refresh(application_, state, frame.refresh_authorization());
        if (const auto* rejected = std::get_if<target::Failure>(&refreshed)) {
          if (rejected->code == target::ErrorCode::Unauthenticated ||
              rejected->code == target::ErrorCode::PermissionDenied) {
            status = rejected_status(*rejected);
            break;
          }
          v1::CoreToAdapter response;
          fence_to_wire(state.authorization.fence, response.mutable_fence());
          auto* result = response.mutable_authorization_result();
          result->set_request_id(frame.refresh_authorization().request_id());
          *result->mutable_error() =
              wire_error(rejected->code, frame.refresh_authorization().request_id(), {});
          state.sampled = false;
          if (!stream->Write(response)) {
            break;
          }
          status = rejected_status(*rejected);
          break;
        }
        state.authorization = std::get<target::BridgeAuthorization>(refreshed);
        state.sampled = false;
        v1::CoreToAdapter response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* result = response.mutable_authorization_result();
        result->set_request_id(frame.refresh_authorization().request_id());
        authorization_to_wire(state.authorization, result->mutable_authorization());
        if (!stream->Write(response)) {
          break;
        }
      } else if (body == v1::AdapterToCore::kRegister) {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "duplicate registration frame"};
        break;
      } else {
        // Adapter event/action_result frames are wired in the event-delivery increment.
        status = {grpc::StatusCode::INVALID_ARGUMENT, "unsupported bridge frame"};
        break;
      }
    }
    application_.disconnect(target::BridgePeerKind::Adapter, state.authorization.fence.instance,
                            state.authorization.fence.connection_generation);
    return status;
  }

private:
  target::RuntimeBridgeApplication& application_;
};

class PluginBridgeImpl final : public v1::PluginBridge::Service {
public:
  explicit PluginBridgeImpl(target::RuntimeBridgeApplication& application)
      : application_(application) {}

  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<v1::CoreToPlugin, v1::PluginToCore>* stream) override {
    std::string bearer;
    if (!bearer_metadata(*context, bearer)) {
      return {grpc::StatusCode::UNAUTHENTICATED, "bridge authorization missing"};
    }
    v1::PluginToCore first;
    if (!stream->Read(&first) || first.body_case() != v1::AdapterToCore::kRegister) {
      return {grpc::StatusCode::INVALID_ARGUMENT, "registration frame required"};
    }
    const auto& registered = first.register_();
    target::BridgePluginRegistration command;
    command.request_id = "plugin-register-" + registered.instance_id();
    command.instance_id = registered.instance_id();
    command.bearer_credential = bearer;
    command.manifest.id = registered.manifest().id();
    command.manifest.name = registered.manifest().name();
    command.manifest.version = registered.manifest().version();
    command.manifest.api_version = registered.manifest().api_version();
    command.manifest.runtime = registered.manifest().runtime();
    command.manifest.entrypoint = registered.manifest().entrypoint();
    command.manifest.isolation = registered.manifest().isolation();
    command.manifest.config_schema = registered.manifest().config_schema();
    command.manifest.ordered = registered.manifest().ordered();
    command.manifest.capabilities.assign(registered.manifest().capabilities().begin(),
                                         registered.manifest().capabilities().end());
    command.manifest.permissions.assign(registered.manifest().permissions().begin(),
                                        registered.manifest().permissions().end());
    command.manifest.dependencies.assign(registered.manifest().dependencies().begin(),
                                         registered.manifest().dependencies().end());
    command.supported_features.assign(registered.supported_features().begin(),
                                      registered.supported_features().end());
    if (!registered.installation_id().empty()) {
      command.installation_id = target::InstallationId{registered.installation_id()};
    }
    if (registered.has_execution_generation()) {
      command.execution_generation = registered.execution_generation();
    }
    auto outcome = application_.register_plugin(command);
    if (const auto* rejected = std::get_if<target::Failure>(&outcome)) {
      return rejected_status(*rejected);
    }
    const auto& result = std::get<target::BridgeRegistrationResult>(outcome);
    StreamState state{target::BridgePeerKind::Plugin, result.authorization, {}, false};
    {
      v1::CoreToPlugin response;
      auto* registered_response = response.mutable_register_result();
      registered_response->set_accepted(true);
      registered_response->set_required_api_version(result.required_api_version);
      for (const auto& permission : result.granted_permissions) {
        registered_response->add_granted_permissions(permission);
      }
      for (const auto& feature : result.enabled_features) {
        registered_response->add_enabled_features(feature);
      }
      fence_to_wire(result.authorization.fence, registered_response->mutable_fence());
      authorization_to_wire(result.authorization, registered_response->mutable_authorization());
      fence_to_wire(result.authorization.fence, response.mutable_fence());
      if (!stream->Write(response)) {
        return grpc::Status::OK;
      }
    }
    grpc::Status status = grpc::Status::OK;
    while (!context->IsCancelled()) {
      v1::PluginToCore frame;
      if (!stream->Read(&frame)) {
        break;
      }
      auto authorized =
          authorize_frame(application_, state, frame.fence(), frame.connection_proof());
      if (const auto* rejected = std::get_if<target::Failure>(&authorized)) {
        status = rejected_status(*rejected);
        break;
      }
      const auto body = frame.body_case();
      if (body == v1::PluginToCore::kHeartbeat) {
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        fill_heartbeat_ack(response.mutable_heartbeat_ack());
        state.sampled = false;
        if (!stream->Write(response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kRefreshAuthorization) {
        auto refreshed = refresh(application_, state, frame.refresh_authorization());
        if (const auto* rejected = std::get_if<target::Failure>(&refreshed)) {
          if (rejected->code == target::ErrorCode::Unauthenticated ||
              rejected->code == target::ErrorCode::PermissionDenied) {
            status = rejected_status(*rejected);
            break;
          }
          v1::CoreToPlugin response;
          fence_to_wire(state.authorization.fence, response.mutable_fence());
          auto* result = response.mutable_authorization_result();
          result->set_request_id(frame.refresh_authorization().request_id());
          *result->mutable_error() =
              wire_error(rejected->code, frame.refresh_authorization().request_id(), {});
          state.sampled = false;
          if (!stream->Write(response)) {
            break;
          }
          status = rejected_status(*rejected);
          break;
        }
        state.authorization = std::get<target::BridgeAuthorization>(refreshed);
        state.sampled = false;
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        auto* result = response.mutable_authorization_result();
        result->set_request_id(frame.refresh_authorization().request_id());
        authorization_to_wire(state.authorization, result->mutable_authorization());
        if (!stream->Write(response)) {
          break;
        }
      } else if (body == v1::PluginToCore::kRegister) {
        status = {grpc::StatusCode::INVALID_ARGUMENT, "duplicate registration frame"};
        break;
      } else {
        // Event ACK / renew / action frames are wired in the event-delivery increment.
        v1::CoreToPlugin response;
        fence_to_wire(state.authorization.fence, response.mutable_fence());
        *response.mutable_error() = wire_error(target::ErrorCode::CapabilityNotSupported, {}, {});
        if (!stream->Write(response)) {
          break;
        }
      }
    }
    application_.disconnect(target::BridgePeerKind::Plugin, state.authorization.fence.instance,
                            state.authorization.fence.connection_generation);
    return status;
  }

private:
  target::RuntimeBridgeApplication& application_;
};

target::Outcome<target::BridgeAuthorization>
refresh(target::RuntimeBridgeApplication& application, StreamState& state,
        const v1::RefreshConnectionAuthorization& wire) {
  target::BridgeRefreshRequest request;
  auto fence = fence_from_wire(wire.fence());
  if (const auto* rejected = std::get_if<target::Failure>(&fence)) {
    return *rejected;
  }
  request.fence = std::get<target::BridgeFence>(fence);
  auto proof = proof_from_wire(wire.proof());
  if (const auto* rejected = std::get_if<target::Failure>(&proof)) {
    return *rejected;
  }
  request.proof = std::get<std::array<std::byte, 32>>(proof);
  auto refresh_material = proof_from_wire(wire.refresh_credential());
  if (const auto* rejected = std::get_if<target::Failure>(&refresh_material)) {
    return *rejected;
  }
  request.refresh_credential = std::get<std::array<std::byte, 32>>(refresh_material);
  request.refresh_generation = wire.refresh_generation();
  return application.refresh(state.kind, state.authorization.fence.instance, state.authorization,
                             request);
}
} // namespace

target::Outcome<std::unique_ptr<v1::AdapterBridge::Service>>
make_adapter_bridge(target::RuntimeBridgeApplication& application) {
  try {
    return std::unique_ptr<v1::AdapterBridge::Service>(
        std::make_unique<AdapterBridgeImpl>(application));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}
target::Outcome<std::unique_ptr<v1::PluginBridge::Service>>
make_plugin_bridge(target::RuntimeBridgeApplication& application) {
  try {
    return std::unique_ptr<v1::PluginBridge::Service>(
        std::make_unique<PluginBridgeImpl>(application));
  } catch (...) {
    return failure(target::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
