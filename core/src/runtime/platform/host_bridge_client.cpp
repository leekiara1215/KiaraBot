#include "kiarabot/core/runtime/platform/host_bridge_client.hpp"

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
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  {
    std::lock_guard lock(mutex);
    high = generator();
    low = generator();
  }
  high = (high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
  low = (low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text(36, '-');
  const std::uint64_t parts[2] = {high, low};
  std::size_t output = 0;
  for (int index = 15; index >= 0; --index) {
    const auto byte = static_cast<unsigned char>((parts[index / 8] >> ((index % 8) * 8)) & 0xff);
    if (output == 8 || output == 13 || output == 18 || output == 23) {
      ++output;
    }
    text[output++] = kHex[byte >> 4];
    text[output++] = kHex[byte & 0x0f];
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
  record.mutable_observed_at()->CopyFrom(
      google::protobuf::util::TimeUtil::NanosecondsToTimestamp(
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
    if (started_) {
      return t::Status{std::monostate{}};
    }
    if (config_.endpoint.empty() || !config_.credentials) {
      return failure(t::ErrorCode::InvalidArgument);
    }
    handler_ = &handler;
    channel_ = grpc::CreateChannel(config_.endpoint, config_.credentials);
    if (!channel_->WaitForConnected(std::chrono::system_clock::now() +
                                    config_.registration_timeout)) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    context_ = std::make_unique<grpc::ClientContext>();
    stub_ = v1::HostBridge::NewStub(channel_);
    stream_ = stub_->Connect(context_.get());
    if (!stream_) {
      return failure(t::ErrorCode::Internal);
    }
    v1::HostToRuntime registration;
    set_context(*registration.mutable_context(), config_.registration_timeout);
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
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    v1::RuntimeToHost response;
    if (!stream_->Read(&response) ||
        response.body_case() != v1::RuntimeToHost::kRegistered) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    if (!response.registered().has_fence()) {
      return failure(t::ErrorCode::UpstreamUnavailable, t::Recovery::Backoff);
    }
    fence_ = response.registered().fence();
    if (fence_.instance_id() != config_.identity.host_id || fence_.runtime_epoch().empty() ||
        !fence_.has_connection_generation() || fence_.connection_generation() == 0 ||
        fence_.has_execution_generation()) {
      return failure(t::ErrorCode::Unauthenticated, t::Recovery::Authenticate);
    }
    {
      std::lock_guard lock(mutex_);
      registered_ = true;
    }
    started_ = true;
    handler_->bridge_ready();
    reader_ = std::thread([this] { read_loop(); });
    writer_ = std::thread([this] { write_loop(); });
    heartbeat_ = std::thread([this] { heartbeat_loop(); });
    return std::monostate{};
  }

  void stop() {
    if (!started_.exchange(false)) {
      return;
    }
    {
      std::lock_guard lock(mutex_);
      registered_ = false;
      stopping_ = true;
    }
    outgoing_cv_.notify_all();
    if (context_) {
      context_->TryCancel();
    }
    if (reader_.joinable()) {
      reader_.join();
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

private:
  bool enqueue(v1::HostToRuntime frame) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || !registered_ || outgoing_.size() >= 1024) {
        return false;
      }
      outgoing_.push_back(std::move(frame));
    }
    outgoing_cv_.notify_one();
    return true;
  }

  void write_loop() {
    for (;;) {
      v1::HostToRuntime frame;
      {
        std::unique_lock lock(mutex_);
        outgoing_cv_.wait(lock, [this] { return stopping_ || !outgoing_.empty(); });
        if (stopping_ && outgoing_.empty()) {
          return;
        }
        frame = std::move(outgoing_.front());
        outgoing_.pop_front();
      }
      if (!stream_->Write(frame)) {
        mark_connection_lost();
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
        mark_connection_lost();
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
        mark_connection_lost();
        return;
      }
    }
    mark_connection_lost();
  }

  void heartbeat_loop() {
    while (registered()) {
      std::this_thread::sleep_for(config_.heartbeat_interval);
      if (!registered()) {
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
    const auto result =
        handler_->stop_execution(request.instance_id(), request.execution_generation(),
                                 request.grace_ms(), request.command_id(),
                                 payload_hash(request.handoff()));
    send_handler_result(request.command_id(), result);
  }

  void drain(const v1::HostDrainHostRequest& request) {
    const auto result = handler_->drain_host(request.grace_ms(), request.command_id(),
                                             payload_hash(request.handoff()));
    send_handler_result(request.command_id(), result);
  }

  void send_handler_result(std::string_view command_id,
                           const t::Outcome<t::HostReceipt>& result) {
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

  void mark_connection_lost() {
    {
      std::lock_guard lock(mutex_);
      registered_ = false;
      stopping_ = true;
    }
    outgoing_cv_.notify_all();
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
  std::atomic<std::uint64_t> delivered_{0};
  mutable std::mutex mutex_;
  std::condition_variable outgoing_cv_;
  std::deque<v1::HostToRuntime> outgoing_;
  bool stopping_ = false;
  std::thread reader_;
  std::thread writer_;
  std::thread heartbeat_;
};

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
