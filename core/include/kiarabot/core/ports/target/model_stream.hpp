#pragma once

#include <stop_token>

#include "kiarabot/core/domain/target/model_stream.hpp"

namespace kiarabot::core::target {

enum class ModelStreamSinkResult { Accepted, Full, Cancelled };

// Synchronous NONBLOCKING queue admission. No network I/O, unknown callbacks, retained borrowed
// references or application/UoW locks here. Full means no copy was accepted; the producer may
// retry the same value inside its bounded enqueue budget. Cancelled permanently stops this call.
// A producer catches exceptions at its boundary and converts them into stable unknown failure.
class ModelProviderStreamSink {
public:
  virtual ~ModelProviderStreamSink() = default;
  virtual ModelStreamSinkResult try_emit(const ModelStreamDelta& delta) = 0;
};

class ModelStreamSink {
public:
  virtual ~ModelStreamSink() = default;
  virtual ModelStreamSinkResult try_emit(const ModelStreamEvent& event) = 0;
};

// Cancellation uses standard C++20 stop_source/stop_token. The owner keeps the source alive
// until all workers join; the borrowed secret/sink remain alive until chat_stream returns.
// Cancellation cannot reverse provider execution, billing or an already committed call.
using ModelStreamCancellation = std::stop_token;

} // namespace kiarabot::core::target
