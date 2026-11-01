#pragma once

#include <memory>

#include "kiarabot/core/runtime/platform/service_config.hpp"
#include "kiarabot/core/runtime/platform/service_process.hpp"

namespace kiarabot::control::target {

// Exclusive outer owner. Async TLS/read/write deadlines are five seconds; max 64 connections,
// 8KiB headers and 1MiB body. Four bounded upstream workers; shutdown cancels RPC and joins.
// Construction validates local resources; start reports bind failure without throwing secrets.
std::unique_ptr<core::platform::ServiceEndpoint>
make_https_endpoint(const core::platform::ProcessConfig& config);

} // namespace kiarabot::control::target
