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

std::optional<t::Sha256Digest> digest_from_hex(std::string_view text) {
  if (text.size() != 64) {
    return std::nullopt;
  }
  auto digit = [](char character) -> int {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
      return character - 'a' + 10;
    }
    return -1;
  };
  t::Sha256Digest digest;
  for (std::size_t index = 0; index < 32; ++index) {
    const int high = digit(text[index * 2]);
    const int low = digit(text[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    digest.bytes[index] = static_cast<std::byte>((high << 4) | low);
  }
  return digest;
}

t::ExecutionOutcome execution_outcome(v1::ExecutionOutcome outcome) {
  switch (outcome) {
  case v1::EXECUTION_KNOWN_SUCCEEDED:
    return t::ExecutionOutcome::KnownSucceeded;
  case v1::EXECUTION_KNOWN_FAILED:
    return t::ExecutionOutcome::KnownFailed;
  case v1::EXECUTION_UNKNOWN:
    return t::ExecutionOutcome::Unknown;
  default:
    return t::ExecutionOutcome::NotStarted;
  }
}

void set_handoff(const t::HostCommandOutboxRecord& command, v1::HandoffContext* handoff) {
  handoff->set_source_service("runtime");
  handoff->set_message_id(command.command_id);
  if (const auto digest = digest_from_hex(command.payload_sha256)) {
    handoff->set_payload_sha256(
        std::string(reinterpret_cast<const char*>(digest->bytes.data()), digest->bytes.size()));
  }
  handoff->mutable_created_at()->CopyFrom(google::protobuf::util::TimeUtil::GetCurrentTime());
}

bool fill_dispatch_frame(const t::HostCommandOutboxRecord& command, v1::RuntimeToHost* frame,
                         const t::HostFence& fence) {
  if (command.host_id != fence.host.value || command.command_id.empty()) {
    return false;
  }
  fence_to_wire(fence, frame->mutable_fence());
  switch (command.kind) {
  case t::HostCommandKind::ApplyDesiredProcess: {
    auto* apply = frame->mutable_apply();
    set_handoff(command, apply->mutable_handoff());
    apply->set_command_id(command.command_id);
    auto* process = apply->mutable_process();
    process->set_instance_id(command.instance_id);
    process->set_installation_id(command.installation_id);
    process->set_execution_generation(command.execution_generation);
    process->set_desired_version(command.desired_version);
    process->set_desired_state(command.desired_state == t::DesiredState::Running
                                   ? v1::DESIRED_RUNNING
                                   : v1::DESIRED_STOPPED);
    if (const auto digest = digest_from_hex(command.package_sha256)) {
      process->set_package_sha256(
          std::string(reinterpret_cast<const char*>(digest->bytes.data()), digest->bytes.size()));
    }
    process->set_launch_profile(command.launch_profile);
    for (const auto& argument : command.arguments) {
      process->add_arguments(argument);
    }
    for (const auto& name : command.environment_names) {
      process->add_environment_names(name);
    }
    return true;
  }
  case t::HostCommandKind::StopExecution: {
    auto* stop = frame->mutable_stop();
    set_handoff(command, stop->mutable_handoff());
    stop->set_command_id(command.command_id);
    stop->set_host_id(command.host_id);
    stop->set_instance_id(command.instance_id);
    stop->set_execution_generation(command.execution_generation);
    stop->set_grace_ms(5000);
    return true;
  }
  case t::HostCommandKind::DrainHost: {
    auto* drain = frame->mutable_drain();
    set_handoff(command, drain->mutable_handoff());
    drain->set_command_id(command.command_id);
    drain->set_host_id(command.host_id);
    drain->set_grace_ms(5000);
    return true;
  }
  }
  return false;
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
    // The accepted fence is the live coordination view for Runtime calls; it is removed when this
    // stream ends so a stale connection can never satisfy a later admission-gate check.
    struct LiveFence {
      t::HostFenceRegistry* registry = nullptr;
      t::HostFence fence;
      ~LiveFence() {
        if (registry != nullptr) {
          registry->erase(fence);
        }
      }
    } live_fence{dependencies_.fences, fence};
    if (dependencies_.fences != nullptr) {
      dependencies_.fences->record(fence);
    }
    if (!stream->Write(response)) {
      return grpc::Status::OK;
    }
    if (!dispatch_pending(stream, fence)) {
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
        if (!dispatch_pending(stream, fence)) {
          return grpc::Status::OK;
        }
        continue;
      }
      auto handled = handle_report(frame, fence);
      if (const auto* rejected = std::get_if<t::Failure>(&handled)) {
        send_failure(stream, *rejected, fence, frame_request_id, frame_trace_id);
        return grpc::Status::OK;
      }
    }
    return grpc::Status::OK;
  }

