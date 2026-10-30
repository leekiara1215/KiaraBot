#include "kiarabot/core/plugin_config_delivery.hpp"

#include "kiarabot/core/control_service.hpp"

namespace kiarabot::core {
namespace {
namespace wire = kiarabot::v1;
wire::Error error(wire::ErrorCode code, const std::string& id = {}, bool retryable = false) {
  wire::Error result;
  result.set_code(code);
  result.set_message(wire::ErrorCode_Name(code));
  result.set_request_id(id);
  result.set_retryable(retryable);
  return result;
}
} // namespace
PluginConfigDelivery::PluginConfigDelivery(RuntimeStore& store, std::string encryption_key,
                                           RuntimeClock clock)
    : store_(store), key_(std::move(encryption_key)), clock_(std::move(clock)) {
  if (!clock_) {
    clock_ = [] { return std::chrono::system_clock::now(); };
  }
}
void PluginConfigDelivery::attach(const std::string& identity, const std::string& fence) {
  std::lock_guard lock(mutex_);
  active_[identity] = Delivery{fence, {}, {"PENDING", {}, {}}, {}, {}, {}};
}
void PluginConfigDelivery::detach(const std::string& identity, const std::string& fence) {
  std::lock_guard lock(mutex_);
  const auto found = active_.find(identity);
  if (found != active_.end() && found->second.fence == fence) {
    active_.erase(found);
  }
}
void PluginConfigDelivery::expire(Delivery& delivery) {
  if (!delivery.frame.delivery_id().empty() && delivery.status.state == "PENDING" &&
      clock_() >= delivery.deadline) {
    delivery.status.state = "REJECTED";
    delivery.status.error = error(wire::TIMEOUT, delivery.frame.delivery_id(), true);
  }
}
PortResult<std::optional<wire::PluginConfig>>
PluginConfigDelivery::next(const std::string& identity, const std::string& fence) try {
  std::lock_guard lock(mutex_);
  const auto found = active_.find(identity);
  if (found == active_.end() || found->second.fence != fence) {
    return error(wire::PERMISSION_DENIED);
  }
  auto& delivery = found->second;
  expire(delivery);
  if (!delivery.frame.delivery_id().empty() && delivery.status.state == "PENDING") {
    return std::optional<wire::PluginConfig>{};
  }
  const auto read = store_.read();
  if (const auto* failure = std::get_if<StorageError>(&read)) {
    return error(*failure == StorageError::Corrupt ? wire::INTERNAL : wire::UPSTREAM_UNAVAILABLE,
                 {}, *failure != StorageError::Corrupt);
  }
  const auto& snapshot = std::get<RuntimeSnapshot>(read);
  if (!control_plugin_enabled(snapshot, identity)) {
    return error(wire::PLUGIN_UNAVAILABLE);
  }
  ControlJson desired{{"version", "0"}, {"values", ControlJson::object()}};
  const auto state = snapshot.namespaces.find("$control");
  if (state != snapshot.namespaces.end() && state->second.contains("state")) {
    const auto document =
        ControlJson::parse(control_decrypt(state->second.at("state").value, key_));
    desired = document.at("configs").value(identity, desired);
  }
  if (desired.at("version") == delivery.frame.version()) {
    return std::optional<wire::PluginConfig>{};
  }
  delivery.retired_id = delivery.frame.delivery_id();
  delivery.retired_version = delivery.frame.version();
  delivery.frame.Clear();
  delivery.frame.set_version(desired.at("version").get<std::string>());
  *delivery.frame.mutable_values() = control_struct(desired.at("values"));
  delivery.frame.set_delivery_id(control_secret());
  delivery.frame.set_apply_timeout_ms(5000);
  delivery.status.state = "PENDING";
  delivery.status.error.reset();
  delivery.deadline = clock_() + std::chrono::seconds(10);
  return std::optional<wire::PluginConfig>{delivery.frame};
} catch (...) {
  return error(wire::INTERNAL);
}
PortStatus PluginConfigDelivery::acknowledge(const std::string& identity, const std::string& fence,
                                             const wire::PluginConfigAck& ack) {
  std::lock_guard lock(mutex_);
  const auto found = active_.find(identity);
  if (found == active_.end() || found->second.fence != fence) {
    return error(wire::PERMISSION_DENIED);
  }
  auto& delivery = found->second;
  if (ack.delivery_id().empty() || (ack.accepted() && ack.has_error()) ||
      (!ack.accepted() && (!ack.has_error() || !wire::ErrorCode_IsValid(ack.error().code()) ||
                           ack.error().code() == wire::ERROR_CODE_UNSPECIFIED))) {
    return error(wire::INVALID_ARGUMENT);
  }
  if (ack.delivery_id() == delivery.retired_id && ack.version() == delivery.retired_version) {
    return std::monostate{};
  }
  if (ack.delivery_id() != delivery.frame.delivery_id() ||
      ack.version() != delivery.frame.version()) {
    return error(wire::INVALID_ARGUMENT);
  }
  expire(delivery);
  if (delivery.status.state != "PENDING") {
    return std::monostate{};
  }
  delivery.status.state = ack.accepted() ? "APPLIED" : "REJECTED";
  if (ack.accepted()) {
    delivery.status.applied_version = ack.version();
    delivery.status.error.reset();
  } else {
    delivery.status.error = error(ack.error().code(), ack.delivery_id(), ack.error().retryable());
  }
  return std::monostate{};
}
ConfigDeliveryStatus PluginConfigDelivery::status(const std::string& identity,
                                                  const std::string& desired_version) {
  std::lock_guard lock(mutex_);
  const auto found = active_.find(identity);
  if (found == active_.end()) {
    return {};
  }
  auto& delivery = found->second;
  expire(delivery);
  auto result = delivery.status;
  if (delivery.frame.version() != desired_version) {
    result.state = "PENDING";
    result.error.reset();
  }
  return result;
}
bool PluginConfigDelivery::ready(const std::string& identity, const std::string& fence) {
  std::lock_guard lock(mutex_);
  const auto found = active_.find(identity);
  return found != active_.end() && found->second.fence == fence &&
         found->second.status.state == "APPLIED";
}
} // namespace kiarabot::core
