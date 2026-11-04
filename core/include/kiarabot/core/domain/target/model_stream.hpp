#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "kiarabot/core/domain/target/model_gateway.hpp"

namespace kiarabot::core::target {

inline constexpr std::size_t kModelStreamMaximumDeltaBytes = 4096;
inline constexpr std::size_t kModelStreamMaximumSseLineBytes = 65536;
inline constexpr std::size_t kModelStreamMaximumSseEventBytes = 65536;
inline constexpr std::size_t kModelStreamMaximumQueuedFrames = 32;
inline constexpr std::size_t kModelStreamMaximumQueuedBytes = 65536;
inline constexpr std::size_t kModelStreamMaximumConnectionCalls = 4;
inline constexpr std::size_t kModelStreamMaximumProcessCalls = 64;
inline constexpr auto kModelStreamEnqueueBudget = std::chrono::milliseconds{250};
inline constexpr auto kModelStreamWriteBudget = std::chrono::seconds{1};

// Validate text at the pure port boundary without depending on a transport/SDK UTF-8 library.
inline bool valid_model_stream_text(std::string_view text) noexcept {
  if (text.empty() || text.size() > kModelStreamMaximumDeltaBytes) {
    return false;
  }
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80U) {
      continue;
    }
    unsigned continuation = 0;
    unsigned minimum = 0x80U;
    unsigned maximum = 0xbfU;
    if (first >= 0xc2U && first <= 0xdfU) {
      continuation = 1;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuation = 2;
      if (first == 0xe0U) {
        minimum = 0xa0U;
      }
      if (first == 0xedU) {
        maximum = 0x9fU;
      }
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuation = 3;
      if (first == 0xf0U) {
        minimum = 0x90U;
      }
      if (first == 0xf4U) {
        maximum = 0x8fU;
      }
    } else {
      return false;
    }
    if (index + continuation > text.size()) {
      return false;
    }
    const auto second = static_cast<unsigned char>(text[index++]);
    if (second < minimum || second > maximum) {
      return false;
    }
    for (unsigned count = 1; count < continuation; ++count) {
      const auto byte = static_cast<unsigned char>(text[index++]);
      if (byte < 0x80U || byte > 0xbfU) {
        return false;
      }
    }
  }
  return true;
}

enum class ModelStreamDisposition { Fresh, ReplayInProgress, ReplayTerminal };
enum class ModelStreamTermination { Completed, Truncated, Failed, Cancelled, Replay };
enum class ModelStreamFinishReason { Unspecified, Stop, Length, ContentFilter };

struct ModelStreamDelta {
  std::string text;
};

struct ModelStreamStarted {
  ModelStreamDisposition disposition = ModelStreamDisposition::Fresh;
  ModelIntent call{};
};

struct ModelStreamTerminal {
  ModelStreamTermination termination = ModelStreamTermination::Failed;
  bool partial = false;
  std::uint64_t emitted_bytes = 0;
  std::uint64_t emitted_delta_count = 0;
  ModelStreamFinishReason finish_reason = ModelStreamFinishReason::Unspecified;
  std::optional<Failure> failure;
  std::optional<ModelIntent> call;
  ModelProviderUsage usage;
  bool may_have_billed = true;
};

// Pure domain events: wire conversion and authenticated Fence belong to transport boundaries.
// Event ownership is value-based. A sink must copy the event if it retains it after try_emit.
struct ModelStreamEvent {
  std::string request_id;
  std::string logical_call_id;
  std::uint64_t sequence = 0;
  std::string trace_id;
  std::variant<ModelStreamStarted, ModelStreamDelta, ModelStreamTerminal> body;
};

// Provider completion facts are independent of delivery completion and durable txn2. Succeeded
// here means the HTTP stream reached a valid finish+[DONE], not that Model has committed a call.
// DefiniteFailure is known rejection/non-execution; Unknown never implies known-unbilled failure.
struct ModelProviderStreamResult {
  ModelProviderOutcome outcome = ModelProviderOutcome::Unknown;
  ModelStreamTermination termination = ModelStreamTermination::Failed;
  ModelStreamFinishReason finish_reason = ModelStreamFinishReason::Unspecified;
  ModelProviderUsage usage;
  bool may_have_billed = true;
  bool provider_dispatched = false;
  bool partial = false;
  std::uint64_t emitted_bytes = 0;
  std::uint64_t emitted_delta_count = 0;
  std::string provider_request_id;
  std::uint32_t latency_ms = 0;
  std::optional<Failure> failure;
};

} // namespace kiarabot::core::target