private:
  t::Status handle_report(const v1::HostToRuntime& frame, const t::HostFence& fence) {
    // A superseded stream may still be open and present its own formerly valid fence.
    // Serialize the current-fence check and report persistence with registration.
    std::lock_guard<std::mutex> lock(mutex_);
    auto current = dependencies_.registration.connection(fence.host);
    if (const auto* rejected = std::get_if<t::Failure>(&current)) {
      return *rejected;
    }
    const auto& connection = std::get<std::optional<t::HostConnectionState>>(current);
    if (!connection || connection->fence != fence) {
      return conflict();
    }
    const auto body = frame.body_case();
    if (body == v1::HostToRuntime::kInventory) {
      if (frame.inventory().host_id() != fence.host.value) {
        return conflict();
      }
      if (dependencies_.callbacks.inventory) {
        return dependencies_.callbacks.inventory(frame.inventory());
      }
      return std::monostate{};
    }
    if (body == v1::HostToRuntime::kReceipt) {
      const auto& receipt = frame.receipt();
      if (!valid_identifier(receipt.command_id())) {
        return invalid_argument();
      }
      if (dependencies_.persistence.receipt) {
        std::optional<t::ExecutionOutcome> outcome;
        std::optional<std::string> marker;
        if (receipt.has_execution()) {
          outcome = execution_outcome(receipt.execution().outcome());
          if (!receipt.execution().process_marker().empty()) {
            marker = receipt.execution().process_marker();
          }
        }
        const auto persisted =
            dependencies_.persistence.receipt(receipt.command_id(), fence.connection_generation,
                                              receipt.persisted(), outcome, marker);
        if (const auto* rejected = std::get_if<t::Failure>(&persisted)) {
          return *rejected;
        }
      }
      if (dependencies_.callbacks.receipt) {
        return dependencies_.callbacks.receipt(receipt);
      }
      return std::monostate{};
    }
    if (body == v1::HostToRuntime::kObservation) {
      const auto& observation = frame.observation();
      if (!observation.has_execution() || observation.execution().host_id() != fence.host.value ||
          !valid_identifier(observation.execution().instance_id()) || !observation.has_cursor() ||
          observation.cursor().sequence() == 0 ||
          observation.cursor().epoch() != fence.runtime_epoch ||
          observation.execution().execution_generation() == 0) {
        return invalid_argument();
      }
      if (dependencies_.persistence.observation) {
        t::HostObservationRecord record;
        record.host_id = fence.host.value;
        record.connection_generation = fence.connection_generation;
        record.sequence = observation.cursor().sequence();
        record.instance_id = observation.execution().instance_id();
        record.execution_generation = observation.execution().execution_generation();
        record.outcome = execution_outcome(observation.execution().outcome());
        record.process_marker = observation.execution().process_marker();
        record.command_id = observation.execution().command_id();
        t::TimePoint observed_at;
        if (!timestamp_to_time(observation.execution().observed_at(), &observed_at)) {
          return invalid_argument();
        }
        record.observed_at = observed_at;
        const auto stored = dependencies_.persistence.observation(record);
        if (const auto* rejected = std::get_if<t::Failure>(&stored)) {
          return *rejected;
        }
      }
      if (dependencies_.callbacks.observation) {
        return dependencies_.callbacks.observation(observation);
      }
      return std::monostate{};
    }
    return invalid_argument();
  }

  bool dispatch_pending(grpc::ServerReaderWriter<v1::RuntimeToHost, v1::HostToRuntime>* stream,
                        const t::HostFence& fence) {
    if (!dependencies_.persistence.pending) {
      return true;
    }
    auto pending =
        dependencies_.persistence.pending(fence.host.value, fence.connection_generation, 20);
    if (const auto* rejected = std::get_if<t::Failure>(&pending)) {
      send_failure(stream, *rejected, fence, {}, {});
      return false;
    }
    for (const auto& command : std::get<std::vector<t::HostCommandOutboxRecord>>(pending)) {
      v1::RuntimeToHost frame;
      if (!fill_dispatch_frame(command, &frame, fence)) {
        send_failure(stream, invalid_argument(), fence, {}, {});
        return false;
      }
      if (!stream->Write(frame)) {
        return false;
      }
      if (dependencies_.persistence.dispatched) {
        const auto marked = dependencies_.persistence.dispatched(
            command.command_id, fence.connection_generation, fence.runtime_epoch);
        if (const auto* rejected = std::get_if<t::Failure>(&marked)) {
          send_failure(stream, *rejected, fence, {}, {});
          return false;
        }
      }
    }
    return true;
  }

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

// In-process store used by the current composition. The PG HostRegistrationPort and its real
// PG fixture landed in P08b-2c, but runtime_composition still injects this store for registration
// (state is intentionally not durable across restarts). Command/receipt/observation persistence is
// wired separately through HostBridgePersistence (P08b-3a/3b), so that path is durable.
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
                            HostBridgeCallbacks callbacks, HostBridgePersistence persistence,
                            t::HostFenceRegistry* fences) {
  try {
    // Keep the in-memory ports alive for the lifetime of the service, which borrows them.
    class OwnedBridge final : public v1::HostBridge::Service {
    public:
      OwnedBridge(std::unique_ptr<t::HostRegistrationPort> store,
                  std::unique_ptr<t::HostClockPort> clock, HostBridgeCallbacks callbacks,
                  HostBridgePersistence persistence, t::HostFenceRegistry* fences)
          : store_(std::move(store)), clock_(std::move(clock)),
            service_(std::make_unique<HostBridgeImpl>(HostBridgeServerDependencies{
                *store_, *clock_, default_peer_certificate, std::move(callbacks),
                std::move(persistence), fences})) {}
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
    return std::unique_ptr<v1::HostBridge::Service>(
        std::make_unique<OwnedBridge>(std::make_unique<InMemoryHostStore>(std::move(allowlist)),
                                      std::make_unique<DeploymentHostClock>(ids),
                                      std::move(callbacks), std::move(persistence), fences));
  } catch (const std::bad_alloc&) {
    return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
  } catch (...) {
    return failure(t::ErrorCode::Internal);
  }
}

} // namespace kiarabot::core::platform
