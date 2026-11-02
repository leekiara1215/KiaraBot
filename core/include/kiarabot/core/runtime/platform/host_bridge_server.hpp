#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "kiarabot/core/application/target/runtime_host_application.hpp"
#include "kiarabot/core/domain/target/host_command_records.hpp"
#include "kiarabot/core/ports/target/runtime_ids.hpp"
#include "kiarabot/v1/host.grpc.pb.h"

namespace kiarabot::core::platform {

// Trusted extraction of the internal-mTLS peer certificate. Production uses
// extract_peer_certificate(ServerContext::auth_context()); tests may inject a fake so the gRPC
// framing/state machine can be exercised without a TLS fixture. The returned value is the
// lowercase SHA-256 of the leaf certificate, never a wire-supplied identity.
using HostPeerCertificateResolver =
    std::function<target::Outcome<std::string>(grpc::ServerContext&)>;

// P08b-3 command/receipt/observation pipelines are implemented through the persistence hooks
// below (P08b-3a); these callbacks remain optional in-process hooks for tests/composition. A null
// hook and null persistence mean transport validation only, never a fabricated durable ack.
struct HostBridgeCallbacks {
  std::function<target::Status(const v1::HostReportInventoryRequest&)> inventory;
  std::function<target::Status(const v1::HostCommandReceipt&)> receipt;
  std::function<target::Status(const v1::HostWatchExecutionsResponse&)> observation;
};

// Durable Runtime-side effects of a Host stream. The application implementation opens its own
// short Runtime UoWs; the transport never commits transactions itself. Null callbacks mean the
// listener is transport-only (used by conformance tests before PG composition), and dispatch then
// sends nothing rather than pretending a command was delivered.
struct HostBridgePersistence {
  std::function<target::Outcome<std::vector<target::HostCommandOutboxRecord>>(
      const std::string&, std::uint64_t, std::size_t)>
      pending;
  std::function<target::Status(const std::string&, std::uint64_t, std::string_view)> dispatched;
  std::function<target::Status(const std::string&, std::uint64_t, bool,
                               std::optional<target::ExecutionOutcome>, std::optional<std::string>)>
      receipt;
  std::function<target::Status(const target::HostObservationRecord&)> observation;
};

struct HostBridgeServerDependencies {
  target::HostRegistrationPort& registration;
  target::HostClockPort& clock;
  HostPeerCertificateResolver peer_certificate;
  HostBridgeCallbacks callbacks;
  HostBridgePersistence persistence;
};

// Synchronous bidirectional HostBridge server for the Runtime internal mTLS listener. The first
// frame must be registration; every later frame must carry the current connection-scoped fence.
// Registration failures use stable gRPC statuses; post-registration frame/fence/command failures
// use RuntimeToHost.failure and end the stream. No bootstrap credential or ConnectionAuthorization
// exists on this internal listener.
target::Outcome<std::unique_ptr<v1::HostBridge::Service>>
make_host_bridge(HostBridgeServerDependencies dependencies);

// Deployment-convenience composition used by runtime_composition until the P08b-2c PG wiring
// lands: the certificate allowlist plus host/connection records are kept in process memory and
// the runtime epoch/connection generation come from the process id port. Registration state is
// therefore NOT durable across Runtime restart; P08b-2c replaces this store with the PG
// HostRegistrationPort implementation.
target::Outcome<std::unique_ptr<v1::HostBridge::Service>> make_deployment_host_bridge(
    target::RuntimeIdPort& ids, std::vector<target::HostCertificateBinding> allowlist,
    HostBridgeCallbacks callbacks = {}, HostBridgePersistence persistence = {});

} // namespace kiarabot::core::platform
