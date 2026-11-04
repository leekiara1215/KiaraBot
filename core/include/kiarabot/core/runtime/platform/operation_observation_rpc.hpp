#pragma once

#include <functional>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/operation_observation_application.hpp"
#include "kiarabot/v1/observations.pb.h"

namespace kiarabot::core::platform {

// Actual mTLS leaf and certificate-bound delegation metadata only. The pure
// application must still call current Identity before opening the owner UoW.
target::Outcome<target::OperationObservationCommand> prepare_operation_observation(
    grpc::ServerContext& transport, const v1::OperationChangesRequest& request,
    target::Service owner,
    const std::function<target::Outcome<target::TimePoint>()>& invocation_deadline = {});
target::Status encode_operation_observation_page(const target::OperationObservationPage& page,
                                                 const v1::RequestContext& request,
                                                 v1::OperationChangesResponse& response);

} // namespace kiarabot::core::platform
