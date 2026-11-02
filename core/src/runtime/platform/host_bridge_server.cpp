#include "kiarabot/core/runtime/platform/host_bridge_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/util/time_util.h>
#include <grpcpp/security/auth_context.h>

#include "kiarabot/core/runtime/platform/peer_certificate.hpp"
#include "kiarabot/core/runtime/platform/target_codec.hpp"

namespace kiarabot::core::platform {
namespace {

namespace t = core::target;
namespace v1 = kiarabot::v1;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}
t::Failure invalid_argument() {
  return failure(t::ErrorCode::InvalidArgument, t::Recovery::CorrectInput);
}
t::Failure permission_denied() {
  return failure(t::ErrorCode::PermissionDenied, t::Recovery::Authenticate);
}
t::Failure unauthenticated() {
  return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
}
t::Failure conflict() {
  return failure(t::ErrorCode::Conflict, t::Recovery::ReadCurrent);
}

std::string hex_digest(const t::Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(digest.bytes.size() * 2);
  for (const std::byte byte : digest.bytes) {
    const auto value = static_cast<unsigned char>(byte);
    text.push_back(kHex[value >> 4]);
    text.push_back(kHex[value & 0x0f]);
  }
  return text;
}

grpc::Status status_of(const t::Failure& failure) {
  auto mapped = target_codec::grpc_status(failure.code);
  if (const auto* status = std::get_if<grpc::StatusCode>(&mapped)) {
    return grpc::Status(*status, std::string{});
  }
  return grpc::Status(grpc::StatusCode::INTERNAL, std::string{});
}

bool has_execution_generation(const v1::Fence& fence) {
  return fence.has_execution_generation();
}

bool fence_matches(const v1::Fence& wire, const t::HostFence& current) {
  return wire.instance_id() == current.host.value &&
         wire.runtime_epoch() == current.runtime_epoch && wire.has_connection_generation() &&
         wire.connection_generation() == current.connection_generation &&
         !has_execution_generation(wire);
}

void fence_to_wire(const t::HostFence& fence, v1::Fence* wire) {
  wire->set_instance_id(fence.host.value);
  wire->set_runtime_epoch(fence.runtime_epoch);
  wire->set_connection_generation(fence.connection_generation);
}

bool timestamp_to_time(const google::protobuf::Timestamp& value, t::TimePoint* output) {
  auto decoded = target_codec::from_wire(value);
  if (!std::holds_alternative<t::TimePoint>(decoded)) {
    return false;
  }
  *output = std::get<t::TimePoint>(decoded);
  return true;
}

void time_to_timestamp(t::TimePoint value, google::protobuf::Timestamp* output) {
  auto encoded = target_codec::to_wire(value);
  if (const auto* timestamp = std::get_if<google::protobuf::Timestamp>(&encoded)) {
    *output = *timestamp;
  }
}

void set_error(const t::Failure& rejected, std::string_view request_id, std::string_view trace_id,
               v1::RuntimeToHost* frame) {
  auto made = target_codec::make_error(rejected.code, request_id, trace_id,
                                       rejected.recovery == t::Recovery::Backoff);
  if (const auto* value = std::get_if<v1::Error>(&made)) {
    *frame->mutable_failure()->mutable_error() = *value;
  } else {
    const auto wire = target_codec::to_wire(rejected.code);
    if (const auto* code = std::get_if<v1::ErrorCode>(&wire)) {
      // Missing/invalid correlation fields on a Host frame must not lose the stable code.
      frame->mutable_failure()->mutable_error()->set_code(*code);
      frame->mutable_failure()->mutable_error()->set_message(v1::ErrorCode_Name(*code));
      frame->mutable_failure()->mutable_error()->set_retryable(rejected.recovery ==
                                                               t::Recovery::Backoff);
    }
  }
  frame->mutable_failure()->mutable_context()->set_recovery(v1::RECOVERY_UNSPECIFIED);
}

void set_context_ids(const v1::RequestContext& context, std::string* request_id,
                     std::string* trace_id) {
  *request_id = context.request_id();
  *trace_id = context.trace_id();
}

bool valid_identifier(std::string_view value, std::size_t maximum = 128) {
  return !value.empty() && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(), [](char character) {
           return character == '\0' || character == '\n' || character == '\r';
         });
}

class HostBridgeImpl final : public v1::HostBridge::Service {
public:
  explicit HostBridgeImpl(HostBridgeServerDependencies dependencies)
      : dependencies_(std::move(dependencies)) {}

