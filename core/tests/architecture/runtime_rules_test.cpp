#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/runtime_rules.hpp"

namespace target = kiarabot::core::target;
using std::chrono_literals::operator""s;

namespace {
constexpr auto kAccount = "11111111-1111-4111-8111-111111111111";
constexpr auto kAdapter = "22222222-2222-4222-8222-222222222222";
constexpr auto kInstallation = "33333333-3333-4333-8333-333333333333";
constexpr auto kInstance = "44444444-4444-4444-8444-444444444444";
constexpr auto kHost = "55555555-5555-4555-8555-555555555555";
const target::TimePoint kNow{std::chrono::seconds{1000}};

void check(bool passed, const char* scenario) {
  if (!passed) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
bool failed(const target::Status& status, target::ErrorCode code) {
  const auto* failure = std::get_if<target::Failure>(&status);
  return failure != nullptr && failure->code == code;
}
template <typename Value> bool outcome_failed(const target::Outcome<Value>& result) {
  return std::holds_alternative<target::Failure>(result);
}
target::AdapterRegistrationMetadata metadata() {
  target::AdapterRegistrationMetadata value;
  value.implementation = "nonebot";
  value.implementation_version = "1.0.0";
  value.protocol_version = "kiarabot.v1";
  value.declared_capabilities = {{"action.send_message.text.v1", "1"}};
  return value;
}
target::AdapterRegistrationRequest first_registration() {
  return {{kAdapter},
          metadata(),
          std::nullopt,
          target::RuntimeEpoch{"epoch-1"},
          target::InstanceId{"instance-1"},
          target::ConnectionId{"connection-1"},
          kNow};
}
target::AdapterRecord accepted_adapter() {
  auto accepted = target::accept_adapter_registration(std::nullopt, first_registration());
  check(!outcome_failed(accepted), "first registration accepted");
  return std::get<target::AdapterRecord>(accepted);
}
void account_rules() {
  check(!failed(target::validate_account_create({kAccount}, "qq", "10001", "Bot"),
                target::ErrorCode::InvalidArgument),
        "valid account accepted");
  check(failed(target::validate_account_create({std::string{36, 'x'}}, "qq", "10001", "Bot"),
               target::ErrorCode::InvalidArgument),
        "non canonical id rejected");
  check(failed(target::validate_account_create({kAccount}, "q q", "10001", "Bot"),
               target::ErrorCode::InvalidArgument),
        "malformed platform rejected");
  check(failed(target::validate_account_create({kAccount}, "qq", "10001", ""),
               target::ErrorCode::InvalidArgument),
        "empty display name rejected");
  const target::BotAccount account{{kAccount}, "qq", "10001", "Bot", {3}};
  check(!failed(target::validate_account_update(account, "Renamed", target::ResourceVersion{3}),
                target::ErrorCode::Conflict),
        "current version update accepted");
  check(failed(target::validate_account_update(account, "Renamed", target::ResourceVersion{2}),
               target::ErrorCode::Conflict),
        "stale expected version conflicts");
  check(failed(target::validate_account_update(account, "Renamed", std::nullopt),
               target::ErrorCode::InvalidArgument),
        "missing expected version rejected");
  check(!failed(target::validate_account_disable(account, target::ResourceVersion{3}, false),
                target::ErrorCode::Conflict),
        "disable without binding accepted");
  check(failed(target::validate_account_disable(account, target::ResourceVersion{3}, true),
               target::ErrorCode::Conflict),
        "disable with active binding conflicts");
  auto disabled = account;
  disabled.state = target::LifecycleState::Disabled;
  check(failed(target::validate_account_disable(disabled, target::ResourceVersion{3}, false),
               target::ErrorCode::Conflict),
        "second disable conflicts");
}
void capability_rules() {
  const std::vector<target::Capability> declared{{"action.send_message.text.v1", "1"},
                                                 {"action.react_message.v1", "1"}};
  check(
      !failed(target::validate_declared_capabilities(declared), target::ErrorCode::InvalidArgument),
      "known capabilities accepted");
  check(failed(target::validate_declared_capabilities({{"action.unknown.v1", "1"}}),
               target::ErrorCode::CapabilityNotSupported),
        "unknown capability rejected");
  check(failed(target::validate_declared_capabilities(
                   {{"action.react_message.v1", "1"}, {"action.react_message.v1", "1"}}),
               target::ErrorCode::InvalidArgument),
        "duplicate capability rejected");
  const std::vector<std::string> supported{"action.react_message.v1"};
  auto negotiated = target::negotiate_capabilities(declared, supported);
  check(!outcome_failed(negotiated), "negotiation accepted");
  const auto& effective = std::get<std::vector<target::Capability>>(negotiated);
  check(effective.size() == 1 && effective.front().name == "action.react_message.v1",
        "effective capabilities are the supported intersection only");
}
void adapter_rules() {
  const auto first = accepted_adapter();
  check(target::valid_adapter_fence(first.current) &&
            first.current.connection_generation.value == 1 &&
            first.current.runtime_epoch.value == "epoch-1" &&
            first.current.instance.value == "instance-1" && first.version.value == 1,
        "first registration establishes a connection-only fence");
  check(target::adapter_online(first, kNow), "current registration is online");
  check(!target::adapter_online(first, kNow + 91s), "missing heartbeat expires online window");
  auto reconnect = first_registration();
  reconnect.presented_instance = target::InstanceId{"instance-1"};
  reconnect.allocated_connection = target::ConnectionId{"connection-2"};
  const auto accepted = target::accept_adapter_registration(first, reconnect);
  check(!outcome_failed(accepted), "reconnect with current instance accepted");
  const auto& second = std::get<target::AdapterRecord>(accepted);
  check(second.current.instance.value == "instance-1" &&
            second.current.connection_generation.value == 2 &&
            second.current.runtime_epoch.value == "epoch-1" && second.version.value == 2,
        "reconnect bumps only the connection generation and preserves the logical instance");
  auto stale = reconnect;
  stale.presented_instance = target::InstanceId{"stale-instance"};
  check(std::holds_alternative<target::Failure>(target::accept_adapter_registration(second, stale)),
        "presented stale instance rejected");
  auto unknown_capability = first_registration();
  unknown_capability.metadata.declared_capabilities = {{"action.unknown.v1", "1"}};
  check(std::holds_alternative<target::Failure>(
            target::accept_adapter_registration(first, unknown_capability)),
        "registration with undeclared capability rejected");
  const target::AdapterFenceClaim current_claim{{"instance-1"}, {"epoch-1"}, {2}};
  check(
      !failed(target::validate_heartbeat(second, current_claim, kNow), target::ErrorCode::Conflict),
      "current connection claim heartbeat accepted");
  auto stale_claim = current_claim;
  stale_claim.connection_generation = target::ConnectionGeneration{1};
  check(failed(target::validate_heartbeat(second, stale_claim, kNow), target::ErrorCode::Conflict),
        "old connection generation heartbeat rejected");
  auto old_epoch = current_claim;
  old_epoch.runtime_epoch = target::RuntimeEpoch{"epoch-0"};
  check(failed(target::validate_heartbeat(second, old_epoch, kNow),
               target::ErrorCode::Unauthenticated),
        "pre-restart runtime epoch rejected as unauthenticated");
  auto offline = first;
  offline.online_until = kNow - 1s;
  check(!target::adapter_online(offline, kNow), "expired window reports offline");
}
void binding_rules() {
  const auto adapter = accepted_adapter();
  const target::BotAccount account{{kAccount}, "qq", "10001", "Bot", {2}};
  check(!failed(target::validate_bind(account, adapter, std::nullopt), target::ErrorCode::Conflict),
        "bind active account to current adapter accepted");
  const target::AdapterBindingRecord binding{{kAccount}, {"instance-1"}, {1}};
  check(failed(target::validate_bind(account, adapter, binding), target::ErrorCode::Conflict),
        "duplicate bind conflicts");
  check(failed(target::validate_unbind(binding, std::nullopt), target::ErrorCode::InvalidArgument),
        "unbind requires expected version");
  check(!failed(target::validate_unbind(binding, target::ResourceVersion{1}),
                target::ErrorCode::Conflict),
        "unbind current binding accepted");
  auto disabled = binding;
  disabled.state = target::LifecycleState::Disabled;
  check(failed(target::validate_unbind(disabled, target::ResourceVersion{1}),
               target::ErrorCode::Conflict),
        "second unbind conflicts");
}
void instance_rules() {
  const target::PluginInstallation installation{{kInstallation}, {"plugin-type"}, {1}};
  check(!failed(target::validate_instance_create({kInstance}, installation,
                                                 target::ResourceId{kHost}, false),
                target::ErrorCode::Conflict),
        "managed instance with host accepted");
  check(failed(target::validate_instance_create({kInstance}, installation, std::nullopt, false),
               target::ErrorCode::InvalidArgument),
        "managed instance without host rejected");
  check(!failed(target::validate_instance_create({kInstance}, installation, std::nullopt, true),
                target::ErrorCode::Conflict),
        "external instance without host accepted");
  auto disabled = installation;
  disabled.state = target::LifecycleState::Disabled;
  check(failed(target::validate_instance_create({kInstance}, disabled, target::ResourceId{kHost},
                                                false),
               target::ErrorCode::Conflict),
        "disabled installation rejects new instances");
  target::PluginInstance instance{{kInstance},
                                  {kInstallation},
                                  {"plugin-type"},
                                  {1},
                                  {0},
                                  std::nullopt,
                                  {4},
                                  target::LifecycleState::Active,
                                  target::ResourceId{kHost},
                                  target::DesiredState::Running};
  auto changed = target::advance_desired_state(instance, target::DesiredState::Stopped,
                                               target::ResourceVersion{4});
  check(!outcome_failed(changed), "conditional desired state accepted");
  const auto& updated = std::get<target::PluginInstance>(changed);
  check(updated.desired == target::DesiredState::Stopped && updated.version.value == 5,
        "desired state and version advance");
  check(std::holds_alternative<target::Failure>(target::advance_desired_state(
            updated, target::DesiredState::Running, target::ResourceVersion{4})),
        "stale desired state version conflicts");
  auto config = target::save_desired_config(updated, target::ResourceVersion{5});
  check(!outcome_failed(config), "conditional config save accepted");
  check(std::get<target::PluginInstance>(config).desired_version.value == 2,
        "config save advances only the desired config version");
  check(std::holds_alternative<std::monostate>(target::validate_config_update(updated, 16)),
        "bounded config accepted");
  check(failed(target::validate_config_update(updated, 0), target::ErrorCode::InvalidArgument),
        "empty config rejected");
}
} // namespace

int main() {
  account_rules();
  capability_rules();
  adapter_rules();
  binding_rules();
  instance_rules();
  return 0;
}
