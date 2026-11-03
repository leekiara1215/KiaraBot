#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/foundation.hpp"
#include "kiarabot/core/domain/target/model_gateway.hpp"

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
class RuntimeModelPort {
public:
  virtual ~RuntimeModelPort() = default;
  virtual Outcome<RuntimeModelChatReply> chat(const RuntimeModelChatRequest& request,
                                              TimePoint deadline) = 0;
  // Readiness observation only.
  virtual bool available() const noexcept = 0;
};

} // namespace kiarabot::core::target