  grpc::Status
  Connect(grpc::ServerContext* context,
          grpc::ServerReaderWriter<v1::RuntimeToHost, v1::HostToRuntime>* stream) override {
    auto fingerprint = dependencies_.peer_certificate(*context);
    if (const auto* rejected = std::get_if<t::Failure>(&fingerprint)) {
      return status_of(*rejected);
    }
    v1::HostToRuntime first;
    if (!stream->Read(&first)) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "registration frame required");
    }
    if (first.body_case() != v1::HostToRuntime::kRegistration) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "registration frame required");
    }
    const auto& registration = first.registration();
    const auto& host = registration.host();
    std::string request_id;
    std::string trace_id;
    set_context_ids(first.context(), &request_id, &trace_id);
    if (!valid_identifier(host.host_id())) {
      return status_of(invalid_argument());
    }
    std::vector<std::string> capabilities;
    capabilities.reserve(static_cast<std::size_t>(host.capabilities_size()));
    for (const auto& capability : host.capabilities()) {
      if (!valid_identifier(capability) ||
          std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end()) {
        return status_of(invalid_argument());
      }
      capabilities.push_back(capability);
    }
    auto epoch = dependencies_.clock.runtime_epoch();
    if (const auto* rejected = std::get_if<t::Failure>(&epoch)) {
      return status_of(*rejected);
    }
    t::HostRegistrationCommand command;
    command.certificate_sha256 = std::get<std::string>(fingerprint);
    command.host_id = host.host_id();
    command.capabilities = std::move(capabilities);
    command.runtime_epoch = std::get<std::string>(epoch);
    t::HostRegistrationResult registered;
    t::HostFence fence;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      t::RuntimeHostApplication application(dependencies_.registration, dependencies_.clock);
      auto result = application.register_host(command);
      if (const auto* rejected = std::get_if<t::Failure>(&result)) {
        return status_of(*rejected);
      }
      registered = std::move(std::get<t::HostRegistrationResult>(result));
      fence = registered.connection.fence;
    }
    v1::RuntimeToHost response;
    fence_to_wire(fence, response.mutable_fence());
    auto* accepted = response.mutable_registered();
    auto* record = accepted->mutable_host();
    record->set_host_id(registered.registration.host.value);
    record->set_version(registered.registration.version.value);
    for (const auto& capability : registered.registration.capabilities) {
      record->add_capabilities(capability);
    }
    time_to_timestamp(registered.registration.observed_at, record->mutable_observed_at());
    fence_to_wire(fence, accepted->mutable_fence());
    if (!stream->Write(response)) {
      return grpc::Status::OK;
    }

    v1::HostToRuntime frame;
    while (stream->Read(&frame)) {
      const auto body = frame.body_case();
      std::string frame_request_id;
      std::string frame_trace_id;
      set_context_ids(frame.context(), &frame_request_id, &frame_trace_id);
      if (body == v1::HostToRuntime::kRegistration) {
        send_failure(stream, conflict(), fence, frame_request_id, frame_trace_id);
        return grpc::Status::OK;
      }
      if (!frame.has_fence() || !fence_matches(frame.fence(), fence)) {
        send_failure(stream, conflict(), fence, frame_request_id, frame_trace_id);
        return grpc::Status::OK;
      }
      if (body == v1::HostToRuntime::kHeartbeat) {
        if (!handle_heartbeat(stream, frame, fence)) {
          return grpc::Status::OK;
        }
        continue;
      }
      if (body == v1::HostToRuntime::kInventory) {
        if (frame.inventory().host_id() != fence.host.value) {
          send_failure(stream, conflict(), fence, frame_request_id, frame_trace_id);
          return grpc::Status::OK;
        }
        t::Status handled = std::monostate{};
        if (dependencies_.callbacks.inventory) {
          handled = dependencies_.callbacks.inventory(frame.inventory());
        }
        if (const auto* rejected = std::get_if<t::Failure>(&handled)) {
          send_failure(stream, *rejected, fence, frame_request_id, frame_trace_id);
          return grpc::Status::OK;
        }
        continue;
      }
      if (body == v1::HostToRuntime::kReceipt) {
        if (!valid_identifier(frame.receipt().command_id())) {
          send_failure(stream, invalid_argument(), fence, frame_request_id, frame_trace_id);
          return grpc::Status::OK;
        }
        t::Status handled = std::monostate{};
        if (dependencies_.callbacks.receipt) {
          handled = dependencies_.callbacks.receipt(frame.receipt());
        }
        if (const auto* rejected = std::get_if<t::Failure>(&handled)) {
          send_failure(stream, *rejected, fence, frame_request_id, frame_trace_id);
          return grpc::Status::OK;
        }
        continue;
      }
      if (body == v1::HostToRuntime::kObservation) {
        const auto& observation = frame.observation();
        if (!observation.has_execution() || observation.execution().host_id() != fence.host.value ||
            !valid_identifier(observation.execution().instance_id())) {
          send_failure(stream, invalid_argument(), fence, frame_request_id, frame_trace_id);
          return grpc::Status::OK;
        }
        t::Status handled = std::monostate{};
        if (dependencies_.callbacks.observation) {
          handled = dependencies_.callbacks.observation(observation);
        }
        if (const auto* rejected = std::get_if<t::Failure>(&handled)) {
          send_failure(stream, *rejected, fence, frame_request_id, frame_trace_id);
          return grpc::Status::OK;
        }
        continue;
      }
      send_failure(stream, invalid_argument(), fence, frame_request_id, frame_trace_id);
      return grpc::Status::OK;
    }
    return grpc::Status::OK;
  }

