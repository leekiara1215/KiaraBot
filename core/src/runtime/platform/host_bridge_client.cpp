#include "kiarabot/core/runtime/platform/host_bridge_client.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include <google/protobuf/util/time_util.h>
#include <grpcpp/grpcpp.h>
#include <kiarabot/v1/host.grpc.pb.h>
#include <kiarabot/v1/host.pb.h>
#include <kiarabot/v1/platform.pb.h>
#include <kiarabot/v1/services.grpc.pb.h>

namespace kiarabot::core::platform {
namespace {

namespace t = core::target;
namespace v1 = kiarabot::v1;

t::Failure failure(t::ErrorCode code, t::Recovery recovery = t::Recovery::Diagnose) {
  return {code, t::Acceptance::NotAccepted, recovery, std::nullopt, std::nullopt};
}

std::string random_uuid() {
  static std::mutex mutex;
  static std::mt19937_64 generator{std::random_device{}()};
  std::array<unsigned char, 16> bytes{};
  {
    std::lock_guard lock(mutex);
    const auto high = generator();
    const auto low = generator();
    for (std::size_t index = 0; index < 8; ++index) {
      bytes[index] = static_cast<unsigned char>((high >> ((7 - index) * 8)) & 0xffU);
      bytes[8 + index] = static_cast<unsigned char>((low >> ((7 - index) * 8)) & 0xffU);
    }
  }
  // RFC 4122 version 4 and variant bits are part of the frozen wire identity; owners reject a
  // non-canonical request/trace id as INVALID_ARGUMENT.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text += '-';
    }
    text += kHex[bytes[index] >> 4U];
    text += kHex[bytes[index] & 0x0fU];
  }
  return text;
}

void set_context(v1::RequestContext& context, std::chrono::milliseconds timeout) {
  context.set_request_id(random_uuid());
  context.set_trace_id(random_uuid());
  context.mutable_deadline()->CopyFrom(
      google::protobuf::util::TimeUtil::GetCurrentTime() +
      google::protobuf::util::TimeUtil::MillisecondsToDuration(timeout.count()));
}

bool same_fence(const v1::Fence& left, const v1::Fence& right) {
  if (left.instance_id() != right.instance_id() || left.runtime_epoch() != right.runtime_epoch() ||
      left.connection_generation() != right.connection_generation() ||
      left.has_execution_generation() != right.has_execution_generation()) {
    return false;
  }
  return !left.has_execution_generation() ||
         left.execution_generation() == right.execution_generation();
}

v1::ExecutionOutcome to_wire(t::ExecutionOutcome outcome) {
  switch (outcome) {
  case t::ExecutionOutcome::NotStarted:
    return v1::EXECUTION_NOT_STARTED;
  case t::ExecutionOutcome::KnownSucceeded:
    return v1::EXECUTION_KNOWN_SUCCEEDED;
  case t::ExecutionOutcome::KnownFailed:
    return v1::EXECUTION_KNOWN_FAILED;
  case t::ExecutionOutcome::Unknown:
    return v1::EXECUTION_UNKNOWN;
  }
  return v1::EXECUTION_OUTCOME_UNSPECIFIED;
}

v1::ExecutionRecord to_wire(const t::ManagedExecution& execution) {
  v1::ExecutionRecord record;
  record.set_host_id(execution.host_id);
  record.set_instance_id(execution.instance_id);
  record.set_execution_generation(execution.execution_generation);
  record.set_desired_version(execution.desired_version);
  record.set_outcome(to_wire(execution.outcome));
  record.set_process_marker(execution.process_marker);
  record.set_command_id(execution.command_id);
  record.mutable_observed_at()->CopyFrom(google::protobuf::util::TimeUtil::NanosecondsToTimestamp(
      execution.observed_at.time_since_epoch().count()));
  return record;
}

std::string hex_digest(const std::string& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0x0f]);
  }
  return text;
}

std::string payload_hash(const v1::HandoffContext& handoff) {
  return hex_digest(handoff.payload_sha256());
}

} // namespace

class HostBridgeClient::Impl {
public:
  explicit Impl(HostBridgeClientConfig config) : config_(std::move(config)) {}

  ~Impl() {
    stop();
  }

