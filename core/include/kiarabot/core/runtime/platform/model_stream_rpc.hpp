#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/sync_stream.h>

#include "kiarabot/core/application/target/model_gateway_application.hpp"
#include "kiarabot/v1/model_stream.pb.h"

namespace kiarabot::core::platform {

// Trusted, validated Model command only. The calling RPC thread is the sole gRPC writer;
// application/Provider threads never block in Write. Cancellation closes and joins all workers.
grpc::Status serve_model_chat_stream(grpc::ServerContext& transport,
                                     grpc::ServerWriter<v1::ModelStreamEvent>& writer,
                                     target::ModelGatewayApplication& application,
                                     const target::ModelChatCommand& command);

} // namespace kiarabot::core::platform
