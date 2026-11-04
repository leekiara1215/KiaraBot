#include "kiarabot/control/target/websocket_source_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
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
#include "kiarabot/v1/identity.grpc.pb.h"
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
bool bounded_printable(std::string_view value, std::size_t minimum, std::size_t maximum) {
  return value.size() >= minimum && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool is_authorization_failure(const grpc::Status& status) {
  if (status.error_details().empty()) {
    return status.error_code() == grpc::StatusCode::UNAUTHENTICATED ||
           status.error_code() == grpc::StatusCode::PERMISSION_DENIED;
  }
  const auto decoded = codec::from_grpc_failure(status);
  if (!std::holds_alternative<v1::RpcFailure>(decoded)) {
    return false;
  }
  const auto code = std::get<v1::RpcFailure>(decoded).error().code();
  return code == v1::UNAUTHENTICATED || code == v1::PERMISSION_DENIED;
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
// Page boundary validation shared by the three v1.2 feed families: the owner/family/epoch is
// carried by the owner, the scope must equal the authorized request scope, and a supplied cursor
// must belong to the same dataset at or before the returned boundary.
bool scoped_page_valid(std::string_view owner, const v1::ResourceScope& scope,
                       const std::optional<v1::StreamCursor>& cursor, const v1::StreamCursor& next,
                       std::size_t changes, bool has_more, bool has_checked_at,
                       const google::protobuf::Timestamp& checked_at) {
  if (!next.source_service().empty() && next.source_service() != owner) {
    return false;
  }
  if (next.epoch().empty() || !websocket_same_scope(next.scope(), scope) ||
      (has_more && changes == 0)) {
    return false;
  }
  if (!has_checked_at ||
      !std::holds_alternative<core::target::TimePoint>(codec::from_wire(checked_at))) {
    return false;
  }
  if (cursor &&
      (cursor->source_service() != owner || cursor->epoch() != next.epoch() ||
       !websocket_same_scope(cursor->scope(), scope) || next.sequence() < cursor->sequence())) {
    return false;
  }
  if (!cursor && changes != 0) {
    return false;
  }
  return true;
}
bool scoped_change_cursor(std::string_view owner, const v1::ResourceScope& scope,
                          const v1::StreamCursor& next, const v1::StreamCursor& change,
                          std::uint64_t previous_sequence) {
  return change.source_service() == owner && change.epoch() == next.epoch() &&
         websocket_same_scope(change.scope(), scope) && change.sequence() > previous_sequence &&
         change.sequence() <= next.sequence();
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
    if (!api.identity_endpoint.empty()) {
      identity_channel_ = grpc::CreateCustomChannel(api.identity_endpoint, credentials, limits);
      identity_service_ = v1::IdentityService::NewStub(identity_channel_);
    }
    if (api.runtime_endpoint) {
      runtime_channel_ = grpc::CreateCustomChannel(*api.runtime_endpoint, credentials, limits);
      runtime_ = v1::RuntimeService::NewStub(runtime_channel_);
    }
    if (api.scheduler_endpoint) {
      scheduler_channel_ = grpc::CreateCustomChannel(*api.scheduler_endpoint, credentials, limits);
      scheduler_ = v1::SchedulerService::NewStub(scheduler_channel_);
    }
    if (api.model_endpoint) {
      model_channel_ = grpc::CreateCustomChannel(*api.model_endpoint, credentials, limits);
      model_ = v1::ModelService::NewStub(model_channel_);
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
      // All frozen routes are recognized. The v1.2 feed channels use their real owner journals;
      // every other non-operations channel (including the not-yet-ready host source) fails closed
      // with CAPABILITY_NOT_SUPPORTED instead of an empty successful stream.
      if (selection.channel != "operations") {
        if (selection.channel == "logs" || selection.channel == "tasks" ||
            selection.channel == "messages") {
          return poll_scoped(selection, std::get<WebSocketIdentity>(validation), access_token,
                             cursor, deadline);
        }
        if (selection.channel == "health" || selection.channel == "adapters" ||
            selection.channel == "plugins") {
          return poll_snapshot(selection, std::get<WebSocketIdentity>(validation), access_token,
                               deadline);
        }
        return WebSocketFailure{v1::CAPABILITY_NOT_SUPPORTED, false};
      }
      if (selection.owner == "host") {
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
  // Snapshot channels. health observes the real local gRPC channel readiness (never a fabricated
  // HEALTHY); adapters/plugins query each selected exact resource through the real Runtime owner
  // RPC with one delegation per resource. Owner failures keep the observation unavailable
  // (unknown/stale) instead of inventing a value; authentication/permission failures close.
  WebSocketResult<WebSocketPoll> poll_snapshot(const WebSocketSelection& selection,
                                               WebSocketIdentity identity,
                                               std::string_view access_token,
                                               WebSocketDeadline deadline) {
    deadline =
        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
    WebSocketPoll result;
    result.identity = std::move(identity);
    WebSocketSnapshotBatch batch;
    if (selection.channel == "health") {
      WebSocketHealthObservation observation;
      const auto ready = [](const std::shared_ptr<grpc::Channel>& channel) {
        return channel != nullptr && channel->GetState(true) == GRPC_CHANNEL_READY;
      };
      observation.identity_confirmed = identity_->identity_ready() && ready(identity_channel_);
      observation.runtime_ready = ready(runtime_channel_);
      observation.scheduler_ready = ready(scheduler_channel_);
      observation.model_ready = ready(model_channel_);
      batch.health = observation;
      result.snapshot = std::move(batch);
      return result;
    }
    const bool adapters = selection.channel == "adapters";
    if (!runtime_) {
      return WebSocketFailure{v1::CAPABILITY_NOT_SUPPORTED, false};
    }
    const bool catalog_mode = selection.scope.resources_size() == 1 &&
                              selection.scope.resources(0).kind() == v1::RESOURCE_CATALOG;
    if (catalog_mode) {
      return poll_catalog_scan(selection, std::move(identity), access_token, deadline, adapters);
    }
    for (const auto& selected : selection.scope.resources()) {
      v1::ResourceScope single;
      *single.add_resources() = selected;
      const auto permission = adapters ? v1::PERMISSION_ADAPTER_READ : v1::PERMISSION_PLUGIN_READ;
      const auto proof =
          identity_->delegate_stream("runtime", permission, single, access_token, deadline);
      if (const auto* error = std::get_if<WebSocketFailure>(&proof)) {
        return *error;
      }
      const auto id = identifier();
      const auto trace = identifier();
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (id.empty() || trace.empty() || remaining <= std::chrono::steady_clock::duration::zero()) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      const auto wall = std::chrono::system_clock::now() + remaining;
      const auto encoded = codec::to_wire(core::target::TimePoint{
          std::chrono::duration_cast<core::target::TimePoint::duration>(wall.time_since_epoch())});
      if (!std::holds_alternative<google::protobuf::Timestamp>(encoded)) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
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
      v1::RequestContext context;
      context.set_request_id(id);
      context.set_trace_id(trace);
      context.set_delegation(std::get<std::string>(proof));
      *context.mutable_deadline() = std::get<google::protobuf::Timestamp>(encoded);
      bool available = false;
      if (adapters) {
        v1::RuntimeGetAdapterRequest input;
        *input.mutable_context() = context;
        *input.mutable_resource() = selected;
        v1::RuntimeGetAdapterResponse response;
        const auto status = runtime_->GetAdapter(&call, input, &response);
        if (status.ok()) {
          const auto& record = response.adapter();
          if (record.resource().kind() != v1::RESOURCE_ADAPTER ||
              record.resource().id() != selected.id() || record.version() == 0) {
            return WebSocketFailure{v1::INTERNAL, false};
          }
          WebSocketAdapterObservation observation;
          observation.record = record;
          observation.available = true;
          batch.adapters.push_back(std::move(observation));
          available = true;
        } else if (is_authorization_failure(status)) {
          return failed(status, context);
        }
      } else {
        v1::RuntimeGetInstanceRequest input;
        *input.mutable_context() = context;
        *input.mutable_resource() = selected;
        v1::RuntimeGetInstanceResponse response;
        const auto status = runtime_->GetInstance(&call, input, &response);
        if (status.ok()) {
          const auto& record = response.instance();
          if (record.instance_id() != selected.id() ||
              record.state().resource().kind() != v1::RESOURCE_INSTANCE ||
              record.state().version() == 0) {
            return WebSocketFailure{v1::INTERNAL, false};
          }
          WebSocketPluginObservation observation;
          observation.record = record;
          observation.available = true;
          batch.plugins.push_back(std::move(observation));
          available = true;
        } else if (is_authorization_failure(status)) {
          return failed(status, context);
        }
      }
      if (!available) {
        // Owner query failed transiently: keep the resource unknown/stale and never emit a value.
        if (adapters) {
          WebSocketAdapterObservation observation;
          batch.adapters.push_back(std::move(observation));
        } else {
          WebSocketPluginObservation observation;
          batch.plugins.push_back(std::move(observation));
        }
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return WebSocketFailure{v1::TIMEOUT, false};
    }
    result.snapshot = std::move(batch);
    return result;
  }

  // Full-directory immutable-page scan for adapters/plugins. The authorized request scope selects
  // exactly one deployment catalog resource; the owner ListAdapters/ListInstances pages are bound
  // to one immutable snapshot, every page re-authorizes online with a fresh delegation, and a
  // wrong snapshot/expired page/owner error closes the stream instead of yielding an empty page.
  WebSocketResult<WebSocketPoll> poll_catalog_scan(const WebSocketSelection& selection,
                                                   WebSocketIdentity identity,
                                                   std::string_view access_token,
                                                   WebSocketDeadline deadline, bool adapters) {
    deadline =
        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
    WebSocketPoll result;
    result.identity = std::move(identity);
    WebSocketSnapshotBatch batch;
    v1::ResourceScope catalog_scope;
    *catalog_scope.add_resources() = selection.scope.resources(0);
    const auto permission = adapters ? v1::PERMISSION_ADAPTER_READ : v1::PERMISSION_PLUGIN_READ;
    constexpr std::size_t kMaximumCatalogPages = 64;
    constexpr std::uint32_t kCatalogPageSize = 100;
    std::string page_token;
    std::string snapshot_id;
    std::uint64_t snapshot_authorization_version = 0;
    std::size_t pages = 0;
    do {
      if (std::chrono::steady_clock::now() >= deadline) {
        return WebSocketFailure{v1::TIMEOUT, false};
      }
      if (++pages > kMaximumCatalogPages) {
        return WebSocketFailure{v1::UPSTREAM_UNAVAILABLE, false};
      }
      // Online per-page authorization: a new delegation for every page, never a scan-wide grant.
      const auto proof =
          identity_->delegate_stream("runtime", permission, catalog_scope, access_token, deadline);
      if (const auto* error = std::get_if<WebSocketFailure>(&proof)) {
        return *error;
      }
      const auto id = identifier();
      const auto trace = identifier();
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (id.empty() || trace.empty() || remaining <= std::chrono::steady_clock::duration::zero()) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      const auto wall = std::chrono::system_clock::now() + remaining;
      const auto encoded = codec::to_wire(core::target::TimePoint{
          std::chrono::duration_cast<core::target::TimePoint::duration>(wall.time_since_epoch())});
      if (!std::holds_alternative<google::protobuf::Timestamp>(encoded)) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
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
      v1::RequestContext context;
      context.set_request_id(id);
      context.set_trace_id(trace);
      context.set_delegation(std::get<std::string>(proof));
      *context.mutable_deadline() = std::get<google::protobuf::Timestamp>(encoded);
      const auto validate_snapshot = [&](const v1::SnapshotInfo& snapshot) {
        if (!snapshot.has_observed_at() || !snapshot.has_expires_at() ||
            !bounded_printable(snapshot.snapshot_id(), 1, 128) ||
            snapshot.authorization_version() == 0 || snapshot.owner_service() != "runtime" ||
            !snapshot.has_page()) {
          return false;
        }
        const auto observed_wire = codec::from_wire(snapshot.observed_at());
        const auto expires_wire = codec::from_wire(snapshot.expires_at());
        const auto* observed = std::get_if<core::target::TimePoint>(&observed_wire);
        const auto* expires = std::get_if<core::target::TimePoint>(&expires_wire);
        if (observed == nullptr || expires == nullptr || *expires <= *observed) {
          return false;
        }
        if (snapshot_id.empty()) {
          snapshot_id = snapshot.snapshot_id();
          snapshot_authorization_version = snapshot.authorization_version();
        } else if (snapshot.snapshot_id() != snapshot_id ||
                   snapshot.authorization_version() != snapshot_authorization_version) {
          // The directory changed under the scan: fail closed instead of mixing two snapshots.
          return false;
        }
        const auto now =
            core::target::TimePoint{std::chrono::duration_cast<core::target::TimePoint::duration>(
                std::chrono::system_clock::now().time_since_epoch())};
        return *expires > now;
      };
      if (adapters) {
        v1::RuntimeListAdaptersRequest input;
        *input.mutable_context() = context;
        *input.mutable_catalog() = selection.scope.resources(0);
        input.mutable_page()->set_page_size(kCatalogPageSize);
        if (!page_token.empty()) {
          input.mutable_page()->set_page_token(page_token);
        }
        v1::RuntimeListAdaptersResponse response;
        const auto status = runtime_->ListAdapters(&call, input, &response);
        if (!status.ok()) {
          return is_authorization_failure(status)
                     ? failed(status, context)
                     : WebSocketFailure{v1::UPSTREAM_UNAVAILABLE, false};
        }
        if (!response.has_snapshot() || !validate_snapshot(response.snapshot()) ||
            static_cast<std::size_t>(response.adapters_size()) > kCatalogPageSize) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        for (const auto& record : response.adapters()) {
          if (record.resource().kind() != v1::RESOURCE_ADAPTER || record.resource().id().empty() ||
              record.version() == 0) {
            return WebSocketFailure{v1::INTERNAL, false};
          }
          WebSocketAdapterObservation observation;
          observation.record = record;
          observation.available = true;
          batch.adapters.push_back(std::move(observation));
        }
        page_token = response.snapshot().page().next_page_token();
      } else {
        v1::RuntimeListInstancesRequest input;
        *input.mutable_context() = context;
        *input.mutable_catalog() = selection.scope.resources(0);
        input.mutable_page()->set_page_size(kCatalogPageSize);
        if (!page_token.empty()) {
          input.mutable_page()->set_page_token(page_token);
        }
        v1::RuntimeListInstancesResponse response;
        const auto status = runtime_->ListInstances(&call, input, &response);
        if (!status.ok()) {
          return is_authorization_failure(status)
                     ? failed(status, context)
                     : WebSocketFailure{v1::UPSTREAM_UNAVAILABLE, false};
        }
        if (!response.has_snapshot() || !validate_snapshot(response.snapshot()) ||
            static_cast<std::size_t>(response.instances_size()) > kCatalogPageSize) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        for (const auto& record : response.instances()) {
          if (record.instance_id().empty() || record.state().version() == 0 ||
              record.state().resource().kind() != v1::RESOURCE_INSTANCE) {
            return WebSocketFailure{v1::INTERNAL, false};
          }
          WebSocketPluginObservation observation;
          observation.record = record;
          observation.available = true;
          batch.plugins.push_back(std::move(observation));
        }
        page_token = response.snapshot().page().next_page_token();
      }
    } while (!page_token.empty());
    if (std::chrono::steady_clock::now() >= deadline) {
      return WebSocketFailure{v1::TIMEOUT, false};
    }
    result.snapshot = std::move(batch);
    return result;
  }

  // v1.2 feed channels: logs (Identity/Runtime ListLogChanges), tasks (Scheduler
  // ListTaskChanges) and received-message metadata (Runtime ListMessageMetadataChanges). The owner
  // cursor/epoch is opaque and reused verbatim; malformed pages and wrong-dataset cursors fail
  // closed instead of being replaced by an empty success.
  WebSocketResult<WebSocketPoll> poll_scoped(const WebSocketSelection& selection,
                                             WebSocketIdentity identity,
                                             std::string_view access_token,
                                             const std::optional<v1::StreamCursor>& cursor,
                                             WebSocketDeadline deadline) {
    deadline =
        std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
    std::optional<std::string> delegation;
    v1::Permission permission = v1::PERMISSION_RESOURCE_READ;
    if (selection.channel == "logs") {
      permission = v1::PERMISSION_LOG_READ;
    } else if (selection.channel == "tasks") {
      permission = v1::PERMISSION_TASK_READ;
    } else {
      permission = v1::PERMISSION_MESSAGE_READ;
    }
    const bool identity_logs = selection.channel == "logs" && selection.owner == "identity";
    if (!identity_logs) {
      const auto proof = identity_->delegate_stream(selection.owner, permission, selection.scope,
                                                    access_token, deadline);
      if (const auto* error = std::get_if<WebSocketFailure>(&proof)) {
        return *error;
      }
      delegation = std::get<std::string>(proof);
    }
    v1::ObservationChangesRequest input;
    const auto id = identifier();
    const auto trace = identifier();
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (id.empty() || trace.empty() || remaining <= std::chrono::steady_clock::duration::zero()) {
      return WebSocketFailure{v1::INTERNAL, false};
    }
    input.mutable_context()->set_request_id(id);
    input.mutable_context()->set_trace_id(trace);
    if (delegation) {
      input.mutable_context()->set_delegation(*delegation);
    }
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
    result.identity = std::move(identity);
    grpc::Status status;
    WebSocketScopedBatch batch;
    if (identity_logs && identity_service_) {
      v1::LogChangesResponse page;
      status = identity_service_->ListLogChanges(&call, input, &page);
      if (!status.ok()) {
        return failed(status, input.context());
      }
      if (!scoped_page_valid(selection.owner, selection.scope, cursor, page.next_cursor(),
                             page.changes_size(), page.has_more(), page.has_checked_at(),
                             page.checked_at())) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      auto sequence = cursor ? cursor->sequence() : page.next_cursor().sequence();
      for (const auto& change : page.changes()) {
        if (!change.has_cursor() ||
            !scoped_change_cursor(selection.owner, selection.scope, page.next_cursor(),
                                  change.cursor(), sequence) ||
            !std::holds_alternative<std::string>(websocket_feed_event(change, 1))) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        sequence = change.cursor().sequence();
        batch.logs.push_back(change);
      }
      batch.next_cursor = page.next_cursor();
      batch.has_more = page.has_more();
    } else if (selection.channel == "tasks" && scheduler_) {
      v1::TaskChangesResponse page;
      status = scheduler_->ListTaskChanges(&call, input, &page);
      if (!status.ok()) {
        return failed(status, input.context());
      }
      if (!scoped_page_valid(selection.owner, selection.scope, cursor, page.next_cursor(),
                             page.changes_size(), page.has_more(), page.has_checked_at(),
                             page.checked_at())) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      auto sequence = cursor ? cursor->sequence() : page.next_cursor().sequence();
      for (const auto& change : page.changes()) {
        if (!change.has_cursor() ||
            !scoped_change_cursor(selection.owner, selection.scope, page.next_cursor(),
                                  change.cursor(), sequence) ||
            !std::holds_alternative<std::string>(websocket_feed_event(change, 1))) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        sequence = change.cursor().sequence();
        batch.tasks.push_back(change);
      }
      batch.next_cursor = page.next_cursor();
      batch.has_more = page.has_more();
    } else if (selection.channel == "messages" && runtime_) {
      v1::MessageMetadataChangesResponse page;
      status = runtime_->ListMessageMetadataChanges(&call, input, &page);
      if (!status.ok()) {
        return failed(status, input.context());
      }
      if (!scoped_page_valid(selection.owner, selection.scope, cursor, page.next_cursor(),
                             page.changes_size(), page.has_more(), page.has_checked_at(),
                             page.checked_at())) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      auto sequence = cursor ? cursor->sequence() : page.next_cursor().sequence();
      for (const auto& change : page.changes()) {
        if (!change.has_cursor() ||
            !scoped_change_cursor(selection.owner, selection.scope, page.next_cursor(),
                                  change.cursor(), sequence) ||
            !std::holds_alternative<std::string>(websocket_feed_event(change, 1))) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        sequence = change.cursor().sequence();
        batch.messages.push_back(change);
      }
      batch.next_cursor = page.next_cursor();
      batch.has_more = page.has_more();
    } else if (selection.channel == "logs" && selection.owner == "runtime" && runtime_) {
      v1::LogChangesResponse page;
      status = runtime_->ListLogChanges(&call, input, &page);
      if (!status.ok()) {
        return failed(status, input.context());
      }
      if (!scoped_page_valid(selection.owner, selection.scope, cursor, page.next_cursor(),
                             page.changes_size(), page.has_more(), page.has_checked_at(),
                             page.checked_at())) {
        return WebSocketFailure{v1::INTERNAL, false};
      }
      auto sequence = cursor ? cursor->sequence() : page.next_cursor().sequence();
      for (const auto& change : page.changes()) {
        if (!change.has_cursor() ||
            !scoped_change_cursor(selection.owner, selection.scope, page.next_cursor(),
                                  change.cursor(), sequence) ||
            !std::holds_alternative<std::string>(websocket_feed_event(change, 1))) {
          return WebSocketFailure{v1::INTERNAL, false};
        }
        sequence = change.cursor().sequence();
        batch.logs.push_back(change);
      }
      batch.next_cursor = page.next_cursor();
      batch.has_more = page.has_more();
    } else {
      return WebSocketFailure{v1::CAPABILITY_NOT_SUPPORTED, false};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return WebSocketFailure{v1::TIMEOUT, false};
    }
    result.scoped = std::move(batch);
    return result;
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
  std::shared_ptr<grpc::Channel> identity_channel_;
  std::shared_ptr<grpc::Channel> runtime_channel_;
  std::shared_ptr<grpc::Channel> scheduler_channel_;
  std::shared_ptr<grpc::Channel> model_channel_;
  std::unique_ptr<v1::IdentityService::Stub> identity_service_;
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
