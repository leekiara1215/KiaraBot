#include "model_sse_parser.hpp"

#include <array>
#include <cstddef>
#include <utf8_validity.h>
#include <utility>

#include <eventsource/sse_parser.h>

#include "kiarabot/core/domain/target/model_stream.hpp"

namespace kiarabot::infrastructure {
namespace t = core::target;

struct ModelSseParser::State {
  explicit State(Receiver value) : receiver(std::move(value)) {
    const sse_parser_buffers_t buffers{data.data(), data.size(),  id.data(),
                                       id.size(),   event.data(), event.size()};
    sse_parser_callbacks_t callbacks{};
    callbacks.userdata = this;
    callbacks.on_event = [](void* context, const sse_parser_event_t* parsed) {
      auto& state = *static_cast<State*>(context);
      if (state.fault != Fault::None) {
        return;
      }
      try {
        const std::string_view name = parsed->event ? parsed->event : "message";
        const std::string_view payload{parsed->data, parsed->data_len};
        if (!utf8_range::IsStructurallyValid(name) || !utf8_range::IsStructurallyValid(payload)) {
          state.fault = Fault::InvalidText;
        } else if (!state.receiver(name.empty() ? "message" : name, payload)) {
          state.fault = Fault::Stopped;
        }
      } catch (...) {
        state.fault = Fault::Stopped;
      }
    };
    callbacks.on_error = [](void* context, sse_parse_error_t error) {
      auto& state = *static_cast<State*>(context);
      if (state.fault != Fault::None) {
        return;
      }
      switch (error) {
      case SSE_PARSE_ERR_DATA_TOO_LARGE:
      case SSE_PARSE_ERR_EVENT_TYPE_TOO_LARGE:
        state.fault = Fault::EventTooLarge;
        break;
      case SSE_PARSE_ERR_EVENT_TYPE_INVALID:
        state.fault = Fault::InvalidText;
        break;
      case SSE_PARSE_ERR_ID_INVALID:
      case SSE_PARSE_ERR_INVALID_RETRY:
      case SSE_PARSE_ERR_UNKNOWN_FIELD:
        // This one-shot POST consumer never uses id/retry/reconnect; unknown SSE fields are
        // ignored, as required. No advisory metadata can cause a Provider request or a retry.
        break;
      }
    };
    sse_parser_init(&parser, &callbacks, &buffers);
  }
  Receiver receiver;
  Fault fault = Fault::None;
  std::size_t received_bytes = 0;
  std::size_t line_bytes = 0;
  std::array<char, t::kModelStreamMaximumSseEventBytes + 1> data{};
  std::array<char, t::kModelMaximumProviderRequestId + 1> id{};
  std::array<char, t::kModelStreamMaximumSseEventBytes + 1> event{};
  sse_parser_t parser{};
};

ModelSseParser::ModelSseParser(Receiver receiver)
    : state_(std::make_unique<State>(std::move(receiver))) {}
ModelSseParser::~ModelSseParser() = default;

bool ModelSseParser::feed(std::string_view bytes) noexcept {
  auto& state = *state_;
  if (state.fault != Fault::None) {
    return false;
  }
  if (bytes.size() > t::kModelMaximumProviderResponseBytes - state.received_bytes) {
    state.fault = Fault::ResponseTooLarge;
    return false;
  }
  state.received_bytes += bytes.size();
  // Bound even ignored/comment lines. Feed one bounded segment at a time, so a violation in
  // a later event never permits that event's callback before the check. The upstream parser
  // itself does not allocate and holds only the fixed buffers above.
  std::size_t begin = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] == '\r' || bytes[index] == '\n') {
      state.line_bytes = 0;
    } else if (++state.line_bytes > t::kModelStreamMaximumSseLineBytes) {
      state.fault = Fault::LineTooLarge;
      return false;
    }
    if (bytes[index] == '\r' || bytes[index] == '\n' || index + 1 == bytes.size()) {
      const auto segment = bytes.substr(begin, index - begin + 1);
      sse_parser_feed(&state.parser, segment.data(), segment.size());
      begin = index + 1;
      if (state.fault != Fault::None) {
        return false;
      }
    }
  }
  return true;
}

ModelSseParser::Fault ModelSseParser::fault() const noexcept {
  return state_->fault;
}

} // namespace kiarabot::infrastructure