  t::Status start(t::HostBridgeHandler& handler) {
    if (started_.exchange(true)) {
      return t::Status{std::monostate{}};
    }
    if (config_.endpoint.empty() || !config_.credentials || config_.identity.host_id.empty()) {
      started_.store(false);
      return failure(t::ErrorCode::InvalidArgument);
    }
    handler_ = &handler;
    channel_ = grpc::CreateChannel(config_.endpoint, config_.credentials);
    stub_ = v1::HostBridge::NewStub(channel_);
    // A Host starts even when Runtime is offline: registration is retried in the background and
    // readiness stays false until the Runtime-assigned fence is accepted. Offline never means the
    // managed processes were stopped.
    connection_ = std::thread([this] { connection_loop(); });
    return std::monostate{};
  }

  void stop() {
    if (!started_.exchange(false)) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    outgoing_cv_.notify_all();
    cancel_active_context();
    if (connection_.joinable()) {
      connection_.join();
    }
    if (writer_.joinable()) {
      writer_.join();
    }
    if (heartbeat_.joinable()) {
      heartbeat_.join();
    }
    stream_.reset();
    stub_.reset();
    context_.reset();
    channel_.reset();
  }

  bool registered() const {
    return registered_.load();
  }

  t::Status send_observation(const t::ExecutionObservation& observation) {
    if (!registered()) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    v1::HostToRuntime frame;
    set_context(*frame.mutable_context(), std::chrono::milliseconds{2000});
    frame.mutable_fence()->CopyFrom(fence_);
    auto* body = frame.mutable_observation();
    body->mutable_execution()->CopyFrom(to_wire(observation.execution));
    auto* cursor = body->mutable_cursor();
    cursor->set_source_service("host");
    cursor->set_epoch(fence_.runtime_epoch());
    cursor->set_sequence(observation.sequence);
    if (!enqueue(std::move(frame))) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    delivered_.store(std::max(delivered_.load(), observation.sequence));
    return std::monostate{};
  }

  t::Status send_receipt(const t::HostReceipt& receipt) {
    if (!registered()) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    v1::HostToRuntime frame;
    set_context(*frame.mutable_context(), std::chrono::milliseconds{2000});
    frame.mutable_fence()->CopyFrom(fence_);
    auto* body = frame.mutable_receipt();
    body->set_command_id(receipt.command_id);
    body->set_persisted(receipt.persisted);
    if (receipt.execution) {
      body->mutable_execution()->CopyFrom(to_wire(*receipt.execution));
    }
    if (!enqueue(std::move(frame))) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    return std::monostate{};
  }

  std::uint64_t delivered_sequence() const {
    return delivered_.load();
  }

  t::Outcome<t::HostFence> current_fence() const {
    std::lock_guard lock(mutex_);
    if (!registered_.load() || fence_.instance_id().empty() ||
        !fence_.has_connection_generation() || fence_.connection_generation() == 0 ||
        fence_.runtime_epoch().empty()) {
      return t::Failure{t::ErrorCode::UpstreamUnavailable, t::Acceptance::NotAccepted,
                        t::Recovery::Backoff, std::nullopt, std::nullopt};
    }
    return t::HostFence{t::ResourceId{fence_.instance_id()}, fence_.connection_generation(),
                        fence_.runtime_epoch()};
  }

private:
  bool stopping() const {
    return stopping_.load();
  }

  void cancel_active_context() {
    std::lock_guard lock(mutex_);
    if (context_) {
      context_->TryCancel();
    }
  }

