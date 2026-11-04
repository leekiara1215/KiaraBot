#include "kiarabot/control/target/websocket_source_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <grpcpp/grpcpp.h>
#include <openssl/rand.h>

#include "kiarabot/control/target/websocket_protocol.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"
#include "kiarabot/v1/model.grpc.pb.h"
#include "kiarabot/v1/runtime.grpc.pb.h"
#include "kiarabot/v1/scheduler.grpc.pb.h"

namespace kiarabot::control::target {
namespace {
namespace codec = core::platform::target_codec;
std::string identifier() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    return {};
  }
  bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
  bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      result += '-';
    }
    result += digits[bytes[i] >> 4U];
    result += digits[bytes[i] & 0x0fU];
  }
  return result;
}
WebSocketFailure failed(const grpc::Status& status, const v1::RequestContext& request) {
  if (!status.error_details().empty()) {
    const auto decoded = codec::from_grpc_failure(status);
    if (!std::holds_alternative<v1::RpcFailure>(decoded)) {
      return {v1::INTERNAL, false};
    }
    const auto& failure = std::get<v1::RpcFailure>(decoded);
    if (failure.error().request_id() != request.request_id() ||
        failure.error().trace_id() != request.trace_id()) {
      return {v1::INTERNAL, false};
    }
    const bool resync = failure.error().code() == v1::CONFLICT &&
                        failure.context().acceptance() == v1::NOT_ACCEPTED &&
                        failure.context().recovery() == v1::RESYNC_SNAPSHOT &&
                        !failure.context().has_operation();
    return {failure.error().code(), resync};
  }
  switch (status.error_code()) {
  case grpc::StatusCode::DEADLINE_EXCEEDED:
    return {v1::TIMEOUT, false};
  case grpc::StatusCode::UNAVAILABLE:
  case grpc::StatusCode::CANCELLED:
    return {v1::UPSTREAM_UNAVAILABLE, false};
  case grpc::StatusCode::UNIMPLEMENTED:
    return {v1::CAPABILITY_NOT_SUPPORTED, false};
  default:
    return {v1::INTERNAL, false};
  }
}
bool valid_identity(const WebSocketIdentity& identity) {
  const auto sane = [](const core::target::TimePoint& value) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch());
    return seconds.count() >= 946684800 && seconds.count() < 7258118400;
  };
  return !identity.principal.empty() && identity.principal.size() <= 128 &&
         std::all_of(identity.principal.begin(), identity.principal.end(),
                     [](unsigned char c) { return c >= 32 && c <= 126; }) &&
         identity.identity_session.size() >= 32 && identity.identity_session.size() <= 256 &&
         std::all_of(identity.identity_session.begin(), identity.identity_session.end(),
                     [](unsigned char c) { return c >= 32 && c <= 126; }) &&
         identity.authorization_version != 0 && sane(identity.checked_at) &&
         sane(identity.expires_at) && identity.expires_at > identity.checked_at;
}
class Client final : public WebSocketSourcePort {
public:
  Client(std::shared_ptr<IdentityAuthClient> identity, const core::platform::ProcessConfig& config)
      : identity_(std::move(identity)) {
    grpc::SslCredentialsOptions tls;
    tls.pem_root_certs = config.tls.ca;
    tls.pem_cert_chain = config.tls.certificate;
    tls.pem_private_key = config.tls.private_key;
    const auto credentials = grpc::SslCredentials(tls);
    grpc::ChannelArguments limits;
    limits.SetMaxReceiveMessageSize(1024 * 1024);
    const auto& api = *config.control_api;
    if (api.runtime_endpoint) {
      runtime_ = v1::RuntimeService::NewStub(
          grpc::CreateCustomChannel(*api.runtime_endpoint, credentials, limits));
    }
    if (api.scheduler_endpoint) {
      scheduler_ = v1::SchedulerService::NewStub(
          grpc::CreateCustomChannel(*api.scheduler_endpoint, credentials, limits));
    }
    if (api.model_endpoint) {
      model_ = v1::ModelService::NewStub(
          grpc::CreateCustomChannel(*api.model_endpoint, credentials, limits));
    }
  }
  ~Client() override {
    cancel();
  }
  WebSocketResult<WebSocketPoll> poll(const WebSocketSelection& selection,
                                      std::string_view access_token,
                                      const std::optional<v1::StreamCursor>& cursor,
                                      WebSocketDeadline deadline) override {
    try {
      deadline =
          std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
      const auto validation = identity_->validate_stream(access_token, deadline);
      if (const auto* error = std::get_if<WebSocketFailure>(&validation)) {
        return *error;
      }
      // All frozen routes are recognized. Each real source is enabled only when
      // implemented; no reference stream or operation-id collection is substituted.
      if (selection.channel != "operations" || selection.owner == "host") {
        return WebSocketFailure{v1::CAPABILITY_NOT_SUPPORTED, false};
      }
      const auto permission =
          selection.owner == "scheduler" ? v1::PERMISSION_TASK_READ : v1::PERMISSION_RESOURCE_READ;
      const auto proof = identity_->delegate_stream(selection.owner, permission, selection.scope,
                                                    access_token, deadline);
      if (const auto* error = std::get_if<WebSocketFailure>(&proof)) {
        return *error;
      }
      v1::OperationChangesRequest input;
      const auto id = identifier();
      const auto trace = identifier();
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (id.empty() || trace.empty()) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return WebSocketFailure{v1::TIMEOUT, false};
      }
      input.mutable_context()->set_request_id(id);
      input.mutable_context()->set_trace_id(trace);
      input.mutable_context()->set_delegation(std::get<std::string>(proof));
      const auto wall = std::chrono::system_clock::now() + remaining;
      const auto encoded = codec::to_wire(core::target::TimePoint{
          std::chrono::duration_cast<core::target::TimePoint::duration>(wall.time_since_epoch())});
      if (!std::holds_alternative<google::protobuf::Timestamp>(encoded)) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      *input.mutable_context()->mutable_deadline() = std::get<google::protobuf::Timestamp>(encoded);
      *input.mutable_scope() = selection.scope;
      if (cursor) {
        *input.mutable_cursor() = *cursor;
      }
      input.set_page_size(32);
      grpc::ClientContext call;
      const auto remaining_micros = std::chrono::duration_cast<std::chrono::microseconds>(
          wall - std::chrono::system_clock::now());
      call.set_deadline(gpr_time_add(
          gpr_now(GPR_CLOCK_MONOTONIC),
          gpr_time_from_micros(std::max<std::int64_t>(remaining_micros.count(), 1), GPR_TIMESPAN)));
      struct Registration {
        Client& owner;
        grpc::ClientContext& context;
        ~Registration() {
          const std::lock_guard lock(owner.mutex_);
          owner.active_.erase(&context);
        }
      } registration{*this, call};
      {
        const std::lock_guard lock(mutex_);
        if (cancelled_) {
          return WebSocketFailure{v1::UPSTREAM_UNAVAILABLE, false};
        }
        active_.insert(&call);
      }
      WebSocketPoll result;
      result.identity = std::get<WebSocketIdentity>(validation);
      if (!valid_identity(result.identity)) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      grpc::Status status;
      if (selection.owner == "runtime" && runtime_) {
        status = runtime_->ListOperationChanges(&call, input, &result.operations);
      } else if (selection.owner == "scheduler" && scheduler_) {
        status = scheduler_->ListOperationChanges(&call, input, &result.operations);
      } else if (selection.owner == "model" && model_) {
        status = model_->ListOperationChanges(&call, input, &result.operations);
      } else {
        return WebSocketFailure{v1::CAPABILITY_NOT_SUPPORTED, false};
      }
      if (!status.ok()) {
        return failed(status, input.context());
      }
      const auto& page = result.operations;
      if (!page.has_next_cursor() || !page.has_checked_at() || page.changes_size() > 32 ||
          (page.has_more() && page.changes_size() == 0) ||
          page.next_cursor().source_service() != selection.owner ||
          page.next_cursor().epoch().empty() ||
          !websocket_same_scope(page.next_cursor().scope(), selection.scope) ||
          (cursor && (cursor->source_service() != selection.owner ||
                      cursor->epoch() != page.next_cursor().epoch() ||
                      !websocket_same_scope(cursor->scope(), selection.scope) ||
                      page.next_cursor().sequence() < cursor->sequence())) ||
          (!cursor && page.changes_size() != 0) ||
          !std::holds_alternative<std::string>(websocket_cursor_json(page.next_cursor())) ||
          !std::holds_alternative<core::target::TimePoint>(codec::from_wire(page.checked_at()))) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      auto sequence = cursor ? cursor->sequence() : page.next_cursor().sequence();
      for (const auto& change : page.changes()) {
        if (!change.has_cursor() || change.cursor().sequence() <= sequence ||
            change.cursor().sequence() > page.next_cursor().sequence() ||
            change.cursor().source_service() != selection.owner ||
            change.cursor().epoch() != page.next_cursor().epoch() ||
            !websocket_same_scope(change.cursor().scope(), selection.scope) ||
            !std::holds_alternative<std::string>(websocket_operation_event(change, 1))) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        sequence = change.cursor().sequence();
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return WebSocketFailure{v1::TIMEOUT, false};
      }
      return result;
    } catch (...) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
  }
  void cancel() noexcept override {
    const std::lock_guard lock(mutex_);
    cancelled_ = true;
    for (auto* context : active_) {
      context->TryCancel();
    }
  }

private:
  std::shared_ptr<IdentityAuthClient> identity_;
  std::unique_ptr<v1::RuntimeService::Stub> runtime_;
  std::unique_ptr<v1::SchedulerService::Stub> scheduler_;
  std::unique_ptr<v1::ModelService::Stub> model_;
  std::mutex mutex_;
  std::set<grpc::ClientContext*> active_;
  bool cancelled_ = false;
};
} // namespace
std::shared_ptr<WebSocketSourcePort>
make_websocket_source_client(std::shared_ptr<IdentityAuthClient> identity,
                             const core::platform::ProcessConfig& config) {
  return std::make_shared<Client>(std::move(identity), config);
}
} // namespace kiarabot::control::target
