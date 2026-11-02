#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/application/target/runtime_host_application.hpp"

namespace t = kiarabot::core::target;
using std::chrono_literals::operator""s;

namespace {
void check(bool value, const char* scenario) {
  if (!value) {
    std::cerr << scenario << '\n';
    std::exit(1);
  }
}
bool rejected_with(const t::Status& status, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&status);
  return failure != nullptr && failure->code == code;
}
template <typename V> bool rejected_with(const t::Outcome<V>& outcome, t::ErrorCode code) {
  const auto* failure = std::get_if<t::Failure>(&outcome);
  return failure != nullptr && failure->code == code;
}
class Store final : public t::HostRegistrationPort {
public:
  std::optional<t::HostCertificateBinding> allow;
  std::map<std::string, t::HostRegistration> hosts;
  std::map<std::string, t::HostConnectionState> connections;
  std::uint64_t generation = 0;
  t::Outcome<std::optional<t::HostCertificateBinding>>
  certificate_binding(std::string_view digest) override {
    if (allow && allow->certificate_sha256 == digest)
      return allow;
    return std::optional<t::HostCertificateBinding>{};
  }
  t::Outcome<std::optional<t::HostRegistration>> find_host(const t::ResourceId& host) override {
    auto found = hosts.find(host.value);
    if (found == hosts.end())
      return std::optional<t::HostRegistration>{};
    return std::optional<t::HostRegistration>{found->second};
  }
  t::Status save_host(const t::HostRegistration& registration,
                      std::optional<t::ResourceVersion> expected) override {
    t::HostRegistration stored = registration;
    auto found = hosts.find(stored.host.value);
    if (expected) {
      if (found == hosts.end() || found->second.version != *expected)
        return {t::Failure{
            t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}}};
      ++stored.version.value;
    } else {
      if (found != hosts.end())
        return {t::Failure{
            t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}}};
    }
    if (!expected)
      stored.version.value = 1;
    hosts[stored.host.value] = stored;
    return std::monostate{};
  }
  t::Outcome<std::optional<t::HostConnectionState>> connection(const t::ResourceId& host) override {
    auto found = connections.find(host.value);
    if (found == connections.end())
      return std::optional<t::HostConnectionState>{};
    return std::optional<t::HostConnectionState>{found->second};
  }
  t::Outcome<t::HostConnectionState>
  activate_connection(const t::HostConnectionState& connection) override {
    auto stored = connection;
    stored.fence.connection_generation = ++generation;
    connections[stored.fence.host.value] = stored;
    return stored;
  }
  t::Status save_connection(const t::HostConnectionState& connection,
                            std::uint64_t expected) override {
    auto found = connections.find(connection.fence.host.value);
    if (found == connections.end() || found->second.fence.connection_generation != expected) {
      return {t::Failure{
          t::ErrorCode::Conflict, t::Acceptance::NotAccepted, t::Recovery::ReadCurrent, {}, {}}};
    }
    connections[connection.fence.host.value] = connection;
    return std::monostate{};
  }
};
class Clock final : public t::HostClockPort {
public:
  t::TimePoint current{std::chrono::seconds{1000}};
  t::Outcome<t::TimePoint> now() override {
    return current;
  }
  t::Outcome<std::string> runtime_epoch() override {
    return std::string{"epoch-1"};
  }
  t::Outcome<std::uint64_t> next_connection_generation() override {
    return std::uint64_t{1};
  }
};
} // namespace

int main() {
  Store store;
  Clock clock;
  store.allow = t::HostCertificateBinding{"host-1", "cert-1"};
  t::RuntimeHostApplication app(store, clock);

  t::HostRegistrationCommand command;
  command.certificate_sha256 = "cert-1";
  command.host_id = "host-1";
  command.runtime_epoch = "epoch-1";
  command.capabilities = {"exec"};
  auto registered = app.register_host(command);
  check(std::holds_alternative<t::HostRegistrationResult>(registered), "registration accepted");
  const auto& result = std::get<t::HostRegistrationResult>(registered);
  check(result.connection.fence.connection_generation == 1, "connection generation assigned");
  check(result.connection.online, "new connection online");
  check(result.registration.version.value == 1, "host registration version");

  auto wrong_host = command;
  wrong_host.host_id = "host-2";
  check(rejected_with(app.register_host(wrong_host), t::ErrorCode::PermissionDenied),
        "self-reported host id without binding denied");
  auto unknown_cert = command;
  unknown_cert.certificate_sha256 = "other";
  check(rejected_with(app.register_host(unknown_cert), t::ErrorCode::PermissionDenied),
        "unknown certificate denied");

  auto heartbeat = app.heartbeat(t::HostHeartbeatCommand{result.connection.fence, clock.current});
  check(std::holds_alternative<t::HostHeartbeatResult>(heartbeat), "heartbeat accepted");
  check(std::get<t::HostHeartbeatResult>(heartbeat).heartbeat_interval == 5s, "heartbeat period");

  auto stale = result.connection.fence;
  stale.connection_generation = 999;
  check(rejected_with(app.heartbeat(t::HostHeartbeatCommand{stale, clock.current}),
                      t::ErrorCode::Conflict),
        "stale connection generation rejected");

  auto reconnected = app.register_host(command);
  check(std::holds_alternative<t::HostRegistrationResult>(reconnected),
        "replacement connection accepted");
  const auto& replacement = std::get<t::HostRegistrationResult>(reconnected).connection;
  check(replacement.fence.connection_generation == 2, "new generation replaces old");
  check(
      rejected_with(app.heartbeat(t::HostHeartbeatCommand{result.connection.fence, clock.current}),
                    t::ErrorCode::Conflict),
      "old generation after replacement rejected");

  clock.current += 14s;
  check(std::get<std::size_t>(app.project_offline({replacement.fence.host})) == 0,
        "offline not before 15s");
  clock.current += 1s;
  check(std::get<std::size_t>(app.project_offline({replacement.fence.host})) == 1,
        "offline at 15s");
  auto offline = app.heartbeat(t::HostHeartbeatCommand{replacement.fence, clock.current});
  check(std::holds_alternative<t::HostHeartbeatResult>(offline), "offline host heartbeat accepted");
  check(store.connections["host-1"].online, "heartbeat restores online projection");

  auto future = app.heartbeat(t::HostHeartbeatCommand{replacement.fence, clock.current + 10s});
  check(rejected_with(future, t::ErrorCode::InvalidArgument), "future heartbeat sends rejected");

  std::cout << "P08b runtime host registration, heartbeat, fence and offline projection passed\n";
  return 0;
}
