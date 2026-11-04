#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kiarabot/core/domain/target/invariants.hpp"
#include "kiarabot/core/ports/target/host_registration_ports.hpp"
#include "kiarabot/core/ports/target/unit_of_work.hpp"

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

// HOST-03 Runtime-owned dispatch gate commands. The certificate fingerprint and live fence are
// trusted boundary observations (actual mTLS leaf / accepted HostBridge registration), never wire
// self-report. Identity online authorization happens before the local durable gate transaction.
struct HostAdmissionGateCommand {
  std::string certificate_sha256;
  ResourceId host;
  HostFence fence;
  OperationRef operation;
  std::uint64_t maintenance_version = 0;
  std::optional<std::uint64_t> expected_gate_version;
  bool admission_open = false;
  TimePoint deadline;
};
struct HostAdmissionGateResult {
  HostAdmissionGateRecord gate;
};
// Narrow v1.1 Runtime -> Identity check: only the current Host leaf and exact RESOURCE_HOST are
// inspected; the response is a current decision/version, never a reusable ticket.
class HostGateAuthorizationPort {
public:
  virtual ~HostGateAuthorizationPort() = default;
  virtual Outcome<ResourceVersion>
  check(const ResourceRef& host, const Sha256Digest& host_certificate, TimePoint deadline) = 0;
};
inline Outcome<Sha256Digest> digest_from_hex(std::string_view value) {
  if (value.size() != 64) {
    return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
  }
  Sha256Digest digest{};
  for (std::size_t index = 0; index < 32; ++index) {
    const auto nibble = [](char character) -> int {
      if (character >= '0' && character <= '9') {
        return character - '0';
      }
      if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
      }
      return -1;
    };
    const int high = nibble(value[index * 2]);
    const int low = nibble(value[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    digest.bytes[index] = static_cast<std::byte>((high << 4) | low);
  }
  return digest;
}
// Persists the Runtime-owned gate inside one owner UoW after re-validating the trusted leaf,
// the live accepted fence and the current Identity decision. Equal intent replays return the same
// gate; a mismatched expected version is CONFLICT and is never a fake "admission closed" success.
class RuntimeHostGateApplication {
public:
  RuntimeHostGateApplication(
      std::function<Outcome<std::unique_ptr<RuntimeUnitOfWork>>()> transactions,
      HostFenceRegistry& fences, HostGateAuthorizationPort& authorization, HostClockPort& clock,
      std::vector<HostCertificateBinding> certificate_allowlist)
      : transactions_(std::move(transactions)), fences_(fences), authorization_(authorization),
        clock_(clock), allowlist_(std::move(certificate_allowlist)) {}

  Outcome<HostAdmissionGateResult> set_admission_gate(const HostAdmissionGateCommand& command) {
    if (command.host.value.empty() || command.fence.host != command.host ||
        command.fence.connection_generation == 0 || command.fence.runtime_epoch.empty() ||
        command.operation.owner != Service::Host || command.operation.id.value.empty() ||
        command.operation.resource != ResourceRef{ResourceKind::Host, command.host} ||
        command.maintenance_version == 0 ||
        (command.expected_gate_version && *command.expected_gate_version == 0)) {
      return rejected(ErrorCode::InvalidArgument, Recovery::CorrectInput);
    }
    auto bound = std::find_if(allowlist_.begin(), allowlist_.end(), [&](const auto& entry) {
      return entry.host_id == command.host.value &&
             entry.certificate_sha256 == command.certificate_sha256;
    });
    if (bound == allowlist_.end()) {
      return rejected(ErrorCode::Unauthenticated, Recovery::Authenticate);
    }
    auto live = fences_.current(command.host);
    if (const auto* failure = std::get_if<Failure>(&live)) {
      return *failure;
    }
    const auto& fence = std::get<std::optional<HostFence>>(live);
    if (!fence || *fence != command.fence) {
      return rejected(ErrorCode::Unauthenticated, Recovery::Authenticate);
    }
    const auto now = clock_.now();
    if (const auto* failure = std::get_if<Failure>(&now)) {
      return *failure;
    }
    if (command.deadline <= std::get<TimePoint>(now)) {
      return rejected(ErrorCode::Timeout, Recovery::Backoff);
    }
    auto digest = digest_from_hex(command.certificate_sha256);
    if (const auto* failure = std::get_if<Failure>(&digest)) {
      return *failure;
    }
    auto authorized = authorization_.check(ResourceRef{ResourceKind::Host, command.host},
                                           std::get<Sha256Digest>(digest), command.deadline);
    if (const auto* failure = std::get_if<Failure>(&authorized)) {
      return *failure;
    }
    if (std::get<ResourceVersion>(authorized).value == 0) {
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    }
    HostAdmissionGateRecord record{
        command.host,           command.operation,       command.maintenance_version, 0,
        command.admission_open, std::get<TimePoint>(now)};
    auto work = transactions_();
    if (const auto* failure = std::get_if<Failure>(&work)) {
      return *failure;
    }
    auto unit = std::get<std::unique_ptr<RuntimeUnitOfWork>>(std::move(work));
    if (!unit) {
      return rejected(ErrorCode::Internal, Recovery::Diagnose);
    }
    auto saved = unit->host_admission_gates().save(record, command.expected_gate_version);
    if (const auto* failure = std::get_if<Failure>(&saved)) {
      return *failure;
    }
    const auto committed = unit->commit();
    switch (committed) {
    case CommitResult::Committed:
      return HostAdmissionGateResult{std::get<HostAdmissionGateRecord>(saved)};
    case CommitResult::RolledBack:
      return rejected(ErrorCode::UpstreamUnavailable, Recovery::Backoff);
    default:
      return Failure{ErrorCode::UpstreamUnavailable, Acceptance::Unknown,
                     Recovery::QuerySameIdentity, std::nullopt, std::nullopt};
    }
  }

private:
  std::function<Outcome<std::unique_ptr<RuntimeUnitOfWork>>()> transactions_;
  HostFenceRegistry& fences_;
  HostGateAuthorizationPort& authorization_;
  HostClockPort& clock_;
  std::vector<HostCertificateBinding> allowlist_;
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
