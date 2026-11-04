#pragma once

#include <functional>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/observation_feed_application.hpp"
#include "kiarabot/v1/observations.pb.h"

namespace kiarabot::core::platform {

// Actual mTLS leaf and certificate-bound delegation metadata only. The pure application must still
// call current Identity before opening the owner UoW. The frozen (owner, family) binding decides
// the method name, permission and exact request scope kinds.
target::Outcome<target::ObservationFeedCommand> prepare_observation_feed(
    grpc::ServerContext& transport, const v1::ObservationChangesRequest& request,
    target::Service owner, target::ObservationFamily family,
    const std::function<target::Outcome<target::TimePoint>()>& invocation_deadline = {});
target::Status encode_log_changes_page(const target::ObservationFeedPage& page,
                                       const v1::RequestContext& request,
                                       v1::LogChangesResponse& response);
target::Status encode_task_changes_page(const target::ObservationFeedPage& page,
                                        const v1::RequestContext& request,
                                        v1::TaskChangesResponse& response);
target::Status encode_message_metadata_changes_page(const target::ObservationFeedPage& page,
                                                    const v1::RequestContext& request,
                                                    v1::MessageMetadataChangesResponse& response);

} // namespace kiarabot::core::platform
