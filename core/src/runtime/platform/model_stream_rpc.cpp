#include "kiarabot/core/runtime/platform/model_stream_rpc.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "kiarabot/core/runtime/platform/model_stream_codec.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {
namespace t = target;
using Clock = std::chrono::steady_clock;
class Queue final : public t::ModelStreamSink {
public:
  Queue(const t::ModelChatCommand& command, std::stop_source& stop)
      : decoder(command.context.request_id, command.logical_call_id, command.context.trace_id),
        stop_(stop) {}
  t::ModelStreamSinkResult try_emit(const t::ModelStreamEvent& event) override {
    const bool is_terminal = std::holds_alternative<t::ModelStreamTerminal>(event.body);
    if (stop_.stop_requested() && !is_terminal) {
      return t::ModelStreamSinkResult::Cancelled;
    }
    std::lock_guard lock{mutex};
    const auto* delta = std::get_if<t::ModelStreamDelta>(&event.body);
    const auto bytes = delta ? delta->text.size() : 0;
    if (!is_terminal && (pending.size() >= t::kModelStreamMaximumQueuedFrames ||
                         queued_bytes + bytes > t::kModelStreamMaximumQueuedBytes)) {
      return t::ModelStreamSinkResult::Full;
    }
    auto wire = encode_model_stream_event(event);
    if (failed || !std::holds_alternative<v1::ModelStreamEvent>(wire) ||
        !std::holds_alternative<t::ModelStreamEvent>(
            decoder.decode(std::get<v1::ModelStreamEvent>(wire)))) {
      failed = true;
      stop_.request_stop();
      changed.notify_all();
      return t::ModelStreamSinkResult::Cancelled;
    }
    if (is_terminal) {
      terminal = std::move(std::get<v1::ModelStreamEvent>(wire));
    } else {
      pending.push_back(std::move(std::get<v1::ModelStreamEvent>(wire)));
      queued_bytes += bytes;
    }
    changed.notify_all();
    return t::ModelStreamSinkResult::Accepted;
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<v1::ModelStreamEvent> pending;
  std::optional<v1::ModelStreamEvent> terminal;
  std::size_t queued_bytes = 0;
  bool failed = false;
  bool done = false;
  ModelStreamDecoder decoder;
  std::optional<Clock::time_point> write_deadline;

private:
  std::stop_source& stop_;
};
grpc::Status stream_error(const t::ModelChatCommand& command) {
  const auto wire =
      encode_model_stream_failure({t::ErrorCode::UpstreamUnavailable,
                                   t::Acceptance::Unknown,
                                   t::Recovery::QuerySameIdentity,
                                   {},
                                   {}},
                                  command.context.request_id, command.context.trace_id);
  if (const auto* failure = std::get_if<v1::RpcFailure>(&wire)) {
    auto status = target_codec::to_grpc_failure(*failure);
    if (const auto* result = std::get_if<grpc::Status>(&status)) {
      return *result;
    }
  }
  return grpc::Status{grpc::StatusCode::INTERNAL, ""};
}
} // namespace

grpc::Status serve_model_chat_stream(grpc::ServerContext& transport,
                                     grpc::ServerWriter<v1::ModelStreamEvent>& writer,
                                     t::ModelGatewayApplication& application,
                                     const t::ModelChatCommand& command) {
  std::stop_source stop;
  Queue queue{command, stop};
  const auto wall_now = t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch())};
  const auto text_deadline = Clock::now() + (command.context.deadline - wall_now);
  bool wrote_terminal = false;
  try {
    std::jthread application_worker{[&](std::stop_token local_stop) {
      std::stop_callback propagate{local_stop, [&] { stop.request_stop(); }};
      try {
        (void)application.chat_stream(command, queue, stop.get_token());
      } catch (...) {
        std::lock_guard lock{queue.mutex};
        queue.failed = true;
      }
      {
        std::lock_guard lock{queue.mutex};
        queue.done = true;
      }
      queue.changed.notify_all();
    }};
    std::jthread watchdog{[&](std::stop_token local_stop) {
      while (!local_stop.stop_requested()) {
        bool cancel_write = false;
        {
          std::unique_lock lock{queue.mutex};
          if (queue.write_deadline && Clock::now() >= *queue.write_deadline) {
            cancel_write = true;
          }
          queue.changed.wait_for(lock, std::chrono::milliseconds{10});
        }
        if (Clock::now() >= text_deadline || transport.IsCancelled()) {
          stop.request_stop();
        }
        if (cancel_write) {
          stop.request_stop();
          transport.TryCancel();
          break;
        }
      }
    }};
    while (true) {
      std::optional<v1::ModelStreamEvent> next;
      {
        std::unique_lock lock{queue.mutex};
        if (transport.IsCancelled() || queue.failed) {
          break;
        }
        if (!queue.pending.empty()) {
          next = std::move(queue.pending.front());
          queue.queued_bytes -= next->has_delta() ? next->delta().text().size() : 0;
          queue.pending.pop_front();
        } else if (queue.terminal) {
          next = std::move(queue.terminal);
          queue.terminal.reset();
        } else if (queue.done) {
          break;
        } else {
          queue.changed.wait_for(lock, std::chrono::milliseconds{10});
          continue;
        }
      }
      const bool final = next->has_terminal();
      const auto deadline =
          std::min(Clock::now() + t::kModelStreamWriteBudget,
                   final ? text_deadline + std::chrono::seconds{5} : text_deadline);
      if (Clock::now() >= deadline) {
        transport.TryCancel();
        break;
      }
      {
        std::lock_guard lock{queue.mutex};
        queue.write_deadline = deadline;
      }
      queue.changed.notify_all();
      const bool written = writer.Write(*next);
      {
        std::lock_guard lock{queue.mutex};
        queue.write_deadline.reset();
      }
      queue.changed.notify_all();
      if (!written) {
        transport.TryCancel();
        break;
      }
      if (final) {
        wrote_terminal = true;
        break;
      }
    }
    stop.request_stop();
    queue.changed.notify_all();
    watchdog.request_stop();
    watchdog.join();
    application_worker.request_stop();
    application_worker.join();
  } catch (...) {
    stop.request_stop();
    transport.TryCancel();
    return stream_error(command);
  }
  if (wrote_terminal) {
    return grpc::Status::OK;
  }
  return stream_error(command);
}
} // namespace kiarabot::core::platform
