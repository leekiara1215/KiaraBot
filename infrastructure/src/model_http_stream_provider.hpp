#pragma once

#include "kiarabot/core/ports/target/model_ports.hpp"

namespace kiarabot::infrastructure {

core::target::ModelProviderStreamResult
perform_model_http_stream(const core::target::ModelProviderRequest& request,
                          core::target::ModelProviderStreamSink& sink,
                          core::target::ModelStreamCancellation cancellation);

} // namespace kiarabot::infrastructure