private:
  bool handle_heartbeat(grpc::ServerReaderWriter<v1::RuntimeToHost, v1::HostToRuntime>* stream,
                        const v1::HostToRuntime& frame, const t::HostFence& fence) {
    const auto& heartbeat = frame.heartbeat();
    t::TimePoint sent_at;
    if (!timestamp_to_time(heartbeat.sent_at(), &sent_at)) {
      send_failure(stream, invalid_argument(), fence, frame.context().request_id(),
                   frame.context().trace_id());
      return false;
    }
    t::HostHeartbeatResult result;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      t::RuntimeHostApplication application(dependencies_.registration, dependencies_.clock);
      auto handled = application.heartbeat(t::HostHeartbeatCommand{fence, sent_at});
      if (const auto* rejected = std::get_if<t::Failure>(&handled)) {
        send_failure(stream, *rejected, fence, frame.context().request_id(),
                     frame.context().trace_id());
        return false;
      }
      result = std::move(std::get<t::HostHeartbeatResult>(handled));
    }
    bool resync_required = result.resync_required;
    if (heartbeat.has_cursor() && (heartbeat.cursor().epoch() != fence.runtime_epoch ||
                                   !valid_identifier(heartbeat.cursor().source_service(), 64))) {
      resync_required = true;
    }
    v1::RuntimeToHost response;
    fence_to_wire(result.fence, response.mutable_fence());
    auto* ack = response.mutable_heartbeat_ack();
    fence_to_wire(result.fence, ack->mutable_fence());
    time_to_timestamp(result.received_at, ack->mutable_received_at());
    ack->set_heartbeat_interval_seconds(
        static_cast<std::uint32_t>(result.heartbeat_interval.count()));
    ack->set_resync_required(resync_required);
    return stream->Write(response);
  }

  void send_failure(grpc::ServerReaderWriter<v1::RuntimeToHost, v1::HostToRuntime>* stream,
                    const t::Failure& rejected, const t::HostFence& fence,
                    std::string_view request_id, std::string_view trace_id) {
    v1::RuntimeToHost frame;
    fence_to_wire(fence, frame.mutable_fence());
    set_error(rejected, request_id, trace_id, &frame);
    (void)stream->Write(frame);
  }

  HostBridgeServerDependencies dependencies_;
  std::mutex mutex_;
};

std::unique_ptr<v1::HostBridge::Service> make_impl(HostBridgeServerDependencies dependencies) {
  return std::make_unique<HostBridgeImpl>(std::move(dependencies));
}

// In-process fallback store for deployment composition before P08b-2c supplies the PG port.
// It preserves the same version/generation semantics; it is explicitly not durable.
class InMemoryHostStore final : public t::HostRegistrationPort {
public:
  explicit InMemoryHostStore(std::vector<t::HostCertificateBinding> allowlist)
      : allowlist_(std::move(allowlist)) {}

