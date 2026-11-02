#pragma once

#include <functional>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/model_gateway_application.hpp"
#include "kiarabot/v1/model.grpc.pb.h"

namespace kiarabot::core::platform {
namespace target = kiarabot::core::target;

// Trusted composition callback mapping the real inbound mTLS evidence to a caller (delegation,
// subject, represented owner instance). It never trusts request metadata. P11a composition keeps a
// fail-closed implementation; P11c/P11d replace it with the Identity/Control delegation gate.
using ModelPeerAuthenticator =
    std::function<target::Outcome<target::ModelCaller>(grpc::ServerContext&)>;

// ModelService transport adapter. All authority/state decisions live in the application; this
// boundary only validates presence/encoding, converts values and maps stable failures.
target::Outcome<std::unique_ptr<v1::ModelService::Service>>
make_model_rpc(target::ModelGatewayApplication& application, ModelPeerAuthenticator authenticate);

} // namespace kiarabot::core::platform
