#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "kiarabot/core/domain/target/model_stream.hpp"
#include "kiarabot/v1/model_stream.pb.h"

namespace kiarabot::core::platform {

// Boundary value conversion; malformed/presence/unknown enum failures never cross as text.
target::Outcome<v1::ModelStreamEvent>
encode_model_stream_event(const target::ModelStreamEvent& event) noexcept;

target::Outcome<v1::RpcFailure> encode_model_stream_failure(const target::Failure& failure,
                                                            std::string_view request_id,
                                                            std::string_view trace_id) noexcept;

// One decoder per attempt, owned by its transport consumer. Validates the full ordered prefix,
// including pinned identity, replay/no-text, terminal counts and authoritative billing presence.
// Invalid input poisons the decoder; a missing terminal is unknown delivery, never completion.
// Decode a validated pre-start RpcFailure trailer into the domain failure value. Correlation must
// match the attempt identifiers; malformed/foreign failures return nullopt.
std::optional<target::Failure> decode_model_stream_failure(const v1::RpcFailure& failure,
                                                           std::string_view request_id,
                                                           std::string_view trace_id) noexcept;

class ModelStreamDecoder {
public:
  ModelStreamDecoder(std::string request_id, std::string logical_call_id, std::string trace_id);
  target::Outcome<target::ModelStreamEvent> decode(const v1::ModelStreamEvent& event) noexcept;
  bool terminal_received() const noexcept {
    return terminal_;
  }

private:
  std::string request_id_;
  std::string logical_call_id_;
  std::string trace_id_;
  std::uint64_t next_sequence_ = 0;
  std::uint64_t bytes_ = 0;
  std::uint64_t deltas_ = 0;
  std::optional<v1::ModelStreamStarted> started_;
  bool terminal_ = false;
  bool invalid_ = false;
};
} // namespace kiarabot::core::platform