  t::Outcome<std::optional<t::HostCertificateBinding>>
  certificate_binding(std::string_view certificate_sha256) override {
    for (const auto& entry : allowlist_) {
      if (entry.certificate_sha256 == certificate_sha256) {
        return std::optional<t::HostCertificateBinding>{entry};
      }
    }
    return std::optional<t::HostCertificateBinding>{};
  }
  t::Outcome<std::optional<t::HostRegistration>> find_host(const t::ResourceId& host) override {
    const auto found = hosts_.find(host.value);
    if (found == hosts_.end()) {
      return std::optional<t::HostRegistration>{};
    }
    return std::optional<t::HostRegistration>{found->second};
  }
  t::Status save_host(const t::HostRegistration& registration,
                      std::optional<t::ResourceVersion> expected) override {
    const auto found = hosts_.find(registration.host.value);
    if (expected) {
      if (found == hosts_.end() || found->second.version != *expected) {
        return conflict();
      }
      auto stored = registration;
      stored.version.value = expected->value + 1;
      hosts_[stored.host.value] = stored;
      return std::monostate{};
    }
    if (found != hosts_.end()) {
      return conflict();
    }
    auto stored = registration;
    stored.version.value = 1;
    hosts_[stored.host.value] = stored;
    return std::monostate{};
  }
  t::Outcome<std::optional<t::HostConnectionState>> connection(const t::ResourceId& host) override {
    const auto found = connections_.find(host.value);
    if (found == connections_.end()) {
      return std::optional<t::HostConnectionState>{};
    }
    return std::optional<t::HostConnectionState>{found->second};
  }
  t::Outcome<t::HostConnectionState>
  activate_connection(const t::HostConnectionState& connection) override {
    auto stored = connection;
    stored.fence.connection_generation = ++generation_;
    connections_[stored.fence.host.value] = stored;
    return stored;
  }
  t::Status save_connection(const t::HostConnectionState& connection,
                            std::uint64_t expected_generation) override {
    const auto found = connections_.find(connection.fence.host.value);
    if (found == connections_.end() ||
        found->second.fence.connection_generation != expected_generation) {
      return conflict();
    }
    connections_[connection.fence.host.value] = connection;
    return std::monostate{};
  }

private:
  std::vector<t::HostCertificateBinding> allowlist_;
  std::map<std::string, t::HostRegistration> hosts_;
  std::map<std::string, t::HostConnectionState> connections_;
  std::uint64_t generation_ = 0;
};

class DeploymentHostClock final : public t::HostClockPort {
public:
  explicit DeploymentHostClock(t::RuntimeIdPort& ids) : ids_(ids) {}
  t::Outcome<t::TimePoint> now() override {
    return t::TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())};
  }
  t::Outcome<std::string> runtime_epoch() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (epoch_.empty()) {
      auto generated = ids_.runtime_epoch();
      if (const auto* rejected = std::get_if<t::Failure>(&generated)) {
        return *rejected;
      }
      epoch_ = std::get<t::RuntimeEpoch>(generated).value;
    }
    return epoch_;
  }
  t::Outcome<std::uint64_t> next_connection_generation() override {
    return generation_.fetch_add(1) + 1;
  }

private:
  t::RuntimeIdPort& ids_;
  std::mutex mutex_;
  std::string epoch_;
  std::atomic<std::uint64_t> generation_{0};
};

t::Outcome<std::string> default_peer_certificate(grpc::ServerContext& context) {
  auto extracted = extract_peer_certificate(*context.auth_context());
  if (const auto* rejected = std::get_if<t::Failure>(&extracted)) {
    return *rejected;
  }
  return hex_digest(std::get<PeerCertificateEvidence>(extracted).leaf_fingerprint);
}

} // namespace

t::Outcome<std::unique_ptr<v1::HostBridge::Service>>
make_host_bridge(HostBridgeServerDependencies dependencies) {
  if (!dependencies.peer_certificate) {
    dependencies.peer_certificate = default_peer_certificate;
  }
  try {
    return make_impl(std::move(dependencies));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

t::Outcome<std::unique_ptr<v1::HostBridge::Service>>
make_deployment_host_bridge(t::RuntimeIdPort& ids, std::vector<t::HostCertificateBinding> allowlist,
                            HostBridgeCallbacks callbacks) {
  try {
    // Keep the in-memory ports alive for the lifetime of the service, which borrows them.
    class OwnedBridge final : public v1::HostBridge::Service {
    public:
      OwnedBridge(std::unique_ptr<t::HostRegistrationPort> store,
                  std::unique_ptr<t::HostClockPort> clock, HostBridgeCallbacks callbacks)
          : store_(std::move(store)), clock_(std::move(clock)),
            service_(std::make_unique<HostBridgeImpl>(HostBridgeServerDependencies{
                *store_, *clock_, default_peer_certificate, std::move(callbacks)})) {}
      grpc::Status
      Connect(grpc::ServerContext* context,
              grpc::ServerReaderWriter<v1::RuntimeToHost, v1::HostToRuntime>* stream) override {
        return service_->Connect(context, stream);
      }

    private:
      std::unique_ptr<t::HostRegistrationPort> store_;
      std::unique_ptr<t::HostClockPort> clock_;
      std::unique_ptr<HostBridgeImpl> service_;
    };
    return std::unique_ptr<v1::HostBridge::Service>(std::make_unique<OwnedBridge>(
        std::make_unique<InMemoryHostStore>(std::move(allowlist)),
        std::make_unique<DeploymentHostClock>(ids), std::move(callbacks)));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
