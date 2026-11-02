#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/invariants.hpp"
#include "kiarabot/core/ports/target/host_registration_ports.hpp"

namespace kiarabot::core::target {

// P08b-1 Runtime-side Host registration/liveness rules. The application only owns use-case
// logic; the port contract types live in ports/target/host_registration_ports.hpp. The P08b-2
// server maps internal-mTLS frames to these commands, and the PG adapter implements the ports.
class RuntimeHostApplication {
public:
  RuntimeHostApplication(HostRegistrationPort& store, HostClockPort& clock)
      : store_(store), clock_(clock) {}

  Outcome<HostRegistrationResult> register_host(const HostRegistrationCommand& command) {
    if (command.certificate_sha256.empty() || command.host_id.empty() ||
        command.runtime_epoch.empty()) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto binding = store_.certificate_binding(command.certificate_sha256);
    if (const auto* failure = std::get_if<Failure>(&binding))
      return *failure;
    const auto& bound = std::get<std::optional<HostCertificateBinding>>(binding);
    if (!bound || bound->host_id != command.host_id) {
      return rejected(ErrorCode::PermissionDenied);
    }
    const auto now = clock_.now();
    if (const auto* failure = std::get_if<Failure>(&now))
      return *failure;
    const auto host = ResourceId{command.host_id};
    auto existing = store_.find_host(host);
    if (const auto* failure = std::get_if<Failure>(&existing))
      return *failure;
    HostRegistration registration;
    std::optional<ResourceVersion> expected;
    if (const auto& found = std::get<std::optional<HostRegistration>>(existing)) {
      if (found->state == LifecycleState::Deleting || found->state == LifecycleState::Deleted) {
        return rejected(ErrorCode::PermissionDenied);
      }
      registration = *found;
      expected = registration.version;
      registration.version.value = expected->value + 1;
    } else {
      registration.host = host;
      registration.state = LifecycleState::Active;
      registration.version = ResourceVersion{1};
    }
    registration.capabilities = command.capabilities;
    registration.observed_at = std::get<TimePoint>(now);
    const auto saved = store_.save_host(registration, expected);
    if (const auto* failure = std::get_if<Failure>(&saved))
      return *failure;
    const auto reloaded = store_.find_host(host);
    if (const auto* failure = std::get_if<Failure>(&reloaded))
      return *failure;
    const auto* persisted = std::get_if<std::optional<HostRegistration>>(&reloaded);
    if (!persisted || !*persisted)
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    registration = **persisted;
    const auto generation = clock_.next_connection_generation();
    if (const auto* failure = std::get_if<Failure>(&generation))
      return *failure;
    HostConnectionState connection;
    connection.fence = HostFence{host, std::get<std::uint64_t>(generation), command.runtime_epoch};
    connection.registered_at = std::get<TimePoint>(now);
    connection.last_heartbeat_at = std::get<TimePoint>(now);
    connection.online = true;
    auto activated = store_.activate_connection(connection);
    if (const auto* failure = std::get_if<Failure>(&activated))
      return *failure;
    HostRegistrationResult result{registration, std::get<HostConnectionState>(activated)};
    // Connection generation is the exact fence the Host must present on every later frame.
    if (result.connection.fence.connection_generation == 0 ||
        result.connection.fence.runtime_epoch != command.runtime_epoch) {
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    }
    return result;
  }

  Outcome<HostHeartbeatResult> heartbeat(const HostHeartbeatCommand& command) {
    if (command.fence.connection_generation == 0 || command.fence.runtime_epoch.empty() ||
        command.fence.host.value.empty()) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto current = store_.connection(command.fence.host);
    if (const auto* failure = std::get_if<Failure>(&current))
      return *failure;
    const auto& connection = std::get<std::optional<HostConnectionState>>(current);
    if (!connection)
      return rejected(ErrorCode::Unauthenticated);
    // A newer connection replaces the old one; frames from the replaced generation are stale.
    if (connection->fence != command.fence)
      return rejected(ErrorCode::Conflict);
    const auto now = clock_.now();
    if (const auto* failure = std::get_if<Failure>(&now))
      return *failure;
    if (command.sent_at > std::get<TimePoint>(now)) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto updated = *connection;
    updated.last_heartbeat_at = std::get<TimePoint>(now);
    updated.online = true;
    const auto saved = store_.save_connection(updated, connection->fence.connection_generation);
    if (const auto* failure = std::get_if<Failure>(&saved))
      return *failure;
    return HostHeartbeatResult{updated.fence, std::get<TimePoint>(now), kHostHeartbeatInterval,
                               command.sent_at < connection->last_heartbeat_at};
  }

  // Liveness sweep: after 15s without a heartbeat mark online=false only. There is deliberately
  // no process-outcome transition here: Host offline does not prove a process stopped.
  Outcome<std::size_t> project_offline(const std::vector<ResourceId>& hosts) {
    auto now = clock_.now();
    if (const auto* failure = std::get_if<Failure>(&now))
      return *failure;
    std::size_t changed = 0;
    for (const auto& host : hosts) {
      auto current = store_.connection(host);
      if (const auto* failure = std::get_if<Failure>(&current))
        return *failure;
      const auto& connection = std::get<std::optional<HostConnectionState>>(current);
      if (!connection || !connection->online)
        continue;
      if (std::get<TimePoint>(now) - connection->last_heartbeat_at < kHostOfflineAfter)
        continue;
      auto updated = *connection;
      updated.online = false;
      const auto status = store_.save_connection(updated, connection->fence.connection_generation);
      if (const auto* failure = std::get_if<Failure>(&status))
        return *failure;
      ++changed;
    }
    return changed;
  }

private:
  HostRegistrationPort& store_;
  HostClockPort& clock_;
};

// A managed instance may only be started on a host whose connection is current and online; the
// later P08b-3 command outbox consumes this gate before writing an apply command.
inline Status host_command_admitted(const HostConnectionState& connection,
                                    const HostFence& presented) {
  if (connection.fence != presented)
    return rejected(ErrorCode::Conflict);
  if (!connection.online)
    return rejected(ErrorCode::UpstreamUnavailable);
  return std::monostate{};
}
} // namespace kiarabot::core::target
