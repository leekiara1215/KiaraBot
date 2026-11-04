#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/model_gateway.hpp"
#include "kiarabot/core/domain/target/model_stream.hpp"
#include "kiarabot/core/ports/target/model_stream.hpp"

namespace kiarabot::core::target {

// Runtime-side outbound Model Chat boundary. The Runtime transport forwards the plugin's stable
// logical_call_id; Model computes and persists the request hash server-side. Retrying or querying
// after an unknown outcome MUST reuse the same logical_call_id, so this port never generates one.
struct RuntimeModelChatRequest {
  InstanceId instance;
  // Authenticated bridge profile's installation; the sealed model parent envelope binds it even
  // though the exchanged child scope is model-only (P05b-4).
  InstallationId installation;
  std::string request_id;
  std::string logical_call_id;
  std::string model;
  std::vector<ModelMessage> messages;
};
struct RuntimeModelChatReply {
  std::string text;
  std::uint64_t input_tokens = 0;
  std::uint64_t output_tokens = 0;
};
// One streaming attempt over the Runtime -> Model boundary. request_id/trace_id are the attempt
// identifiers (canonical UUIDs); logical_call_id is the stable plugin-side call identity shared
// with the unary Chat replay semantics. The transport never invents either id.
struct RuntimeModelStreamRequest {
  InstanceId instance;
  InstallationId installation;
  std::string request_id;
  std::string trace_id;
  std::string logical_call_id;
  std::string model;
  std::vector<ModelMessage> messages;
};

// Non-blocking consumer boundary. Accepted means the event value was copied; Full asks the
// producer to retry the same event within its bounded budget; Cancelled permanently stops this
// attempt. The sink MUST NOT block on network writes or unknown callbacks.
class RuntimeModelStreamSink {
public:
  virtual ~RuntimeModelStreamSink() = default;
  virtual ModelStreamSinkResult try_emit(const ModelStreamEvent& event) = 0;
};

// Result of one consumed attempt. `terminal` is only set by an authoritative in-band terminal that
// passed strict wire validation; nullopt is unknown delivery and must never be reported as
// completion. Local received counters describe what Runtime delivered to its sink.
struct RuntimeModelStreamResult {
  std::optional<ModelStreamTerminal> terminal;
  std::uint64_t received_bytes = 0;
  std::uint64_t received_delta_count = 0;
  bool cancelled_by_consumer = false;
};

class RuntimeModelPort {
public:
  virtual ~RuntimeModelPort() = default;
  virtual Outcome<RuntimeModelChatReply> chat(const RuntimeModelChatRequest& request,
                                              TimePoint deadline) = 0;
  // Consume Model ChatStream with strict frame validation. The default implementation fails closed
  // so existing non-streaming implementations never fabricate a stream.
  virtual Outcome<RuntimeModelStreamResult> chat_stream(const RuntimeModelStreamRequest&,
                                                        RuntimeModelStreamSink&, std::stop_token,
                                                        TimePoint) {
    return Failure{
        ErrorCode::CapabilityNotSupported, Acceptance::NotAccepted, Recovery::CorrectInput, {}, {}};
  }
  // Readiness observation only.
  virtual bool available() const noexcept = 0;
};

} // namespace kiarabot::core::target