  bool sleep_interruptible(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      if (stopping()) {
        return false;
      }
      std::this_thread::sleep_for(std::min(std::chrono::milliseconds{100},
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               deadline - std::chrono::steady_clock::now())));
    }
    return !stopping();
  }

  bool enqueue(v1::HostToRuntime frame) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || !session_active_ || outgoing_.size() >= 1024) {
        return false;
      }
      outgoing_.push_back(std::move(frame));
    }
    outgoing_cv_.notify_one();
    return true;
  }

  bool open_session() {
    if (stopping()) {
      return false;
    }
    const auto wait = std::min(config_.registration_timeout, std::chrono::milliseconds{1000});
    if (!channel_->WaitForConnected(std::chrono::system_clock::now() + wait)) {
      return false;
    }
    auto context = std::make_unique<grpc::ClientContext>();
    auto stream = stub_->Connect(context.get());
    if (!stream) {
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return false;
      }
      context_ = std::move(context);
      stream_ = std::move(stream);
    }
    v1::HostToRuntime registration;
    set_context(*registration.mutable_context(), wait);
    auto* host = registration.mutable_registration()->mutable_host();
    host->set_host_id(config_.identity.host_id);
    for (const auto& package : config_.identity.packages) {
      auto* wire = host->add_packages();
      wire->set_plugin_type_id(package.plugin_type_id);
      wire->set_package_sha256(package.sha256);
      wire->set_launch_profile(package.launch_profile);
      for (const auto& name : package.allowed_argument_names) {
        wire->add_allowed_argument_names(name);
      }
    }
    for (const auto& capability : config_.identity.capabilities) {
      host->add_capabilities(capability);
    }
    host->mutable_observed_at()->CopyFrom(google::protobuf::util::TimeUtil::GetCurrentTime());
    if (!stream_->Write(registration)) {
      return false;
    }
    v1::RuntimeToHost response;
    if (!stream_->Read(&response) || response.body_case() != v1::RuntimeToHost::kRegistered ||
        !response.registered().has_fence()) {
      return false;
    }
    const auto& fence = response.registered().fence();
    if (fence.instance_id() != config_.identity.host_id || fence.runtime_epoch().empty() ||
        !fence.has_connection_generation() || fence.connection_generation() == 0 ||
        fence.has_execution_generation()) {
      return false;
    }
    fence_ = fence;
    {
      std::lock_guard lock(mutex_);
      session_active_ = true;
      session_stop_ = false;
      registered_ = true;
    }
    return true;
  }

  void clear_session() {
    {
      std::lock_guard lock(mutex_);
      session_active_ = false;
      session_stop_ = true;
      registered_ = false;
      stream_.reset();
      context_.reset();
    }
    outgoing_cv_.notify_all();
  }

  void start_io() {
    writer_ = std::thread([this] { write_loop(); });
    heartbeat_ = std::thread([this] { heartbeat_loop(); });
  }

  void stop_io() {
    {
      std::lock_guard lock(mutex_);
      session_stop_ = true;
    }
    outgoing_cv_.notify_all();
    if (writer_.joinable()) {
      writer_.join();
    }
    if (heartbeat_.joinable()) {
      heartbeat_.join();
    }
  }

  void connection_loop() {
    std::chrono::milliseconds backoff{200};
    while (!stopping()) {
      if (open_session()) {
        backoff = std::chrono::milliseconds{200};
        start_io();
        handler_->bridge_ready();
        read_loop();
        stop_io();
        clear_session();
        // Drop frames that could not be written on the lost session; the durable observation
        // journal re-sends them after the next registration/resync.
        {
          std::lock_guard lock(mutex_);
          outgoing_.clear();
        }
      }
      if (stopping()) {
        break;
      }
      if (!sleep_interruptible(backoff)) {
        break;
      }
      backoff = std::min(backoff * 2, std::chrono::milliseconds{5000});
    }
    stop_io();
    if (!stopping()) {
      return;
    }
    clear_session();
  }

  void write_loop() {
    for (;;) {
      v1::HostToRuntime frame;
      {
        std::unique_lock lock(mutex_);
        outgoing_cv_.wait(lock,
                          [this] { return stopping_ || session_stop_ || !outgoing_.empty(); });
        if (stopping_ || session_stop_) {
          return;
        }
        frame = std::move(outgoing_.front());
        outgoing_.pop_front();
      }
      if (!stream_->Write(frame)) {
        cancel_active_context();
        return;
      }
    }
  }

  void read_loop() {
    v1::RuntimeToHost frame;
    while (stream_->Read(&frame)) {
      if (!registered()) {
        return;
      }
      const auto body = frame.body_case();
      if (body != v1::RuntimeToHost::kRegistered &&
          (!frame.has_fence() || !same_fence(frame.fence(), fence_))) {
        return;
      }
      if (body == v1::RuntimeToHost::kHeartbeatAck) {
        if (frame.heartbeat_ack().resync_required()) {
          handler_->bridge_resync();
        }
        continue;
      }
      if (body == v1::RuntimeToHost::kApply) {
        apply(frame.apply());
        continue;
      }
      if (body == v1::RuntimeToHost::kStop) {
        stop_execution(frame.stop());
        continue;
      }
      if (body == v1::RuntimeToHost::kDrain) {
        drain(frame.drain());
        continue;
      }
      if (body == v1::RuntimeToHost::kFailure) {
        return;
      }
    }
  }

  void heartbeat_loop() {
    const auto interval = config_.heartbeat_interval;
    const auto slice = std::chrono::milliseconds{100};
    for (;;) {
      auto waited = std::chrono::milliseconds{0};
      while (waited < interval) {
        if (stopping() || !session_active_.load()) {
          return;
        }
        std::this_thread::sleep_for(slice);
        waited += slice;
      }
      if (stopping() || !session_active_.load()) {
        return;
      }
      v1::HostToRuntime frame;
      set_context(*frame.mutable_context(), std::chrono::milliseconds{2000});
      frame.mutable_fence()->CopyFrom(fence_);
      auto* body = frame.mutable_heartbeat();
      body->mutable_fence()->CopyFrom(fence_);
      body->mutable_sent_at()->CopyFrom(google::protobuf::util::TimeUtil::GetCurrentTime());
      body->mutable_cursor()->set_source_service("host");
      body->mutable_cursor()->set_epoch(fence_.runtime_epoch());
      body->mutable_cursor()->set_sequence(delivered_.load());
      if (!enqueue(std::move(frame))) {
        return;
      }
    }
  }

  void apply(const v1::HostApplyDesiredProcessRequest& request) {
    t::HostProcessSpec spec;
    spec.instance_id = request.process().instance_id();
    spec.installation_id = request.process().installation_id();
    spec.execution_generation = request.process().execution_generation();
    spec.desired_version = request.process().desired_version();
    spec.desired_state = request.process().desired_state() == v1::DESIRED_STOPPED
                             ? t::DesiredState::Stopped
                             : t::DesiredState::Running;
    spec.package_sha256 = hex_digest(request.process().package_sha256());
    spec.launch_profile = request.process().launch_profile();
    for (const auto& argument : request.process().arguments()) {
      spec.arguments.push_back(argument);
    }
    for (const auto& name : request.process().environment_names()) {
      spec.environment_names.push_back(name);
    }
    const auto result = handler_->apply_desired_process(spec, request.command_id(),
                                                        payload_hash(request.handoff()));
    send_handler_result(request.command_id(), result);
  }

  void stop_execution(const v1::HostStopExecutionRequest& request) {
    const auto result = handler_->stop_execution(
        request.instance_id(), request.execution_generation(), request.grace_ms(),
        request.command_id(), payload_hash(request.handoff()));
    send_handler_result(request.command_id(), result);
  }

  void drain(const v1::HostDrainHostRequest& request) {
    const auto result = handler_->drain_host(request.grace_ms(), request.command_id(),
                                             payload_hash(request.handoff()));
    send_handler_result(request.command_id(), result);
  }

  void send_handler_result(std::string_view command_id, const t::Outcome<t::HostReceipt>& result) {
    t::HostReceipt receipt;
    if (const auto* value = std::get_if<t::HostReceipt>(&result)) {
      receipt = *value;
    } else {
      // HostCommandReceipt has no error member in the frozen P08 wire contract; a rejected
      // command is expressed as persisted=false and the Runtime records the rejection.
      receipt.command_id = std::string(command_id);
      receipt.persisted = false;
    }
    (void)send_receipt(receipt);
  }

  HostBridgeClientConfig config_;
  t::HostBridgeHandler* handler_ = nullptr;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<v1::HostBridge::Stub> stub_;
  std::unique_ptr<grpc::ClientContext> context_;
  std::unique_ptr<grpc::ClientReaderWriter<v1::HostToRuntime, v1::RuntimeToHost>> stream_;
  v1::Fence fence_;
  std::atomic<bool> registered_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> session_active_{false};
  std::atomic<std::uint64_t> delivered_{0};
  mutable std::mutex mutex_;
  std::condition_variable outgoing_cv_;
  std::deque<v1::HostToRuntime> outgoing_;
  bool session_stop_ = false;
  std::thread connection_;
  std::thread writer_;
  std::thread heartbeat_;
};

t::Outcome<t::HostFence> HostBridgeClient::current_fence() const {
  return impl_->current_fence();
}

HostBridgeClient::HostBridgeClient(HostBridgeClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
HostBridgeClient::~HostBridgeClient() = default;

t::Status HostBridgeClient::start(t::HostBridgeHandler& handler) {
  return impl_->start(handler);
}
void HostBridgeClient::stop() {
  impl_->stop();
}
bool HostBridgeClient::registered() const {
  return impl_->registered();
}
t::Status HostBridgeClient::send_observation(const t::ExecutionObservation& observation) {
  return impl_->send_observation(observation);
}
t::Status HostBridgeClient::send_receipt(const t::HostReceipt& receipt) {
  return impl_->send_receipt(receipt);
}
std::uint64_t HostBridgeClient::delivered_sequence() const {
  return impl_->delivered_sequence();
}

} // namespace kiarabot::core::platform
